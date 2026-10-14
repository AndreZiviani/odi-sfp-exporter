/*
 * The GEM flows, as metrics: what each GEM port carries, downstream and
 * upstream, from /proc/odi_gpon/flows of the odi-oss kernel driver. One GEM
 * port that goes quiet on a multi-service line (IPTV, VoIP) shows here and
 * not in the aggregate counters of gpon_link.h.
 *
 * Like the link counters, a read of the file and nothing more: the hardware
 * registers behind it clear when read, so the driver is their one reader and
 * keeps 64-bit totals since each flow was created (odi-oss docs/TOOLS.md has
 * the full format). A flow created again starts from zero, which a counter
 * reset covers in rate(). The file is optional: the stock firmware, and an
 * odi-oss kernel from before it, have none, and then nothing is emitted.
 *
 * The lines, one flow each, a flow index (the driver's, not the GEM port)
 * then the GEM port and name/value pairs:
 *
 *     ds <flow> gem <port> gem_frames <n> bytes <n> eth_rx <n> eth_fwd <n>
 *     us <flow> gem <port> gem_frames <n> eth_frames <n> bytes <n>
 *     flow_ack_timeouts <n>
 *     flow_saturated <n>
 *
 * The metrics are labelled by direction and GEM port, not by flow index, so
 * a flow keeps its series when the index changes. Should two flows of one
 * direction carry the same port, the first line wins: a series must be
 * unique in a scrape. A value that is not 1 to 20 digits drops its sample,
 * a name the table does not know is skipped (a newer kernel can add one), and
 * a last line without its newline is ignored, since a read that stopped
 * short can cut it anywhere.
 *
 * Pure text in, text out, through the provision.h writer: no syscalls, so
 * test/test_gpon_flow.c runs it natively against fixtures.
 */
#ifndef ODI_GPON_FLOW_H
#define ODI_GPON_FLOW_H

#include "provision.h"

struct flow_row {
	const char *dir;	/* "ds" or "us": the first word of the line */
	const char *field;	/* the name whose value is the sample */
	const char *kind;	/* a label to add, or "" */
};

struct flow_fam {
	const char *name;
	const char *type;
	const char *help;
	struct flow_row rows[4];	/* ended by a row with no dir */
};

static const struct flow_fam flow_fams[] = {
	{ "gpon_gem_frames_total", "counter",
	  "GEM frames on a GEM port since the flow was created: received downstream, sent upstream. A GEM frame is a fragment of an Ethernet frame or a whole one.",
	  { { "ds", "gem_frames", "" }, { "us", "gem_frames", "" } } },
	{ "gpon_gem_bytes_total", "counter",
	  "Bytes on a GEM port since the flow was created: received downstream, sent upstream.",
	  { { "ds", "bytes", "" }, { "us", "bytes", "" } } },
	{ "gpon_gem_eth_frames_total", "counter",
	  "Ethernet frames on a GEM port since the flow was created, by kind: rx and fwd downstream (received from the GEM layer, forwarded to the switch), tx upstream.",
	  { { "ds", "eth_rx", "kind=\"rx\"" }, { "ds", "eth_fwd", "kind=\"fwd\"" },
	    { "us", "eth_frames", "kind=\"tx\"" } } },
};

#define FLOW_NFAMS (sizeof(flow_fams) / sizeof(flow_fams[0]))

/* "ack_timeout" and "saturated": the driver's own tallies, one value each. */
struct flow_fault {
	const char *key;
	const char *kind;
};

static const struct flow_fault flow_faults[] = {
	{ "flow_ack_timeouts", "ack_timeout" },
	{ "flow_saturated", "saturated" },
};

#define FLOW_NFAULTS (sizeof(flow_faults) / sizeof(flow_faults[0]))

#define FLOW_WORDS 12	/* "ds 1 gem 1434" and up to four pairs, with room */

/* One line of the file, in words. */
struct flow_line {
	const char *w[FLOW_WORDS];
	unsigned long n[FLOW_WORDS];
	int count;
};

static void flow_split(const char *l, unsigned long len, struct flow_line *fl)
{
	unsigned long i = 0;

	fl->count = 0;
	while (i < len && fl->count < FLOW_WORDS) {
		while (i < len && l[i] == ' ')
			i++;
		if (i >= len)
			break;
		fl->w[fl->count] = l + i;
		while (i < len && l[i] != ' ')
			i++;
		fl->n[fl->count] = (unsigned long)(l + i - fl->w[fl->count]);
		fl->count++;
	}
}

static int flow_word_is(const struct flow_line *fl, int i, const char *s)
{
	unsigned long k = 0;

	if (i >= fl->count)
		return 0;
	while (k < fl->n[i] && s[k] && fl->w[i][k] == s[k])
		k++;
	return k == fl->n[i] && !s[k];
}

/* A count: 1 to 20 digits, the width of a 64-bit total. (provision.h's
 * prov_digits stops at 10, which a byte total passes within hours.) */
static int flow_count_ok(const char *s, unsigned long n)
{
	unsigned long i;

	for (i = 0; i < n; i++)
		if (s[i] < '0' || s[i] > '9')
			return 0;
	return n > 0 && n <= 20;
}

/* A line that is a flow of direction dir: "<dir> <flow> gem <port> ...",
 * the flow and the port plain numbers. The port is then w[3]. */
static int flow_parse(const char *l, unsigned long n, const char *dir,
		      struct flow_line *fl)
{
	flow_split(l, n, fl);
	return fl->count >= 4 && flow_word_is(fl, 0, dir) &&
	       flow_count_ok(fl->w[1], fl->n[1]) && flow_word_is(fl, 2, "gem") &&
	       flow_count_ok(fl->w[3], fl->n[3]) && fl->n[3] <= 5;
}

/* The value after the name `field` on a parsed flow line, or 0. */
static int flow_value(const struct flow_line *fl, const char *field,
		      const char **v, unsigned long *vn)
{
	int i;

	for (i = 4; i + 1 < fl->count; i += 2)
		if (flow_word_is(fl, i, field)) {
			*v = fl->w[i + 1];
			*vn = fl->n[i + 1];
			return flow_count_ok(*v, *vn);
		}
	return 0;
}

static int flow_same(const char *a, const char *b, unsigned long n)
{
	unsigned long i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* Whether a line before `upto` of the same direction already names the port
 * of the parsed line fl. */
static int flow_port_seen(const char *proc, const char *upto, const char *dir,
			  const struct flow_line *fl)
{
	unsigned long i = 0, end = (unsigned long)(upto - proc);

	while (i < end) {
		unsigned long n = prov_line_end(proc + i);
		struct flow_line prev;

		if (flow_parse(proc + i, n, dir, &prev) && prev.n[3] == fl->n[3] &&
		    flow_same(prev.w[3], fl->w[3], fl->n[3]))
			return 1;
		i += n + 1;
	}
	return 0;
}

/* The name{labels} of a sample, for direction, port and kind. */
static void flow_put_name(const struct prov_out *o, const char *name, const char *dir,
			  const char *port, unsigned long portn, const char *kind)
{
	prov_puts(o, name);
	prov_puts(o, "{direction=\"");
	prov_puts(o, dir);
	prov_puts(o, "\",gem_port=\"");
	prov_putn(o, port, portn);
	prov_puts(o, "\"");
	if (kind[0]) {
		prov_puts(o, ",");
		prov_puts(o, kind);
	}
	prov_puts(o, "} ");
}

static void flow_put_header(const struct prov_out *o, const char *name, const char *type,
			    const char *help)
{
	prov_puts(o, "# HELP ");
	prov_puts(o, name);
	prov_puts(o, " ");
	prov_puts(o, help);
	prov_puts(o, "\n# TYPE ");
	prov_puts(o, name);
	prov_puts(o, " ");
	prov_puts(o, type);
	prov_puts(o, "\n");
}

/* One sample from the line at proc + i, if the row matches it; 1 when it
 * did. With dry set it only says so. */
static int flow_row_sample(const struct prov_out *o, const char *proc,
			   const struct flow_fam *fam, const struct flow_row *r,
			   unsigned long i, int dry)
{
	unsigned long n = prov_line_end(proc + i);
	struct flow_line fl;
	const char *v;
	unsigned long vn;

	if (proc[i + n] != '\n')	/* a line the read cut short */
		return 0;
	if (!flow_parse(proc + i, n, r->dir, &fl) || !flow_value(&fl, r->field, &v, &vn))
		return 0;
	if (flow_port_seen(proc, proc + i, r->dir, &fl))
		return 0;
	if (dry)
		return 1;
	flow_put_name(o, fam->name, r->dir, fl.w[3], fl.n[3], r->kind);
	prov_putn(o, v, vn);
	prov_puts(o, "\n");
	return 1;
}

static void flow_emit(const struct prov_out *o, const char *proc)
{
	unsigned long f, k;

	for (f = 0; f < FLOW_NFAMS; f++) {
		const struct flow_fam *fam = &flow_fams[f];
		const struct flow_row *r;
		int any = 0;

		for (r = fam->rows; r->dir; r++) {
			unsigned long i = 0;

			while (proc[i]) {
				unsigned long n = prov_line_end(proc + i);

				if (flow_row_sample(o, proc, fam, r, i, 1)) {
					if (!any)
						flow_put_header(o, fam->name, fam->type, fam->help);
					any = 1;
					flow_row_sample(o, proc, fam, r, i, 0);
				}
				i += n;
				if (proc[i])
					i++;
			}
		}
	}

	/* The driver's own tallies of a count it skipped or read full. */
	{
		const char *name = "gpon_gem_flow_count_faults_total";
		int any = 0;

		for (k = 0; k < FLOW_NFAULTS; k++) {
			unsigned long i = 0;

			while (proc[i]) {
				unsigned long n = prov_line_end(proc + i);
				struct flow_line fl;

				flow_split(proc + i, n, &fl);
				if (proc[i + n] == '\n' && fl.count == 2 &&
				    flow_word_is(&fl, 0, flow_faults[k].key) &&
				    flow_count_ok(fl.w[1], fl.n[1])) {
					if (!any)
						flow_put_header(o, name, "counter",
								"Per-flow counts the driver could not use: ack_timeout, a count that never answered and stays for the next second; saturated, a count read full, which lost counts.");
					any = 1;
					prov_puts(o, name);
					prov_puts(o, "{kind=\"");
					prov_puts(o, flow_faults[k].kind);
					prov_puts(o, "\"} ");
					prov_putn(o, fl.w[1], fl.n[1]);
					prov_puts(o, "\n");
				}
				i += n;
				if (proc[i])
					i++;
			}
		}
	}
}

#endif /* ODI_GPON_FLOW_H */
