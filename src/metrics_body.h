/*
 * The metrics themselves, shared by both transports: metricsd.c (standalone
 * HTTP server — what this stick needs) and metrics.c (CGI, for devices whose
 * web server can exec one).
 *
 * Everything writes to an explicit fd so the same code serves stdout and a
 * socket.
 *
 * The optical values are not in /proc — `ls /proc` turns up only rtk_smux — so
 * they come from /bin/diag, run once per scrape and scraped back out of its
 * output. See docs/METRICS.md for the command list and how it was established.
 */

#ifndef ODI_METRICS_BODY_H
#define ODI_METRICS_BODY_H

#include "syscall.h"
#include "wrap.h"
#include "resetinfo.h"
#include "confighash.h"
#include "slot_state.h"

#define DIAG_PATH "/bin/diag"

/*
 * Bounds for every child this exporter forks, in milliseconds. Both go
 * through drain_bounded() (syscall.h): past this budget the child is
 * SIGKILLed and reaped rather than left to hang the single-threaded HTTP
 * server (found on hardware, rc3, claro, 2026-09-28: a stuck omcid left
 * metricsd parked in read() with 9100 not accepting new connections).
 *
 * OMCICLI_TIMEOUT_MS: the exporter's own requirement -- `omcicli dump
 * srvflow` must answer within 2 s of a respawn (odi-oss qemu-test.sh
 * exercises exactly this bound against a real omcid).
 *
 * DIAG_TIMEOUT_MS: diag costs ~32-48 ms per scrape even for the heaviest
 * script measured (run_script_to_buf's comment), so 3 s is over 60x
 * headroom -- generous on purpose, since a diag that is merely slow must
 * still be allowed to finish, only one that is actually wedged should be
 * killed.
 */
#define OMCICLI_TIMEOUT_MS 2000
#define DIAG_TIMEOUT_MS    3000
/*
 * MD5SUM_TIMEOUT_MS: md5sum over the whole config partition is at most
 * 240 KB of input, tens of milliseconds on this CPU, and it runs only when a
 * file changed (metric_config), so 2 s is a bound for a wedged read of the
 * flash, not for a slow hash.
 */
#define MD5SUM_TIMEOUT_MS  2000

/*
 * Build identity, reported as gpon_exporter_build_info.
 *
 * This matters more here than it looks. The exporter can be replaced WITHOUT
 * reflashing: rc35 prefers /etc/config/metricsd — on the jffs2 config
 * partition, which fwu.sh does not touch — over the /bin/metricsd baked into
 * the image. So an override survives both reboots and reflashes, and can end up
 * older than the image it is running on with nothing to say so. Exporting the
 * version AND the path it was started from makes that visible from a query
 * instead of an inspection.
 *
 * BUILD_ID comes from `git describe` on the host at build time; see the
 * Makefile.
 */
#ifndef BUILD_ID
#define BUILD_ID "unknown"
#endif

/* argv[0] as invoked, set by main(). Not a copy: argv lives for the life of the
 * process. */
static const char *exporter_path = "unknown";

/*
 * Values are emitted as the literal text diag printed. No parsing to a number
 * and back: Prometheus wants a bare decimal and diag already produces one, so
 * this avoids float formatting, and with it soft-float and a libc. It also
 * cannot introduce a rounding difference between what the device reports and
 * what is scraped.
 */
static int scan_decimal(const char *s, unsigned long *start, unsigned long *len)
{
	unsigned long i;

	for (i = 0; s[i]; i++) {
		unsigned long j = i;
		unsigned long int_start, frac_start;

		if (s[j] == '-')
			j++;

		int_start = j;
		while (s[j] >= '0' && s[j] <= '9')
			j++;
		if (j == int_start || s[j] != '.')
			continue;

		j++;
		frac_start = j;
		while (s[j] >= '0' && s[j] <= '9')
			j++;
		if (j == frac_start)
			continue;

		*start = i;
		*len = j - i;
		return 1;
	}
	return 0;
}

/* `diag gpon get onu-state` prints e.g. "ONU state: operation state(O5)". */
static int scan_onu_state(const char *s, unsigned long *start, unsigned long *len)
{
	unsigned long i;

	for (i = 0; s[i]; i++) {
		unsigned long j;

		if (s[i] != '(' || s[i + 1] != 'O')
			continue;

		j = i + 2;
		*start = j;
		while (s[j] >= '0' && s[j] <= '9')
			j++;
		if (j == *start)
			continue;

		*len = j - *start;
		return 1;
	}
	return 0;
}

static void emit_header(int fd, const char *name, const char *help, const char *type)
{
	put_fd(fd, "# HELP ");
	put_fd(fd, name);
	put_fd(fd, " ");
	put_fd(fd, help);
	put_fd(fd, "\n# TYPE ");
	put_fd(fd, name);
	put_fd(fd, " ");
	put_fd(fd, type);
	put_fd(fd, "\n");
}

/* Normalise a label in place: lowercase, spaces to underscores. Written into a
 * caller buffer so the whole label goes out in one write rather than one
 * syscall per character. */
static unsigned long norm_label(char *dst, unsigned long cap,
				const char *src, unsigned long len)
{
	unsigned long n = 0, k;

	for (k = 0; k < len && n + 1 < cap; k++) {
		char c = src[k];

		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		else if (c == ' ' || c == '-')
			c = '_';
		dst[n++] = c;
	}
	dst[n] = 0;
	return n;
}

/*
 * A single-valued metric: one extractor applied to one command's section of the
 * diag output. On a parse failure nothing is emitted at all — an absent series
 * is honest, whereas a zero would look like a real reading of zero dBm.
 */
static void emit_scalar(int fd, const char *name, const char *help,
			const char *sec,
			int (*scan)(const char *, unsigned long *, unsigned long *))
{
	unsigned long start = 0, len = 0;

	if (!scan(sec, &start, &len))
		return;

	emit_header(fd, name, help, "gauge");
	put_fd(fd, name);
	put_fd(fd, " ");
	write_all(fd, sec + start, len);
	put_fd(fd, "\n");
}

static void put_first_token(int fd, const char *s)
{
	unsigned long i = 0;

	while (s[i] && s[i] != ' ' && s[i] != '\n' && s[i] != '\t')
		i++;

	if (i == 0) {
		put_fd(fd, "0");
		return;
	}
	write_all(fd, s, i);
}

/* Offset just past `key` if the line starting at i begins with it, else 0. */
static unsigned long line_key(const char *buf, unsigned long i, const char *key)
{
	unsigned long k = 0;

	while (key[k] && buf[i + k] == key[k])
		k++;
	return key[k] ? 0 : i + k;
}

/*
 * /proc/meminfo on a device with ~64 MB is the metric most likely to explain a
 * reboot, and it costs no fork. Values are in kB; multiplying to bytes keeps
 * Prometheus base units and stays in integer arithmetic.
 */
static void metric_meminfo(int fd)
{
	static const char *const keys[] = {
		"MemTotal:", "MemFree:", "Buffers:", "Cached:", 0
	};
	static const char *const names[] = {
		"total", "free", "buffers", "cached", 0
	};
	char buf[2048];
	unsigned long i;
	int k, have_header = 0;

	if (read_file("/proc/meminfo", buf, sizeof(buf)) <= 0)
		return;

	for (k = 0; keys[k]; k++) {
		for (i = 0; buf[i]; i++) {
			unsigned long v, start, val = 0;

			if (i && buf[i - 1] != '\n')
				continue;
			v = line_key(buf, i, keys[k]);
			if (!v)
				continue;

			while (buf[v] == ' ')
				v++;
			start = v;
			while (buf[v] >= '0' && buf[v] <= '9') {
				val = val * 10 + (unsigned long)(buf[v] - '0');
				v++;
			}
			if (v == start)
				break;

			if (!have_header) {
				emit_header(fd, "gpon_memory_bytes",
					    "Kernel memory, from /proc/meminfo.", "gauge");
				have_header = 1;
			}
			put_fd(fd, "gpon_memory_bytes{kind=\"");
			put_fd(fd, names[k]);
			put_fd(fd, "\"} ");
			put_u32_fd(fd, val * 1024);
			put_fd(fd, "\n");
			break;
		}
	}
}

/*
 * /proc/net/dev. Unlike the ONU MAC registers these are ordinary Linux
 * counters: monotonic, and reading does not clear them. Field order is fixed —
 * rx: bytes packets errs drop ...  tx: bytes packets errs drop ...
 */
static void metric_netdev(int fd)
{
	static const unsigned long FIELD[] = { 0, 1, 2, 3, 8, 9, 10, 11 };
	static const char *const METRIC[] = {
		"gpon_network_receive_bytes_total",
		"gpon_network_receive_packets_total",
		"gpon_network_receive_errs_total",
		"gpon_network_receive_drop_total",
		"gpon_network_transmit_bytes_total",
		"gpon_network_transmit_packets_total",
		"gpon_network_transmit_errs_total",
		"gpon_network_transmit_drop_total",
	};
	char buf[4096];
	char dev[32];
	int m;

	if (read_file("/proc/net/dev", buf, sizeof(buf)) <= 0)
		return;

	/*
	 * Metric-major: one pass per metric over the whole buffer. The exposition
	 * format requires every sample of a metric family to be contiguous, and
	 * the natural interface-major loop scatters them. Re-scanning an in-memory
	 * buffer eight times costs nothing.
	 */
	for (m = 0; m < 8; m++) {
		unsigned long i = 0;
		int line = 0, header_done = 0;

		while (buf[i]) {
			unsigned long ls = i, le = i, c, a, b, n, f, pos;

			while (buf[le] && buf[le] != '\n')
				le++;

			if (line++ < 2)		/* two header lines */
				goto next;

			c = ls;
			while (c < le && buf[c] != ':')
				c++;
			if (c == le)
				goto next;

			a = ls;
			while (a < c && buf[a] == ' ')
				a++;
			b = c;
			if (b == a)
				goto next;
			n = norm_label(dev, sizeof(dev), buf + a, b - a);

			pos = c + 1;
			for (f = 0; f < 16 && pos < le; f++) {
				unsigned long start;

				while (pos < le && buf[pos] == ' ')
					pos++;
				start = pos;
				while (pos < le && buf[pos] >= '0' && buf[pos] <= '9')
					pos++;
				if (pos == start)
					break;
				if (f != FIELD[m])
					continue;

				if (!header_done) {
					emit_header(fd, METRIC[m],
						    "Interface counter from /proc/net/dev.",
						    "counter");
					header_done = 1;
				}
				put_fd(fd, METRIC[m]);
				put_fd(fd, "{device=\"");
				write_all(fd, dev, n);
				put_fd(fd, "\"} ");
				write_all(fd, buf + start, pos - start);
				put_fd(fd, "\n");
			}

next:
			i = (buf[le] == '\n') ? le + 1 : le;
		}
	}
}

/* /proc/loadavg: "0.00 0.01 0.05 1/45 1234" — three decimals, emitted verbatim. */
static void metric_loadavg(int fd)
{
	static const char *const NAMES[] = { "gpon_load1", "gpon_load5", "gpon_load15" };
	char buf[128];
	unsigned long i = 0;
	int k;

	if (read_file("/proc/loadavg", buf, sizeof(buf)) <= 0)
		return;

	for (k = 0; k < 3; k++) {
		unsigned long start;

		while (buf[i] == ' ')
			i++;
		start = i;
		while ((buf[i] >= '0' && buf[i] <= '9') || buf[i] == '.')
			i++;
		if (i == start)
			return;

		emit_header(fd, NAMES[k], "Load average, from /proc/loadavg.", "gauge");
		put_fd(fd, NAMES[k]);
		put_fd(fd, " ");
		write_all(fd, buf + start, i - start);
		put_fd(fd, "\n");
	}
}

static void metric_uptime(int fd)
{
	char buf[64];

	if (read_file("/proc/uptime", buf, sizeof(buf)) <= 0)
		return;

	emit_header(fd, "gpon_uptime_seconds", "Time since the stick booted.", "gauge");
	put_fd(fd, "gpon_uptime_seconds ");
	put_first_token(fd, buf);
	put_fd(fd, "\n");
}

/*
 * `diag gpon get alarm-status` prints one line per alarm:
 *
 *     Alarm LOS, status: clear
 *     Alarm TX Too Long, status: clear
 *
 * Seven of them, so this is one labelled metric rather than seven names. Alarm
 * names are normalised to lowercase with underscores: "TX Too Long" ->
 * tx_too_long.
 */
static void metric_alarms(int fd, const char *buf)
{
	char label[64];
	unsigned long i = 0;
	int have_header = 0;

	while (buf[i]) {
		unsigned long name, end, st, n;
		int clear;

		if (buf[i] != 'A' || buf[i+1] != 'l' || buf[i+2] != 'a' ||
		    buf[i+3] != 'r' || buf[i+4] != 'm' || buf[i+5] != ' ') {
			i++;
			continue;
		}

		name = i + 6;
		end = name;
		while (buf[end] && buf[end] != ',' && buf[end] != '\n')
			end++;
		if (buf[end] != ',')
			break;

		st = end;
		while (buf[st] && buf[st] != ':' && buf[st] != '\n')
			st++;
		if (buf[st] != ':')
			break;
		st++;
		while (buf[st] == ' ')
			st++;

		clear = (buf[st] == 'c' && buf[st+1] == 'l' && buf[st+2] == 'e' &&
			 buf[st+3] == 'a' && buf[st+4] == 'r');

		n = norm_label(label, sizeof(label), buf + name, end - name);

		if (!have_header) {
			emit_header(fd, "gpon_alarm",
				    "GPON alarm state; 1 means the alarm is asserted.",
				    "gauge");
			have_header = 1;
		}

		put_fd(fd, "gpon_alarm{alarm=\"");
		write_all(fd, label, n);
		put_fd(fd, "\"} ");
		put_fd(fd, clear ? "0\n" : "1\n");

		i = end;
	}
}

/*
 * `diag gpon show counter global ds-eth` prints a banner and then one
 * "Label : value" line per counter:
 *
 *     GPON ONU MAC Device Counter: DS ETH
 *     Total Unicast   : 989
 *     FCS Error       : 0
 *
 * Parsed generically — any line whose text after the colon is nothing but
 * digits becomes a series. That rejects the banner (value "DS ETH") and the
 * "RTK.0> command:" prompt (no value) without special-casing either, and picks
 * up counters that are not in this list if the firmware grows them.
 *
 * us-eth currently prints the banner and no rows, so it yields no series at
 * all — which is the correct representation of "the device reports nothing".
 */
__attribute__((unused))
static void metric_counters(int fd, const char *name, const char *help,
			    char *const argv[])
{
	char buf[2048];
	char label[64];
	unsigned long i = 0;
	int have_header = 0;

	if (run_to_buf(DIAG_PATH, argv, buf, sizeof(buf), DIAG_TIMEOUT_MS) <= 0)
		return;
	while (buf[i]) {
		unsigned long ls = i, le = i, col, v, vstart, tail, a, b, n;

		while (buf[le] && buf[le] != '\n')
			le++;

		col = ls;
		while (col < le && buf[col] != ':')
			col++;
		if (col == le)
			goto next;

		v = col + 1;
		while (v < le && buf[v] == ' ')
			v++;
		vstart = v;
		while (v < le && buf[v] >= '0' && buf[v] <= '9')
			v++;
		if (v == vstart)
			goto next;		/* no number: banner or prompt */

		tail = v;
		while (tail < le && (buf[tail] == ' ' || buf[tail] == '\r'))
			tail++;
		if (tail != le)
			goto next;		/* trailing junk: not a counter */

		a = ls;
		b = col;
		while (a < b && buf[a] == ' ')
			a++;
		while (b > a && buf[b - 1] == ' ')
			b--;
		if (b == a)
			goto next;

		if (!have_header) {
			emit_header(fd, name, help, "counter");
			have_header = 1;
		}

		n = norm_label(label, sizeof(label), buf + a, b - a);
		put_fd(fd, name);
		put_fd(fd, "{counter=\"");
		write_all(fd, label, n);
		put_fd(fd, "\"} ");
		write_all(fd, buf + vstart, v - vstart);
		put_fd(fd, "\n");

next:
		i = (buf[le] == '\n') ? le + 1 : le;
	}
}

/*
 * `diag mib dump counter port all` — the switch/PON MAC MIB counters.
 *
 * This is the block the vendor web UI reads (boa's ponGetStatus, which prints
 * them with %llu), and it is a DIFFERENT counter block from
 * `gpon show counter global ...` further down this file. The difference is the
 * whole reason these can be exported and those cannot:
 *
 *   - Free-running, not read-and-clear. `diag mib get count-mode` reports
 *     "normal free run", and two reads six seconds apart gave 165417214 then
 *     172149239 on port 0 — it grew, it did not reset. Reading is therefore
 *     non-destructive, so a scrape does not steal counts from the web UI or
 *     from a manual diag. Resetting is a separate explicit command
 *     (`diag mib reset counter port ...`), which nothing here ever runs.
 *   - ifInOctets/ifOutOctets are meant to be wider than 32 bits: they are two
 *     consecutive registers in odi-oss's own kernel driver
 *     (odi_switch_mib.c, ODI_SW_MIB_WIDE), and one read of ifInOctets came
 *     back 4881693552, past 2^32. But a scrape does not get to assume the
 *     stick it is talking to is running that driver -- rc35 can load an
 *     /etc/config/metricsd override built against a newer exporter than the
 *     kernel underneath, and an older driver reports these as plain 32-bit
 *     registers that DO wrap, at 4.29 GB. So the exporter tracks the last raw
 *     reading per port and extends it into a monotonic 64-bit total itself
 *     (wrap.h, extend_octets()) rather than trusting the field width. On a
 *     stick with the fixed driver this is a no-op: the reading never goes
 *     backwards, so the "extend" path never fires and the total tracks the
 *     raw value exactly. See docs/DESIGN.md.
 *
 * Both properties together mean these are real Prometheus counters. Every
 * other family below is passed straight through as the digits diag printed,
 * with no parsing or accumulation -- only the two octet counters go through
 * extend_octets(), because they are the only ones a register width mismatch
 * can plausibly wrap inside one scrape interval; see docs/METRICS.md for why
 * packet/error/drop counters do not need the same treatment.
 *
 * Ports, established by correlating deltas over one 25 s window: port 2 is the
 * PON side and port 0 the host SerDes side. Their deltas mirror each other —
 * p2-in 13220591 against p0-out 13203165, p0-in 12626164 against p2-out
 * 12653844 — which is the switch forwarding between the two.
 *
 * This is also why /proc/net/dev is no substitute and pon0 there reads zero:
 * forwarding happens in switch hardware and never reaches the CPU, so eth0's
 * counters only ever show the stick's own management traffic.
 *
 * Only the counters with an unambiguous unit are exported. The device prints 46
 * per port; most of the rest are half-duplex collision counters that mean
 * nothing on a SerDes or a PON. The frame-size histogram (etherStats*Pkts*Octets)
 * is exported too, as gpon_port_frames_total below -- it used to be left out
 * for the same reason, but etherStatsRxOversizePkts turned out to be one of its
 * buckets under an error-sounding name (see mib_frames). Run the command by
 * hand to see the rest.
 */
struct mib_key {
	const char *key;	/* the label diag prints, matched exactly */
	const char *labels;	/* extra label text, or "" */
};

struct mib_fam {
	const char *metric;
	const char *help;
	const struct mib_key *keys;	/* terminated by a NULL key */
	struct octet_state *ostate;	/* non-NULL: extend a wrapped 32-bit
					 * reading to a monotonic 64-bit total,
					 * one entry per port (wrap.h) */
};

static const struct mib_key mib_rx_octets[] = {
	{ "ifInOctets", "" }, { 0, 0 }
};
static const struct mib_key mib_tx_octets[] = {
	{ "ifOutOctets", "" }, { 0, 0 }
};
static const struct mib_key mib_rx_pkts[] = {
	{ "ifInUcastPkts",     ",kind=\"unicast\""   },
	{ "ifInMulticastPkts", ",kind=\"multicast\"" },
	{ "ifInBroadcastPkts", ",kind=\"broadcast\"" },
	{ 0, 0 }
};
static const struct mib_key mib_tx_pkts[] = {
	{ "ifOutUcastPkts",     ",kind=\"unicast\""   },
	{ "ifOutMulticastPkts", ",kind=\"multicast\"" },
	{ "ifOutBroadcastPkts", ",kind=\"broadcast\"" },
	{ 0, 0 }
};
static const struct mib_key mib_rx_drops[] = {
	{ "dot1dTpPortInDiscards", "" }, { 0, 0 }
};
static const struct mib_key mib_tx_drops[] = {
	{ "ifOutDiscards", "" }, { 0, 0 }
};
static const struct mib_key mib_rx_errors[] = {
	{ "etherStatsCRCAlignErrors", ",kind=\"crc_align\"" },
	{ "etherStatsFragments",      ",kind=\"fragment\""  },
	{ "etherStatsJabbers",        ",kind=\"jabber\""    },
	{ "etherStatsRxUndersizePkts", ",kind=\"undersize\"" },
	{ 0, 0 }
};
static const struct mib_key mib_pause[] = {
	{ "dot3InPauseFrames",  ",direction=\"receive\""  },
	{ "dot3OutPauseFrames", ",direction=\"transmit\"" },
	{ 0, 0 }
};

/*
 * Frame-size histogram, both directions in one family. `etherStatsRxOversizePkts`
 * used to be exported as `gpon_port_receive_errors_total{kind="oversize"}`;
 * it is not an error, it is this same "1519_max" bucket under another name --
 * two independent live counters matched it exactly (38605 == 38605 rx,
 * 52482 == 52482 tx) on a stick carrying ordinary VLAN-tagged full-size
 * traffic. See docs/METRICS.md.
 */
static const struct mib_key mib_frames[] = {
	{ "etherStatsRxPkts64Octets",         ",direction=\"rx\",size=\"64\""        },
	{ "etherStatsRxPkts65to127Octets",    ",direction=\"rx\",size=\"65_127\""    },
	{ "etherStatsRxPkts128to255Octets",   ",direction=\"rx\",size=\"128_255\""   },
	{ "etherStatsRxPkts256to511Octets",   ",direction=\"rx\",size=\"256_511\""   },
	{ "etherStatsRxPkts512to1023Octets",  ",direction=\"rx\",size=\"512_1023\""  },
	{ "etherStatsRxPkts1024to1518Octets", ",direction=\"rx\",size=\"1024_1518\"" },
	{ "etherStatsRxPkts1519toMaxOctets",  ",direction=\"rx\",size=\"1519_max\""  },
	{ "etherStatsTxPkts64Octets",         ",direction=\"tx\",size=\"64\""        },
	{ "etherStatsTxPkts65to127Octets",    ",direction=\"tx\",size=\"65_127\""    },
	{ "etherStatsTxPkts128to255Octets",   ",direction=\"tx\",size=\"128_255\""   },
	{ "etherStatsTxPkts256to511Octets",   ",direction=\"tx\",size=\"256_511\""   },
	{ "etherStatsTxPkts512to1023Octets",  ",direction=\"tx\",size=\"512_1023\""  },
	{ "etherStatsTxPkts1024to1518Octets", ",direction=\"tx\",size=\"1024_1518\"" },
	{ "etherStatsTxPkts1519toMaxOctets",  ",direction=\"tx\",size=\"1519_max\""  },
	{ 0, 0 }
};

/*
 * Per-port running totals for the two wide counters, extended past whatever
 * width the register underneath actually turns out to have. File-scope and
 * never reset, so a value only ever grows for the life of the process -- the
 * counter-reset semantics Prometheus expects from a `counter`. See wrap.h.
 */
static struct octet_state rx_octet_state[MIB_MAX_PORTS];
static struct octet_state tx_octet_state[MIB_MAX_PORTS];

static const struct mib_fam mib_fams[] = {
	{ "gpon_port_receive_octets_total",
	  "Octets received on a switch port. port=\"2\" is the PON side, \"0\" the host SerDes side.",
	  mib_rx_octets, rx_octet_state },
	{ "gpon_port_transmit_octets_total",
	  "Octets transmitted on a switch port.", mib_tx_octets, tx_octet_state },
	{ "gpon_port_receive_packets_total",
	  "Packets received on a switch port, by destination kind.", mib_rx_pkts, 0 },
	{ "gpon_port_transmit_packets_total",
	  "Packets transmitted on a switch port, by destination kind.", mib_tx_pkts, 0 },
	{ "gpon_port_receive_drops_total",
	  "Received frames dropped by the bridge on a switch port.", mib_rx_drops, 0 },
	{ "gpon_port_transmit_drops_total",
	  "Frames dropped instead of being transmitted on a switch port.", mib_tx_drops, 0 },
	{ "gpon_port_receive_errors_total",
	  "Malformed frames received on a switch port, by error kind.", mib_rx_errors, 0 },
	{ "gpon_port_pause_frames_total",
	  "802.3x pause frames seen on a switch port.", mib_pause, 0 },
	{ "gpon_port_frames_total",
	  "Frames seen on a switch port, bucketed by size in octets.", mib_frames, 0 },
	{ 0, 0, 0, 0 }
};

/*
 * Emit every sample for one key, walking the whole buffer and tracking which
 * "Port: N" block each line falls in. Driven per key rather than per line so
 * each family's "# HELP"/"# TYPE" is written exactly once — the device prints
 * one complete block per port, so a line-ordered walk would repeat the header
 * for every port and Prometheus rejects a duplicated header.
 */
static void mib_emit_key(int fd, const char *buf, const char *metric,
			 const struct mib_key *mk, struct octet_state *ostates)
{
	char port[8];
	unsigned long i = 0, plen = 0;

	while (buf[i]) {
		unsigned long ls = i, le = i, k, vs;

		while (buf[le] && buf[le] != '\n')
			le++;

		k = line_key(buf, ls, "Port:");
		if (k) {
			while (k < le && buf[k] == ' ')
				k++;
			plen = 0;
			while (k < le && buf[k] >= '0' && buf[k] <= '9' &&
			       plen + 1 < sizeof(port))
				port[plen++] = buf[k++];
			goto next;
		}

		if (!plen)
			goto next;	/* a counter before any "Port:" line */

		k = line_key(buf, ls, mk->key);
		if (!k)
			goto next;
		/* The label has to END here: only spaces, then the colon.
		 * Without this a key would also match any longer label it
		 * happens to be a prefix of. */
		while (k < le && buf[k] == ' ')
			k++;
		if (k >= le || buf[k] != ':')
			goto next;

		k++;
		while (k < le && buf[k] == ' ')
			k++;
		vs = k;
		while (k < le && buf[k] >= '0' && buf[k] <= '9')
			k++;
		if (k == vs)
			goto next;	/* no number: not a counter line */

		put_fd(fd, metric);
		put_fd(fd, "{port=\"");
		write_all(fd, port, plen);
		put_fd(fd, "\"");
		put_fd(fd, mk->labels);
		put_fd(fd, "} ");

		/* ostates is only non-NULL for ifInOctets/ifOutOctets. An
		 * out-of-range port index falls back to the raw digits
		 * verbatim -- the same thing every other counter here does --
		 * rather than guessing which array slot it would be. */
		if (ostates) {
			int pi = port_index(port, plen);

			if (pi >= 0) {
				unsigned long long raw = parse_u64(buf + vs, k - vs);
				char digits[20];
				unsigned long n = format_u64(digits,
							      extend_octets(&ostates[pi], raw));

				write_all(fd, digits, n);
			} else {
				write_all(fd, buf + vs, k - vs);
			}
		} else {
			write_all(fd, buf + vs, k - vs);
		}
		put_fd(fd, "\n");

next:
		i = (buf[le] == '\n') ? le + 1 : le;
	}
}

static void metric_port_mib(int fd, const char *buf)
{
	unsigned long f, k;

	for (f = 0; mib_fams[f].metric; f++) {
		emit_header(fd, mib_fams[f].metric, mib_fams[f].help, "counter");
		for (k = 0; mib_fams[f].keys[k].key; k++)
			mib_emit_key(fd, buf, mib_fams[f].metric,
				     &mib_fams[f].keys[k], mib_fams[f].ostate);
	}
}

/*
 * One diag invocation per scrape.
 *
 * /bin/diag costs ~32 ms per run on this CPU and almost all of it is startup:
 * a single transceiver read measured 32.5 ms against 35.5 ms for the 92-counter
 * mib dump, and 4.0 ms for a bare fork+exec of /bin/true. Paying that eight
 * times came to 259.5 ms a scrape; the same eight commands piped into one diag
 * take 47.5 ms. So the cost scales with the number of PROCESSES, not the number
 * of metrics, and the fix is to stop starting more of them.
 *
 * diag reads commands from stdin and echoes each after its "RTK.0> " prompt,
 * which is what makes the output splittable back into per-command sections.
 *
 * The trade: this is now one failure domain. Previously a diag that hung or
 * crashed cost one metric; now it costs all of them. The /proc metrics are
 * unaffected and gpon_exporter_up still reports, so a scrape still tells you
 * the stick is alive.
 */
#define DIAG_PROMPT "RTK.0> "

struct diag_sec {
	const char *cmd;	/* sent to diag, and matched against its echo */
	const char *metric;	/* single-valued case */
	const char *help;
	int (*scan)(const char *, unsigned long *, unsigned long *);
	void (*custom)(int fd, const char *sec);	/* multi-series case */
};

/*
 * Every metric name in this file lives here and nowhere else; rename in one
 * place if you need to match an existing dashboard. The order is the order the
 * commands are sent, and therefore the order the series come out.
 */
static const struct diag_sec diag_secs[] = {
	{ "pon get transceiver bias-current", "gpon_bias_current_ma",
	  "Bias current of the GPON transceiver, in mA.", scan_decimal, 0 },
	{ "pon get transceiver rx-power", "gpon_rx_power_dbm",
	  "Rx power of the GPON transceiver, in dBm.", scan_decimal, 0 },
	{ "pon get transceiver tx-power", "gpon_tx_power_dbm",
	  "Tx power of the GPON transceiver, in dBm.", scan_decimal, 0 },
	{ "pon get transceiver temperature", "gpon_temperature_celsius",
	  "Temperature of the GPON transceiver, in Celsius.", scan_decimal, 0 },
	{ "pon get transceiver voltage", "gpon_voltage_volts",
	  "Supply voltage of the GPON transceiver, in Volts.", scan_decimal, 0 },
	{ "gpon get onu-state", "gpon_onu_state",
	  "ONU state number, the N in O(N). 5 is operational.", scan_onu_state, 0 },
	{ "gpon get alarm-status", 0, 0, 0, metric_alarms },
	{ "mib dump counter port all", 0, 0, 0, metric_port_mib },
	{ 0, 0, 0, 0, 0 }
};

/*
 * Built from the table rather than written out as a literal, so the commands
 * sent and the echoes matched against cannot drift apart. A mismatch would not
 * fail loudly — it would silently drop that metric.
 */
static unsigned long build_diag_script(char *dst, unsigned long cap)
{
	const char *tail = "exit\n";
	unsigned long n = 0, i, k;

	for (i = 0; diag_secs[i].cmd; i++) {
		for (k = 0; diag_secs[i].cmd[k]; k++)
			if (n + 2 < cap)
				dst[n++] = diag_secs[i].cmd[k];
		if (n + 2 < cap)
			dst[n++] = '\n';
	}
	/* Closing stdin would end it too, but `exit` lets diag leave on its own
	 * terms rather than on a read error. */
	for (k = 0; tail[k]; k++)
		if (n + 2 < cap)
			dst[n++] = tail[k];

	dst[n] = 0;
	return n;
}

/* Index just past `needle`, or 0 if absent. 0 is unambiguous as "not found"
 * because a match always lands past the needle's own length. */
static unsigned long find_after(const char *buf, unsigned long from,
				const char *needle)
{
	unsigned long i, k;

	for (i = from; buf[i]; i++) {
		for (k = 0; needle[k] && buf[i + k] == needle[k]; k++)
			;
		if (!needle[k])
			return i + k;
	}
	return 0;
}

/* Whether the `alen` bytes at `a` are exactly the string `b`. */
static int cmd_is(const char *a, unsigned long alen, const char *b)
{
	unsigned long k;

	for (k = 0; k < alen; k++)
		if (!b[k] || a[k] != b[k])
			return 0;
	return b[alen] == 0;
}

/*
 * Whether the diag scrape worked, and how completely.
 *
 * Batching every command into one fork made diag a single failure domain: one
 * hang, crash or missing binary now costs ~90 metric families at once. Their
 * absence is detectable in Prometheus, but absence is a weak signal -- it looks
 * identical to a stick that has not been scraped yet, or to a relabelling
 * mistake, and gpon_exporter_up is hardcoded to 1 so it keeps reporting health
 * that only covers the /proc half.
 *
 * The section count matters as much as the boolean. diag's output is about
 * 6.9 KB into a 16 KB buffer, and a truncated read costs the TAIL sections
 * silently -- the mib counter dump is last, so a partial scrape looks like a
 * working one that simply has no forwarding data. Emitting parsed against
 * expected makes that a comparison rather than something you have to notice.
 */
static void emit_diag_health(int fd, int up, unsigned long parsed,
			     unsigned long expected)
{
	emit_header(fd, "gpon_diag_up",
		    "1 when /bin/diag ran and at least one section parsed.", "gauge");
	put_fd(fd, "gpon_diag_up ");
	put_fd(fd, up ? "1\n" : "0\n");

	emit_header(fd, "gpon_diag_sections_parsed",
		    "diag command sections understood in this scrape.", "gauge");
	put_fd(fd, "gpon_diag_sections_parsed ");
	put_u32_fd(fd, parsed);
	put_fd(fd, "\n");

	emit_header(fd, "gpon_diag_sections_expected",
		    "diag command sections this build asks for; parsed below this is a truncated scrape.",
		    "gauge");
	put_fd(fd, "gpon_diag_sections_expected ");
	put_u32_fd(fd, expected);
	put_fd(fd, "\n");
}

/*
 * Provisioned, as opposed to merely synced.
 *
 * O5 says the OLT ranged the ONU and finished MIB sync; it says nothing about
 * whether a service exists. An OLT that accepts a wrong serial or PLOAM
 * password leaves the stick at O5 with no VLAN, no GEM flow and no bridge
 * connection, and every other metric here looks healthy (Anime4000/RTL960x
 * issues 461 and 475). The one table that answers is the data-path service
 * table: `omcicli dump srvflow` prints one "SERVID n: Used: 1, (...)" row per
 * installed connection, on the vendor omci_app and on odi-oss omcid alike.
 * Counting those rows costs one short fork; the vendor binary prints 256
 * rows, most of them Used: 0.
 */
#define OMCICLI_PATH "/bin/omcicli"

static void emit_omci_up(int fd, int up)
{
	emit_header(fd, "gpon_omci_up",
		    "1 when omcicli got an answer from omcid within "
		    "OMCICLI_TIMEOUT_MS; 0 means omcid did not answer in time "
		    "and the child was killed -- gpon_omci_services below is "
		    "stale or absent.", "gauge");
	put_fd(fd, "gpon_omci_up ");
	put_fd(fd, up ? "1\n" : "0\n");
}

static void emit_omci_metrics(int fd)
{
	static char *const argv[] = { "omcicli", "dump", "srvflow", 0 };
	char buf[8192];
	unsigned long i, used = 0;
	long n = run_to_buf(OMCICLI_PATH, argv, buf, sizeof(buf) - 1,
			    OMCICLI_TIMEOUT_MS);

	if (n <= 0) {
		/* n == -1: no omcicli, or the fork/pipe setup itself failed --
		 * nothing ran, so there is nothing to call "down". n == -2:
		 * omcicli ran but omcid never answered it within the bound
		 * (the respawn/0x800 bug this metric exists to catch) -- that
		 * IS down, and must say so rather than go quiet. */
		if (n == -2)
			emit_omci_up(fd, 0);
		return;
	}
	buf[n] = 0;
	for (i = 0; buf[i]; i++)
		if (buf[i] == 'U' && buf[i + 1] == 's' && buf[i + 2] == 'e' &&
		    buf[i + 3] == 'd' && buf[i + 4] == ':' && buf[i + 5] == ' ' &&
		    buf[i + 6] == '1')
			used++;
	emit_omci_up(fd, 1);
	emit_header(fd, "gpon_omci_services",
		    "Bridge connections (services) the OLT has provisioned and the ONU "
		    "installed, from `omcicli dump srvflow`. 0 at O5 means synced but "
		    "not provisioned.", "gauge");
	put_fd(fd, "gpon_omci_services ");
	put_u32_fd(fd, used);
	put_fd(fd, "\n");
}

static void emit_diag_metrics(int fd)
{
	static char *const argv[] = { "diag", 0 };
	char script[512];
	/* Two ports of mib counters are 5856 bytes on their own; the whole
	 * scrape's output measured about 6.9 KB, so this is a bit over 2x
	 * headroom. A truncated read costs the tail sections rather than
	 * corrupting anything, but the mib dump is last, so headroom matters
	 * more than it looks. */
	char buf[16384];
	unsigned long cs;
	unsigned long parsed = 0, expected = 0;

	while (diag_secs[expected].cmd)
		expected++;

	build_diag_script(script, sizeof(script));
	if (run_script_to_buf(DIAG_PATH, argv, script, buf, sizeof(buf),
			      DIAG_TIMEOUT_MS) <= 0) {
		emit_diag_health(fd, 0, 0, expected);
		return;
	}

	/* cs always points at a command, just past its prompt. find_after
	 * returns the position AFTER the needle, so the next section's start is
	 * the previous search's result — searching again from it would step over
	 * a prompt and drop every other metric. */
	cs = find_after(buf, 0, DIAG_PROMPT);
	while (cs) {
		unsigned long ce, ss, nxt, cut, t;
		char saved = 0;

		ce = cs;
		while (buf[ce] && buf[ce] != '\n' && buf[ce] != '\r')
			ce++;
		if (!buf[ce])
			break;		/* the trailing prompt, with no command */

		ss = ce;
		while (buf[ss] == '\n' || buf[ss] == '\r')
			ss++;

		nxt = find_after(buf, ss, DIAG_PROMPT);

		/* Terminate this section before handing it over. Every extractor
		 * takes a NUL-terminated string and stops at its first match, so
		 * without this a command that printed nothing would be handed the
		 * NEXT command's output and report it as its own value. The byte
		 * is restored afterwards because it is part of the marker used to
		 * find the following section. */
		cut = nxt ? nxt - (sizeof(DIAG_PROMPT) - 1) : 0;
		if (cut) {
			saved = buf[cut];
			buf[cut] = 0;
		}

		for (t = 0; diag_secs[t].cmd; t++) {
			if (!cmd_is(buf + cs, ce - cs, diag_secs[t].cmd))
				continue;
			if (diag_secs[t].custom)
				diag_secs[t].custom(fd, buf + ss);
			else
				emit_scalar(fd, diag_secs[t].metric,
					    diag_secs[t].help, buf + ss,
					    diag_secs[t].scan);
			parsed++;
			break;
		}

		if (cut)
			buf[cut] = saved;
		cs = nxt;
	}

	emit_diag_health(fd, parsed > 0, parsed, expected);
}

/*
 * A label value must not contain a quote, a backslash or a newline, and this
 * one comes from argv[0] — chosen by whoever started the process, not by us.
 * Escaping it properly would be more code than refusing it: an unexpected path
 * is reported as "unknown" rather than being allowed to produce an exposition
 * Prometheus cannot parse.
 */
static int label_safe(const char *s)
{
	unsigned long i;

	for (i = 0; s[i]; i++)
		if (s[i] == '"' || s[i] == '\\' || s[i] == '\n' || s[i] == '\r')
			return 0;
	return i != 0;
}

static void metric_build_info(int fd)
{
	emit_header(fd, "gpon_exporter_build_info",
		    "Always 1. version is `git describe` at build time; path is argv[0], "
		    "which says whether this is the image's /bin/metricsd or an "
		    "/etc/config override.", "gauge");
	put_fd(fd, "gpon_exporter_build_info{version=\"" BUILD_ID "\",path=\"");
	put_fd(fd, label_safe(exporter_path) ? exporter_path : "unknown");
	put_fd(fd, "\"} 1\n");
}

/*
 * The image's build manifest, as labels on one gauge.
 *
 * gpon_exporter_build_info says which EXPORTER is running; this says which
 * IMAGE it came out of, which is a different question and the one that was
 * unanswerable before /etc/odi-build existed. Both partitions of a stick used
 * to report the same vendor version string, so "which build is on which half"
 * had to be reconstructed from memory.
 *
 * Absent on an older image, in which case nothing is emitted rather than
 * emitting empty labels -- an absent series is honest, and a label set full of
 * "" is not.
 */
static void metric_image_info(int fd)
{
	char man[512];
	long n = read_file("/etc/odi-build", man, sizeof(man));
	unsigned long i = 0;
	int first = 1;

	if (n <= 0)
		return;

	emit_header(fd, "gpon_image_info",
		    "Always 1. Labels name the firmware image this stick was built "
		    "from and the component builds inside it, from /etc/odi-build.",
		    "gauge");
	put_fd(fd, "gpon_image_info{");
	while (man[i]) {
		unsigned long ls = i, le = i, eq;

		while (man[le] && man[le] != '\n')
			le++;
		eq = ls;
		while (eq < le && man[eq] != '=')
			eq++;
		if (eq == le || eq == ls)
			goto next;

		/* Both halves have to survive being a label. A stray quote in a
		 * git describe string would produce an exposition Prometheus
		 * cannot parse, so the pair is dropped rather than escaped. */
		man[eq] = 0;
		man[le] = 0;
		if (label_safe(man + ls) && label_safe(man + eq + 1)) {
			if (!first)
				put_fd(fd, ",");
			first = 0;
			put_fd(fd, man + ls);
			put_fd(fd, "=\"");
			put_fd(fd, man + eq + 1);
			put_fd(fd, "\"");
		}
next:
		i = le + 1;
	}
	put_fd(fd, "} 1\n");
}

/*
 * The boot counter and why the previous boot ended, from odi-oss's
 * /proc/odi_ramlog_prev (src/resetinfo.h has the format). Both are fixed for
 * the life of a boot, so the first successful read is kept and later scrapes
 * do not re-read it: the kernel renders the whole saved ramlog, about 8 KB,
 * on every read of that file. One bounded read of its first 255 bytes. Absent
 * on a stock kernel, or an odi-oss kernel without the ramlog: nothing is
 * emitted, and each scrape tries again.
 */
static struct resetinfo reset_info;
static int reset_info_ok;

static void metric_reset_info(int fd)
{
	if (!reset_info_ok) {
		char buf[256];

		if (read_file("/proc/odi_ramlog_prev", buf, sizeof(buf)) <= 0)
			return;
		reset_info_ok = resetinfo_parse(buf, &reset_info);
		if (!reset_info_ok)
			return;
	}

	emit_header(fd, "gpon_boot_count",
		    "Boots of this image since the last power cycle, this one "
		    "included, from /proc/odi_ramlog_prev. Restarts at 1 after a "
		    "power cycle.", "gauge");
	put_fd(fd, "gpon_boot_count ");
	put_fd(fd, reset_info.boot);
	put_fd(fd, "\n");

	if (!reset_info.reason[0])
		return;
	emit_header(fd, "gpon_last_reset_reason",
		    "Always 1. reason is why the previous boot ended, as the kernel "
		    "recorded it: wdt_client (with client), wdt_mem, wdt_userland, "
		    "reboot, halt, poweroff, panic, oops, power or unknown.",
		    "gauge");
	put_fd(fd, "gpon_last_reset_reason{reason=\"");
	put_fd(fd, reset_info.reason);
	if (reset_info.client[0]) {
		put_fd(fd, "\",client=\"");
		put_fd(fd, reset_info.client);
	}
	put_fd(fd, "\"} 1\n");
}

/*
 * The config store: one gpon_config_info{file,hash} per file present, and
 * gpon_config_mtime_seconds{file}. src/confighash.h has which files and why.
 *
 * A hash, never the contents: lastgood*.xml hold the PLOAM password and the
 * LOID password, and nothing of them goes into a label. Alert on the hash
 * changing (a new series for the file), not on the mtime: the stock firmware
 * and flash may rewrite a file with the same contents.
 *
 * Cheap on every scrape: one stat per file, no fork. md5sum (busybox, the
 * native tool, which also lets anyone check the label by hand) runs only when
 * a stat differs from the one the cached hash was taken under, so normally
 * once per boot. A file that is absent emits nothing for it, which an alert
 * can tell from a stick that was not scraped because gpon_exporter_up is
 * still there. If md5sum fails or times out, the mtime is still emitted, the
 * hash is not, and the next scrape tries again.
 */
#define MD5SUM_PATH "/bin/md5sum"

static struct config_stamp config_cached[CONFIG_NFILES];
static char config_hash[CONFIG_NFILES][CONFIG_HASH_LEN + 1];
static unsigned config_hash_ok;		/* bit i: config_hash[i] is current */

static void metric_config(int fd)
{
	struct config_stamp now[CONFIG_NFILES];
	char paths[CONFIG_NFILES][32];
	char *argv[CONFIG_NFILES + 2];
	int f, argc = 1, stale = 0, any = 0;

	argv[0] = "md5sum";
	for (f = 0; f < CONFIG_NFILES; f++) {
		struct stat64 st;
		unsigned long k = 0, n = 0;
		const char *d = CONFIG_DIR;

		while (d[k] && n + 1 < sizeof(paths[f]))
			paths[f][n++] = d[k++];
		for (k = 0; config_names[f][k] && n + 1 < sizeof(paths[f]); k++)
			paths[f][n++] = config_names[f][k];
		paths[f][n] = 0;

		now[f].present = stat_path(paths[f], &st) == 0;
		now[f].ino = now[f].present ? st.st_ino : 0;
		now[f].size = now[f].present ? st.st_size : 0;
		now[f].mtime = now[f].present ? st.st_mtime : 0;
		now[f].mtime_nsec = now[f].present ? st.st_mtime_nsec : 0;
		now[f].ctime = now[f].present ? st.st_ctime : 0;
		now[f].ctime_nsec = now[f].present ? st.st_ctime_nsec : 0;
		if (!now[f].present)
			continue;
		any = 1;
		argv[argc++] = paths[f];
		if (!(config_hash_ok & (1u << f)) ||
		    !config_stamp_eq(&now[f], &config_cached[f]))
			stale = 1;
	}
	argv[argc] = 0;

	if (!any)
		return;

	if (stale) {
		char buf[512];
		long got = run_to_buf(MD5SUM_PATH, argv, buf, sizeof(buf),
				      MD5SUM_TIMEOUT_MS);

		config_hash_ok = got > 0 ? config_parse_md5sum(buf, config_hash) : 0;
		for (f = 0; f < CONFIG_NFILES; f++)
			config_stamp_copy(&config_cached[f], &now[f]);
	}

	emit_header(fd, "gpon_config_info",
		    "Always 1. hash is the first 12 hex digits of the md5 of one "
		    "config-store file under /var/config; a new hash is a changed "
		    "file.", "gauge");
	for (f = 0; f < CONFIG_NFILES; f++) {
		if (!now[f].present || !(config_hash_ok & (1u << f)))
			continue;
		put_fd(fd, "gpon_config_info{file=\"");
		put_fd(fd, config_names[f]);
		put_fd(fd, "\",hash=\"");
		put_fd(fd, config_hash[f]);
		put_fd(fd, "\"} 1\n");
	}

	emit_header(fd, "gpon_config_mtime_seconds",
		    "Modification time of one config-store file under /var/config, "
		    "by the stick clock at the time of the write.", "gauge");
	for (f = 0; f < CONFIG_NFILES; f++) {
		if (!now[f].present)
			continue;
		put_fd(fd, "gpon_config_mtime_seconds{file=\"");
		put_fd(fd, config_names[f]);
		put_fd(fd, "\"} ");
		put_u32_fd(fd, (unsigned long)now[f].mtime);
		put_fd(fd, "\n");
	}
}

/*
 * Which slot is running and whether it is committed, from the file odi-oss
 * writes at boot (src/slot_state.h has the format and the rendering). A tmpfs
 * file of a few hundred bytes: the read cannot block, and the buffer bounds
 * it. No file, nothing emitted.
 */
static void metric_slot_state(int fd)
{
	char st[512], out[1024];
	unsigned long n;

	if (read_file(SLOT_STATE_PATH, st, sizeof(st)) <= 0)
		return;
	n = slot_state_render(st, out, sizeof(out));
	if (n)
		write_all(fd, out, n);
}

static void emit_metrics(int fd)
{
	put_fd(fd, "# HELP gpon_exporter_up Always 1. Confirms the exporter ran.\n"
		   "# TYPE gpon_exporter_up gauge\n"
		   "gpon_exporter_up 1\n");

	metric_build_info(fd);
	metric_image_info(fd);
	metric_slot_state(fd);
	metric_uptime(fd);
	metric_reset_info(fd);
	metric_loadavg(fd);
	metric_meminfo(fd);
	metric_netdev(fd);
	metric_config(fd);

	/* Everything from /bin/diag, in a single fork. */
	emit_diag_metrics(fd);

	/* And one more fork for the OMCI service table. */
	emit_omci_metrics(fd);

	/*
	 * NOT exporting `gpon show counter global ds-eth`. Four consecutive reads
	 * gave 55827, 13537, 53607, 8873 — the registers are read-and-clear, each
	 * read returning the count since the previous one.
	 *
	 * Two problems, the second worse than the first:
	 *
	 *  - They are not monotonic, so Prometheus `counter` is the wrong type and
	 *    rate() would read every reset as a counter restart.
	 *  - Reading is DESTRUCTIVE. A scrape consumes the delta, so the vendor web
	 *    UI or a manual `diag` silently loses whatever we took, and we lose
	 *    whatever they take. Scraping would mutate device state that something
	 *    else may depend on.
	 *
	 * The fix, if we want them, is to accumulate deltas into a monotonic total
	 * held in this process — which is the standard treatment for read-and-clear
	 * hardware counters, and gives correct counter-reset semantics on restart.
	 * It is only sound if this exporter is the sole reader. Left undone until
	 * that is established.
	 */
}

#endif /* ODI_METRICS_BODY_H */
