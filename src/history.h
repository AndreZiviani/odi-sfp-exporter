/*
 * The minute history: one line a minute, the newest HIST_KEEP of them, in
 * /var/run/history, for a look back after a failure on a stick that no
 * Prometheus scrapes. The web UI draws it (odi-ui GET /api/history) and the
 * odi-oss diagnostics bundle carries it; both read the file and nothing else,
 * so the format below is an interface.
 *
 * A line is space-separated key=value pairs, oldest line first. A reader takes
 * the keys it knows, so a key added later breaks nobody. A key whose source is
 * missing is left out (a stock slot has no /proc/odi_gpon, a diag that failed
 * gives no optics), never written as 0:
 *
 *   t=      the wall clock, epoch seconds (meaningless before NTP synced)
 *   up=     /proc/uptime, whole seconds; what orders the samples
 *   st=     ONU state, 1 to 7 (diag gpon get onu-state, else odi_gpon)
 *   al=     asserted GPON alarms, comma separated (LOS,LOF), or -
 *   rx= tx= receive and transmit power, dBm; tc= module temperature, C;
 *   bi=     bias current, mA; all four rounded to two decimals
 *   di= do= octets in and out of switch port 2, the fibre side; totals
 *   los= lof= lom=   alarm events since the driver loaded (odi_gpon)
 *   o5i= o5o=        transitions into and out of O5 since then: a drop
 *                    between two samples shows here even when both read O5
 *   bip=    downstream BIP error bits, total; fecu= uncorrectable FEC
 *           codewords, total (/proc/odi_gpon/counters)
 *   cb= ct= CPU ticks busy (all but idle and iowait) and in all, /proc/stat
 *   ma=     MemAvailable, kB
 *
 * Totals stay totals: the reader takes the difference between two samples.
 * Every value is the text its source printed (rounded, for the four optics
 * figures) or a sum of such texts, never a float.
 *
 * Pure: no syscalls, so it is tested on the host (test/test_history.c).
 * metricsd.c does the reading, the timing and the writing.
 */
#ifndef ODI_HISTORY_H
#define ODI_HISTORY_H

#include "wrap.h"

#define HIST_KEEP 120		/* two hours of minutes */
/* A line is about 170 bytes. 512 holds the longest one the parsers can
 * produce (every total at its 20 digits, the alarm list at its 62
 * characters), so a sample is never cut; hist_build_line refuses one that
 * would not fit all the same. The file is then at most 60 KiB. */
#define HIST_LINE 512
#define HIST_INTERVAL_S 60

/* What one sample is built from: the texts as read, any of them null. */
struct hist_in {
	const char *diag;		/* the reply to HIST_DIAG_SCRIPT */
	const char *gpon_status;	/* /proc/odi_gpon/status */
	const char *gpon_counters;	/* /proc/odi_gpon/counters */
	const char *stat;		/* /proc/stat */
	const char *meminfo;		/* /proc/meminfo */
	const char *uptime;		/* /proc/uptime */
	long wall;			/* epoch seconds, or < 0 when unknown */
};

/*
 * The diag questions of a sample, the same shapes the scrape asks. mib dump
 * for all ports, as the scrape does, since that spelling is the one both the
 * stock and the odi-oss diag answer.
 */
#define HIST_DIAG_SCRIPT \
	"pon get transceiver rx-power\n" \
	"pon get transceiver tx-power\n" \
	"pon get transceiver temperature\n" \
	"pon get transceiver bias-current\n" \
	"gpon get onu-state\n" \
	"gpon get alarm-status\n" \
	"mib dump counter port all\n" \
	"exit\n"

/* ---- small text helpers ------------------------------------------------ */

static int hist_isdigit(char c) { return c >= '0' && c <= '9'; }

static unsigned long hist_len(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

/* Whether s (n bytes) starts with p. */
static int hist_pre(const char *s, unsigned long n, const char *p)
{
	unsigned long i;

	for (i = 0; p[i]; i++)
		if (i >= n || s[i] != p[i])
			return 0;
	return 1;
}

/* The next line of text from *pos: its start and length, without the \n and
 * without any \r (diag puts one before some prompts). 0 at the end. */
static int hist_next_line(const char *text, unsigned long *pos,
			  const char **line, unsigned long *len)
{
	unsigned long i = *pos, e;

	if (!text || !text[i])
		return 0;
	while (text[i] == '\r')
		i++;
	for (e = i; text[e] && text[e] != '\n'; e++)
		;
	*line = text + i;
	*len = e - i;
	while (*len && (*line)[*len - 1] == '\r')
		(*len)--;
	*pos = text[e] ? e + 1 : e;
	return 1;
}

/* The idx-th whitespace-separated field of a line (0-based). */
static int hist_field(const char *line, unsigned long len, unsigned idx,
		      const char **f, unsigned long *flen)
{
	unsigned long i = 0, s;
	unsigned n = 0;

	for (;;) {
		while (i < len && (line[i] == ' ' || line[i] == '\t'))
			i++;
		if (i >= len)
			return 0;
		s = i;
		while (i < len && line[i] != ' ' && line[i] != '\t')
			i++;
		if (n++ == idx) {
			*f = line + s;
			*flen = i - s;
			return 1;
		}
	}
}

static int hist_field_is(const char *line, unsigned long len, unsigned idx, const char *want)
{
	const char *f;
	unsigned long n;

	return hist_field(line, len, idx, &f, &n) && n == hist_len(want) && hist_pre(f, n, want);
}

static int hist_all_digits(const char *s, unsigned long n)
{
	unsigned long i;

	if (!n || n > 20)
		return 0;
	for (i = 0; i < n; i++)
		if (!hist_isdigit(s[i]))
			return 0;
	return 1;
}

/* A value: up to 20 digits, kept as text. */
struct hist_val {
	char s[24];
	int set;
};

static void hist_val_setn(struct hist_val *v, const char *s, unsigned long n)
{
	unsigned long i;

	if (n > sizeof(v->s) - 1)
		n = sizeof(v->s) - 1;
	for (i = 0; i < n; i++)
		v->s[i] = s[i];
	v->s[n] = 0;
	v->set = 1;
}

static void hist_val_u64(struct hist_val *v, unsigned long long x)
{
	v->set = 1;
	v->s[format_u64(v->s, x)] = 0;
}

/*
 * The figure after the colon, "[-]digits[.digits]", rounded half away from
 * zero to two decimals: "Rx Power: -22.924298  dBm" gives -22.92. Anything
 * else leaves v unset (diag prints "% transceiver read failed" without a
 * colon on LOS).
 */
static void hist_fig(struct hist_val *v, const char *line, unsigned long len)
{
	unsigned long i = 0, n = 0;
	unsigned long ip = 0, frac = 0, fd = 0;
	int neg = 0, round = 0;
	char out[24];

	while (i < len && line[i] != ':')
		i++;
	if (i >= len)
		return;
	i++;
	while (i < len && (line[i] == ' ' || line[i] == '\t'))
		i++;
	if (i < len && line[i] == '-') {
		neg = 1;
		i++;
	}
	if (i >= len || !hist_isdigit(line[i]))
		return;
	for (; i < len && hist_isdigit(line[i]); i++) {
		if (++n > 6)
			return;		/* not an optics figure */
		ip = ip * 10 + (unsigned long)(line[i] - '0');
	}
	if (i < len && line[i] == '.') {
		for (i++; i < len && hist_isdigit(line[i]); i++, fd++) {
			if (fd < 2)
				frac = frac * 10 + (unsigned long)(line[i] - '0');
			else if (fd == 2)
				round = line[i] >= '5';
		}
	}
	if (i < len && line[i] != ' ' && line[i] != '\t')
		return;
	while (fd < 2) {
		frac *= 10;
		fd++;
	}
	frac += ip * 100 + (unsigned long)round;
	n = 0;
	if (neg && frac)
		out[n++] = '-';
	{
		char d[12];
		unsigned long k = 0, w = frac / 100, j;

		do {
			d[k++] = (char)('0' + w % 10);
			w /= 10;
		} while (w);
		for (j = 0; j < k; j++)
			out[n++] = d[k - 1 - j];
	}
	out[n++] = '.';
	out[n++] = (char)('0' + (frac / 10) % 10);
	out[n++] = (char)('0' + frac % 10);
	hist_val_setn(v, out, n);
}

/* ---- one sample -------------------------------------------------------- */

struct hist_sample {
	struct hist_val t, up, st, rx, tx, tc, bi, di, dout;
	struct hist_val los, lof, lom, o5i, o5o, bip, fecu, cb, ct, ma;
	char al[64];		/* "-", "LOS,LOF", or empty when diag gave none */
};

static void hist_from_diag(struct hist_sample *s, const char *diag)
{
	const char *line, *f;
	unsigned long len, n, pos = 0, i;
	char cmd[48];
	int port = -1, seen = 0;
	unsigned long al = 0;

	cmd[0] = 0;
	while (hist_next_line(diag, &pos, &line, &len)) {
		if (hist_pre(line, len, "RTK.0> ")) {
			n = len - 7;
			if (n > sizeof(cmd) - 1)
				n = sizeof(cmd) - 1;
			for (i = 0; i < n; i++)
				cmd[i] = line[7 + i];
			cmd[n] = 0;
			port = -1;
			continue;
		}
		if (!hist_len(cmd))
			continue;
		if (hist_pre(cmd, hist_len(cmd), "pon get transceiver ")) {
			const char *what = cmd + 20;
			struct hist_val *v =
				hist_pre(what, hist_len(what), "rx-power") ? &s->rx :
				hist_pre(what, hist_len(what), "tx-power") ? &s->tx :
				hist_pre(what, hist_len(what), "temperature") ? &s->tc :
				hist_pre(what, hist_len(what), "bias-current") ? &s->bi : 0;

			if (v && !v->set)
				hist_fig(v, line, len);
		} else if (hist_pre(cmd, hist_len(cmd), "gpon get onu-state")) {
			for (i = 0; i + 3 < len; i++)
				if (line[i] == '(' && line[i + 1] == 'O' &&
				    line[i + 2] >= '1' && line[i + 2] <= '7' && line[i + 3] == ')')
					hist_val_setn(&s->st, line + i + 2, 1);
		} else if (hist_pre(cmd, hist_len(cmd), "gpon get alarm-status")) {
			int clear = 0;

			if (!hist_pre(line, len, "Alarm "))
				continue;
			seen = 1;
			for (i = 0; i + 5 <= len; i++)
				if (hist_pre(line + i, len - i, "clear"))
					clear = 1;
			if (clear || !hist_field(line, len, 1, &f, &n))
				continue;
			if (n && f[n - 1] == ',')
				n--;
			if (al + n + 2 >= sizeof(s->al))
				continue;
			if (al)
				s->al[al++] = ',';
			for (i = 0; i < n; i++)
				s->al[al++] = f[i];
			s->al[al] = 0;
		} else if (hist_pre(cmd, hist_len(cmd), "mib dump counter port")) {
			if (hist_pre(line, len, "Port:")) {
				port = hist_field(line, len, 1, &f, &n) && n == 1 && f[0] == '2' ? 2 : -1;
				continue;
			}
			if (port != 2 || !hist_field(line, len, 0, &f, &n))
				continue;
			{
				const char *v;
				unsigned long vn;
				unsigned idx = 1;

				/* "ifInOctets : 1000": the value is the last field */
				while (hist_field(line, len, idx + 1, &v, &vn))
					idx++;
				if (!hist_field(line, len, idx, &v, &vn) || !hist_all_digits(v, vn))
					continue;
				if (n == 10 && hist_pre(f, n, "ifInOctets"))
					hist_val_setn(&s->di, v, vn);
				else if (n == 11 && hist_pre(f, n, "ifOutOctets"))
					hist_val_setn(&s->dout, v, vn);
			}
		}
	}
	if (seen && !al) {
		s->al[0] = '-';
		s->al[1] = 0;
	}
}

/*
 * The O5 transitions are only known when the driver printed its counter
 * block: the onu_state_ms lines are always in it, a transition line only
 * for a pair that happened. Without the block (the driver could not take
 * its snapshot) o5i and o5o are left out, not written as 0, or the next
 * sample would show every old drop as new.
 */
static void hist_from_status(struct hist_sample *s, const char *text)
{
	const char *line, *f, *g;
	unsigned long len, n, gn, pos = 0;
	unsigned long long in5 = 0, out5 = 0;
	int block = 0;

	while (hist_next_line(text, &pos, &line, &len)) {
		if (hist_field_is(line, len, 0, "state") && hist_field(line, len, 1, &f, &n) &&
		    n == 1 && f[0] >= '1' && f[0] <= '7') {
			if (!s->st.set)
				hist_val_setn(&s->st, f, 1);
		} else if (hist_field_is(line, len, 0, "onu_state_ms")) {
			block = 1;
		} else if (hist_field_is(line, len, 0, "alarm_events") &&
			   hist_field(line, len, 2, &g, &gn) && hist_all_digits(g, gn)) {
			struct hist_val *v = hist_field_is(line, len, 1, "los") ? &s->los :
					     hist_field_is(line, len, 1, "lof") ? &s->lof :
					     hist_field_is(line, len, 1, "lom") ? &s->lom : 0;
			if (v)
				hist_val_setn(v, g, gn);
		} else if (hist_field_is(line, len, 0, "onu_transitions") &&
			   hist_field(line, len, 3, &g, &gn) && hist_all_digits(g, gn)) {
			unsigned long long c = parse_u64(g, gn);

			block = 1;
			if (hist_field_is(line, len, 2, "O5"))
				in5 += c;
			if (hist_field_is(line, len, 1, "O5"))
				out5 += c;
		}
	}
	if (block) {
		hist_val_u64(&s->o5i, in5);
		hist_val_u64(&s->o5o, out5);
	}
}

static void hist_from_counters(struct hist_sample *s, const char *text)
{
	const char *line, *g;
	unsigned long len, gn, pos = 0;

	while (hist_next_line(text, &pos, &line, &len)) {
		if (!hist_field_is(line, len, 0, "ctr") || !hist_field(line, len, 2, &g, &gn) ||
		    !hist_all_digits(g, gn))
			continue;
		if (hist_field_is(line, len, 1, "ds_bip_err_bits"))
			hist_val_setn(&s->bip, g, gn);
		else if (hist_field_is(line, len, 1, "ds_fec_uncorrectable_cw"))
			hist_val_setn(&s->fecu, g, gn);
	}
}

/* The first line of /proc/stat, "cpu  user nice system idle iowait ...". */
static void hist_from_stat(struct hist_sample *s, const char *text)
{
	const char *line, *f;
	unsigned long len, n, pos = 0;
	unsigned long long tot = 0, idle = 0;
	unsigned k;

	if (!hist_next_line(text, &pos, &line, &len) || !hist_field_is(line, len, 0, "cpu"))
		return;
	for (k = 1; hist_field(line, len, k, &f, &n); k++) {
		unsigned long long v;

		if (!hist_all_digits(f, n))
			return;
		v = parse_u64(f, n);
		tot += v;
		if (k == 4 || k == 5)
			idle += v;
	}
	if (k < 5)
		return;
	hist_val_u64(&s->cb, tot - idle);
	hist_val_u64(&s->ct, tot);
}

static void hist_from_meminfo(struct hist_sample *s, const char *text)
{
	const char *line, *f;
	unsigned long len, n, pos = 0;

	while (hist_next_line(text, &pos, &line, &len))
		if (hist_field_is(line, len, 0, "MemAvailable:") &&
		    hist_field(line, len, 1, &f, &n) && hist_all_digits(f, n))
			hist_val_setn(&s->ma, f, n);
}

/* Append s; 0 when it did not fit (out then holds a cut line). */
static int hist_put(char *out, unsigned long cap, unsigned long *len, const char *s)
{
	while (*s && *len + 1 < cap)
		out[(*len)++] = *s++;
	out[*len] = 0;
	return !*s;
}

static int hist_pair(char *out, unsigned long cap, unsigned long *len,
		     const char *k, const char *v)
{
	if (!v || !v[0])
		return 1;
	return (!*len || hist_put(out, cap, len, " ")) && hist_put(out, cap, len, k) &&
	       hist_put(out, cap, len, "=") && hist_put(out, cap, len, v);
}

#define HIST_PAIR(k, val) (ok &= hist_pair(out, cap, &n, k, (val).set ? (val).s : 0))

/* One sample as its line, without the newline; returns its length, or 0
 * (and an empty out) when the line does not fit cap: a cut line could end
 * in a number missing its last digits. Diag first for the ONU state, so
 * the driver only fills it in when diag gave none (a diag that timed out
 * still leaves the kernel view). */
static unsigned long hist_build_line(const struct hist_in *in, char *out, unsigned long cap)
{
	struct hist_sample s;
	unsigned long n = 0, i;
	char *z = (char *)&s;
	int ok = 1;

	for (i = 0; i < sizeof(s); i++)
		z[i] = 0;
	out[0] = 0;
	if (in->wall >= 0)
		hist_val_u64(&s.t, (unsigned long long)in->wall);
	if (in->uptime) {
		for (i = 0; hist_isdigit(in->uptime[i]); i++)
			;
		if (i && i <= 20)
			hist_val_setn(&s.up, in->uptime, i);
	}
	if (in->diag)
		hist_from_diag(&s, in->diag);
	if (in->gpon_status)
		hist_from_status(&s, in->gpon_status);
	if (in->gpon_counters)
		hist_from_counters(&s, in->gpon_counters);
	if (in->stat)
		hist_from_stat(&s, in->stat);
	if (in->meminfo)
		hist_from_meminfo(&s, in->meminfo);

	HIST_PAIR("t", s.t);
	HIST_PAIR("up", s.up);
	HIST_PAIR("st", s.st);
	ok &= hist_pair(out, cap, &n, "al", s.al);
	HIST_PAIR("rx", s.rx);
	HIST_PAIR("tx", s.tx);
	HIST_PAIR("tc", s.tc);
	HIST_PAIR("bi", s.bi);
	HIST_PAIR("di", s.di);
	HIST_PAIR("do", s.dout);
	HIST_PAIR("los", s.los);
	HIST_PAIR("lof", s.lof);
	HIST_PAIR("lom", s.lom);
	HIST_PAIR("o5i", s.o5i);
	HIST_PAIR("o5o", s.o5o);
	HIST_PAIR("bip", s.bip);
	HIST_PAIR("fecu", s.fecu);
	HIST_PAIR("cb", s.cb);
	HIST_PAIR("ct", s.ct);
	HIST_PAIR("ma", s.ma);
	if (!ok) {
		out[0] = 0;
		return 0;
	}
	return n;
}

#undef HIST_PAIR

/* ---- the ring ---------------------------------------------------------- */

struct hist_ring {
	char line[HIST_KEEP][HIST_LINE];
	unsigned long n;	/* lines held */
	unsigned long first;	/* index of the oldest */
};

/* Add a line (len bytes, no newline), dropping the oldest when full. A line
 * too long for a slot is dropped rather than cut, so a reader never sees a
 * key cut in half. */
static void hist_ring_push(struct hist_ring *r, const char *s, unsigned long len)
{
	unsigned long slot, i;

	if (!len || len >= HIST_LINE)
		return;
	if (r->n < HIST_KEEP) {
		slot = (r->first + r->n) % HIST_KEEP;
		r->n++;
	} else {
		slot = r->first;
		r->first = (r->first + 1) % HIST_KEEP;
	}
	for (i = 0; i < len; i++)
		r->line[slot][i] = s[i];
	r->line[slot][len] = 0;
}

/*
 * What a ring file already holds, oldest first: so a restarted metricsd
 * carries on the history of this boot instead of starting it again. Only
 * lines with an up= of plain digits are taken; anything else is dropped.
 */
static void hist_ring_load(struct hist_ring *r, const char *text)
{
	const char *line;
	unsigned long len, pos = 0, i;

	while (hist_next_line(text, &pos, &line, &len)) {
		int ok = 0;

		const char *f;
		unsigned long n;

		for (i = 0; hist_field(line, len, (unsigned)i, &f, &n); i++)
			if (n > 3 && hist_pre(f, n, "up=") && hist_all_digits(f + 3, n - 3))
				ok = 1;
		if (ok)
			hist_ring_push(r, line, len);
	}
}

/* The up= of the newest line, or -1: a metricsd that takes the history
 * over samples a minute after it, not at once. */
static long hist_ring_last_up(const struct hist_ring *r)
{
	const char *s, *f;
	unsigned long len, n, k;

	if (!r->n)
		return -1;
	s = r->line[(r->first + r->n - 1) % HIST_KEEP];
	len = hist_len(s);
	for (k = 0; hist_field(s, len, k, &f, &n); k++)
		if (n > 3 && n <= 12 && hist_pre(f, n, "up=") && hist_all_digits(f + 3, n - 3))
			return (long)parse_u64(f + 3, n - 3);
	return -1;
}

/* The k-th line held, oldest first (k < r->n): the file is these lines,
 * each with a newline. */
static const char *hist_ring_line(const struct hist_ring *r, unsigned long k)
{
	return r->line[(r->first + k) % HIST_KEEP];
}

#endif /* ODI_HISTORY_H */
