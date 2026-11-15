/*
 * Prometheus exporter for the ODI DFP-34X-2C2, as a standalone HTTP server.
 *
 * Realtek's boa cannot exec external CGI (it 404s anything typed
 * application/x-httpd-cgi, while serving the same file happily as a static
 * download), so the exporter has to carry its own listener. That turns out to
 * be about forty lines on top of the syscall layer.
 *
 * Still freestanding: no libc, so nothing has to respect the -march=mips2
 * ceiling on our behalf. Single-threaded and serial — Prometheus makes one
 * request per scrape, and a fork per connection would need a wait/reap loop for
 * no benefit here.
 *
 * MIPS socket ABI traps, all silent if wrong and all verified against the
 * mips-linux-gnu headers: SOCK_STREAM is 2 (not 1 — MIPS swaps it with
 * SOCK_DGRAM), and SOL_SOCKET is 65535 (not 1). See syscall.h.
 *
 * This CPU is big-endian, so host order already is network order: there is no
 * htons or htonl anywhere in this file, deliberately.
 */

#include "metrics_body.h"
#include "history.h"

#define DEFAULT_PORT 9100
#define BACKLOG      8

/* The listener is serial, so one client holds every scrape behind it. A
 * client that connects and sends nothing gets REQUEST_WAIT_MS to start its
 * request and is then closed unanswered; Prometheus sends its GET at once.
 * A client that stops reading the response is cut off after SEND_WAIT_S on
 * one write, and one that reads only a trickle once the response is not
 * done SEND_DEADLINE_S after the request came in (write_all() then skips
 * the rest of that scrape). A scrape takes a few seconds. */
#define REQUEST_WAIT_MS 5000
#define SEND_WAIT_S     10
#define SEND_DEADLINE_S 30

#define __NR_time   4013
#define __NR_unlink 4010
#define __NR_rename 4038
#define __NR_fcntl  4055
#define __NR_flock  4143

#define F_GETFL     3
#define F_SETFL     4
#define O_NONBLOCK  0x0080	/* MIPS; the generic ABI says 04000 */
#define LOCK_EX     2
#define LOCK_NB     4
#define EAGAIN      11

#define HIST_FILE "/var/run/history"
#define HIST_TMP  "/var/run/history.tmp"
#define HIST_LOCK "/var/run/history.lock"

/*
 * The minute history (src/history.h): metricsd already asks diag these
 * questions for a scrape, so it also asks them once a minute for the ring
 * the web UI and the diagnostics bundle read. One process, one diag at a
 * time: a sample and a scrape never run diag side by side.
 *
 * Timed by /proc/uptime, not the wall clock, which jumps when NTP sets it.
 *
 * One writer: a second metricsd (a hand-started copy on another port)
 * would keep a ring of its own and rename over the first one's file. The
 * writer holds an flock on HIST_LOCK for as long as it runs; a copy that
 * cannot take it serves scrapes and tries again at the next minute, so it
 * takes over when the first one is gone.
 */
static struct hist_ring hist;
static int hist_owner;

static long uptime_s(void)
{
	char b[64];
	long v = 0;
	int i;

	if (read_file("/proc/uptime", b, sizeof(b)) <= 0)
		return -1;
	for (i = 0; b[i] >= '0' && b[i] <= '9'; i++)
		v = v * 10 + (b[i] - '0');
	return i ? v : -1;
}

/* The file as a ring: read in pieces, a line at a time, so the ring
 * itself is the only copy of it in memory. */
static void hist_load(void)
{
	char buf[1024], part[HIST_LINE];
	unsigned long pl = 0;
	int cut = 0;
	long fd = syscall3(__NR_open, (long)HIST_FILE, O_RDONLY, 0);
	long got, i;

	if (fd < 0)
		return;
	while ((got = syscall3(__NR_read, fd, (long)buf, sizeof(buf))) > 0) {
		for (i = 0; i < got; i++) {
			if (buf[i] != '\n') {
				if (pl + 1 < sizeof(part))
					part[pl++] = buf[i];
				else
					cut = 1;
				continue;
			}
			part[pl] = 0;
			if (!cut)
				hist_ring_load(&hist, part);
			pl = 0;
			cut = 0;
		}
	}
	syscall3(__NR_close, fd, 0, 0);
	part[pl] = 0;		/* a last line without its newline, at EOF only */
	if (got == 0 && pl && !cut)
		hist_ring_load(&hist, part);
}

/* Whether this process writes the ring: 1 when it holds the lock, 2 when
 * it took it just now (and loaded the ring it found), 0 when another
 * metricsd holds it. */
static int hist_own(void)
{
	long fd;

	if (hist_owner)
		return 1;
	fd = syscall3(__NR_open, (long)HIST_LOCK, O_WRONLY | O_CREAT, 0644);
	if (fd < 0)
		return 0;
	if (syscall3(__NR_flock, fd, LOCK_EX | LOCK_NB, 0) < 0) {
		syscall3(__NR_close, fd, 0, 0);
		return 0;
	}
	hist_owner = 1;		/* fd stays open: the lock lasts the process */
	hist.n = 0;
	hist.first = 0;
	hist_load();
	return 2;
}

/* The ring to HIST_TMP, then renamed over HIST_FILE, so a reader gets
 * the old ring or the new one, never half of each. A failed write leaves
 * the old file alone. */
static void hist_write(void)
{
	char buf[2048];
	unsigned long n = 0, k, len;
	long fd = syscall3(__NR_open, (long)HIST_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	int ok = 1;

	if (fd < 0)
		return;
	/* write_all() refuses the descriptor of its last failed write; a
	 * failed history write is no reason to refuse the next one. */
	write_failed_fd = -1;
	for (k = 0; ok && k < hist.n; k++) {
		const char *l = hist_ring_line(&hist, k);

		len = str_len(l);
		if (n + len + 1 > sizeof(buf)) {
			ok = write_all((int)fd, buf, n) == (long)n;
			n = 0;
		}
		for (; *l; l++)
			buf[n++] = *l;
		buf[n++] = '\n';
	}
	if (ok && n)
		ok = write_all((int)fd, buf, n) == (long)n;
	syscall3(__NR_close, fd, 0, 0);
	write_failed_fd = -1;
	if (!ok || syscall3(__NR_rename, (long)HIST_TMP, (long)HIST_FILE, 0) < 0)
		syscall3(__NR_unlink, (long)HIST_TMP, 0, 0);
}

static void hist_sample(void)
{
	static char *const argv[] = { "diag", 0 };
	static char diag[16384];
	static char status[4096], counters[2048], stat[2048], meminfo[2048], up[64];
	char line[HIST_LINE];
	struct hist_in in;
	unsigned long n;

	in.diag = run_script_to_buf(DIAG_PATH, argv, HIST_DIAG_SCRIPT, diag,
				    sizeof(diag), DIAG_TIMEOUT_MS) > 0 ? diag : 0;
	in.gpon_status = read_file("/proc/odi_gpon/status", status, sizeof(status)) > 0 ? status : 0;
	in.gpon_counters = read_file("/proc/odi_gpon/counters", counters, sizeof(counters)) > 0 ? counters : 0;
	in.stat = read_file("/proc/stat", stat, sizeof(stat)) > 0 ? stat : 0;
	in.meminfo = read_file("/proc/meminfo", meminfo, sizeof(meminfo)) > 0 ? meminfo : 0;
	in.uptime = read_file("/proc/uptime", up, sizeof(up)) > 0 ? up : 0;
	in.wall = syscall3(__NR_time, 0, 0, 0);

	n = hist_build_line(&in, line, sizeof(line));
	if (!n)
		return;
	hist_ring_push(&hist, line, n);
	hist_write();
}

/* How long to wait for the next sample, in ms; takes the sample when it
 * is due. *due is the uptime of the next one, -1 until /proc/uptime has
 * answered. */
static long hist_tick(long *due)
{
	long now = uptime_s();

	if (now < 0)
		return HIST_INTERVAL_S * 1000;	/* try again in a minute */
	if (*due < 0)
		*due = now;
	if (now >= *due) {
		int own = hist_own();

		if (own == 2) {
			/* Taken over at start or from a metricsd that is gone:
			 * the next sample a minute after the newest one found,
			 * not at once, so a respawn loop does not fill the
			 * ring with samples seconds apart. */
			long last = hist_ring_last_up(&hist);

			if (last >= 0 && now >= last && now - last < HIST_INTERVAL_S) {
				*due = last + HIST_INTERVAL_S;
				return (*due - now) * 1000;
			}
		}
		if (own) {
			hist_sample();
			/* The sample took a moment, and a long one (diag
			 * timing out) can leave the schedule behind. */
			long after = uptime_s();

			if (after >= now)
				now = after;
		}
		*due += HIST_INTERVAL_S;
		if (*due <= now)
			*due = now + HIST_INTERVAL_S;
	}
	return (*due - now) * 1000;
}

static unsigned long parse_u16(const char *s, unsigned long fallback)
{
	unsigned long v = 0;
	unsigned long i = 0;

	if (!s || !s[0])
		return fallback;

	for (i = 0; s[i]; i++) {
		if (s[i] < '0' || s[i] > '9')
			return fallback;
		v = v * 10 + (unsigned long)(s[i] - '0');
		if (v > 65535)
			return fallback;
	}
	return v ? v : fallback;
}

static void serve(int conn)
{
	char req[512];
	struct pollfd pfd;
	long tv[2] = { SEND_WAIT_S, 0 };   /* struct timeval on o32 */
	long now;

	write_failed_fd = -1;
	pfd.fd = conn;
	pfd.events = POLLIN;
	pfd.revents = 0;
	/* Only an actual request is answered: poll also returns for a client
	 * that hung up or errored, and then the read gives 0 or an error. */
	if (syscall3(__NR_poll, (long)&pfd, 1, REQUEST_WAIT_MS) <= 0 ||
	    !(pfd.revents & POLLIN))
		return;
	__syscall6(__NR_setsockopt, conn, SOL_SOCKET, SO_SNDTIMEO,
		   (long)tv, sizeof(tv), 0);

	/* Read the request and discard it: we answer the same on every path.
	 * Draining it matters anyway — replying to an unread request invites the
	 * client's close to arrive as an RST that discards our response. The
	 * poll above means this read does not block. */
	if (syscall3(__NR_read, conn, (long)req, sizeof(req) - 1) <= 0)
		return;

	now = mono_s();
	if (now >= 0) {
		write_deadline_fd = conn;
		write_deadline_s = now + SEND_DEADLINE_S;
	}

	put_fd(conn,
	       "HTTP/1.0 200 OK\r\n"
	       "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
	       "Connection: close\r\n"
	       "\r\n");
	emit_metrics(conn);
}

int main(int argc, char **argv)
{
	unsigned long port = parse_u16(argc > 1 ? argv[1] : 0, DEFAULT_PORT);

	/*
	 * Ignore SIGPIPE, or a client that hangs up mid-response kills the
	 * exporter. Writing to a socket whose peer has closed raises it and the
	 * default action is to terminate; with no libc nothing installs a
	 * handler for us.
	 *
	 * Prometheus reads a scrape to the end, which is why this never showed
	 * in months of running. It is still one interrupted `curl :9100` away,
	 * and it took the sibling confd daemon down repeatedly once a browser
	 * was pointed at it. Two lines to remove the whole class.
	 */
	sig_ignore(SIGPIPE);

	/* Reported as the `path` label on gpon_exporter_build_info, so a scrape
	 * says whether this is the image's binary or an /etc/config override. */
	if (argc > 0 && argv[0] && argv[0][0])
		exporter_path = argv[0];

	long one = 1;
	long fd, conn;

	/* struct sockaddr_in, laid out by hand to avoid assuming a header we do
	 * not include. sin_family is host order; sin_port is network order, which
	 * on this big-endian CPU is the same thing. */
	unsigned char addr[16];
	unsigned long i;

	for (i = 0; i < sizeof(addr); i++)
		addr[i] = 0;

	addr[0] = 0;                                    /* sin_family, high byte */
	addr[1] = AF_INET;                              /* sin_family, low byte  */
	addr[2] = (unsigned char)((port >> 8) & 0xff);  /* sin_port              */
	addr[3] = (unsigned char)(port & 0xff);
	/* addr[4..7] stay zero: sin_addr = INADDR_ANY */

	fd = syscall3(__NR_socket, AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		put("socket() failed\n");
		return 1;
	}

	/* Without SO_REUSEADDR a restart inside the TIME_WAIT window fails to
	 * bind, which looks exactly like the port already being taken. */
	__syscall6(__NR_setsockopt, fd, SOL_SOCKET, SO_REUSEADDR,
		   (long)&one, sizeof(one), 0);

	if (syscall3(__NR_bind, fd, (long)addr, sizeof(addr)) < 0) {
		put("bind() failed -- port already in use?\n");
		return 1;
	}

	if (syscall3(__NR_listen, fd, BACKLOG, 0) < 0) {
		put("listen() failed\n");
		return 1;
	}

	/*
	 * Non-blocking: poll() saying the socket is readable does not promise
	 * accept() a connection (the client may have gone in between), and a
	 * blocking accept() would then hold up the history until the next
	 * client came.
	 */
	syscall3(__NR_fcntl, fd, F_SETFL, syscall3(__NR_fcntl, fd, F_GETFL, 0) | O_NONBLOCK);

	/* The first tick takes the history over: a fresh boot samples at
	 * once, a restart carries on the ring a minute after its newest
	 * sample (hist_tick). */
	long due = -1;

	for (;;) {
		struct pollfd pfd;
		long wait_ms = hist_tick(&due);

		pfd.fd = (int)fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (syscall3(__NR_poll, (long)&pfd, 1, wait_ms) <= 0 || !(pfd.revents & POLLIN))
			continue;	/* the minute is up, or EINTR */

		conn = syscall3(__NR_accept, fd, 0, 0);
		if (conn == -EAGAIN)
			continue;	/* the client went away again */
		if (conn < 0) {
			/* EINTR and friends: keep serving. A lasting error
			 * (EMFILE, ENOMEM) leaves the connection queued, so
			 * poll says ready again at once: wait a little first,
			 * or the loop spins a CPU the stick does not have. */
			syscall3(__NR_poll, 0, 0, 100);
			continue;
		}

		serve((int)conn);
		syscall3(__NR_close, conn, 0, 0);
		/* write_all() refuses a descriptor whose write failed or ran
		 * past its deadline; the number is free again now, and the
		 * next open() of the history file is likely to get it. */
		write_failed_fd = -1;
		write_deadline_fd = -1;
	}
}
