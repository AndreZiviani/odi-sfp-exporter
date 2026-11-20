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

/* After the response the connection is closed in stages (RFC 9112, 9.6):
 * shutdown(SHUT_WR) sends the FIN, then whatever the client still sends
 * is read and dropped until it closes too. Closing with request bytes
 * unread makes Linux answer with an RST instead of a FIN, and the RST
 * throws away what is still queued for the client: a request longer than
 * the one read in serve() takes, or one sent in two segments, ended the
 * scrape in "connection reset by peer", which Prometheus counts as a
 * failed scrape. A client that closes on EOF (Prometheus, curl, a browser
 * with Connection: close) costs one round trip here. One that stays open
 * is left after LINGER_MS of silence, and one that keeps sending after
 * LINGER_MAX_MS in all: what has arrived by then is read, however much.
 * Without a clock LINGER_MAX_MS is a budget of waits, each charged in full,
 * and each wake reads what is already there, up to LINGER_ROUND_MAX. */
#define LINGER_MS        500
#define LINGER_MAX_MS    2000
#define LINGER_ROUND_MAX 65536

static void hang_up(int conn)
{
	char buf[1024];
	struct pollfd pfd;
	unsigned long t0 = 0;
	long budget = LINGER_MAX_MS;
	int clock = mono_ms(&t0);

	/* Cut off (write timeout or deadline): close at once, as before. */
	if (conn == write_failed_fd)
		return;
	if (syscall3(__NR_shutdown, conn, SHUT_WR, 0) < 0)
		return;
	pfd.fd = conn;
	pfd.events = POLLIN;
	for (;;) {
		long wait = deadline_wait(clock, t0, LINGER_MAX_MS, &budget, LINGER_MS);
		unsigned long round = 0;

		pfd.revents = 0;
		if (!wait || syscall3(__NR_poll, (long)&pfd, 1, wait) <= 0)
			return;
		/* This read, and whatever else is there already. */
		do {
			long n = syscall3(__NR_read, conn, (long)buf, sizeof(buf));

			if (n <= 0)
				return;		/* the client closed, or the socket failed */
			round += (unsigned long)n;
			pfd.revents = 0;
		} while (round < LINGER_ROUND_MAX &&
			 syscall3(__NR_poll, (long)&pfd, 1, 0) > 0);
	}
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
	 * One read takes a request of up to 511 bytes that has arrived whole;
	 * hang_up() drops the rest, if any, before the close. The poll above
	 * means this read does not block. */
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
	hang_up(conn);
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

	for (;;) {
		conn = syscall3(__NR_accept, fd, 0, 0);
		if (conn < 0) {
			/* EINTR and friends: keep serving. A lasting error
			 * (EMFILE, ENOMEM) leaves the connection queued, so
			 * accept fails again at once: wait a little first,
			 * or the loop spins a CPU the stick does not have. */
			syscall3(__NR_poll, 0, 0, 100);
			continue;
		}

		serve((int)conn);
		syscall3(__NR_close, conn, 0, 0);
		/* The number goes back to the pool; a later file may get it. */
		write_failed_fd = -1;
		write_deadline_fd = -1;
	}
}
