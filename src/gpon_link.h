/*
 * The GPON link, as metrics: the line counters of the GPON MAC, how long
 * each alarm and each ONU state lasted, and the driver's own PLOAM and
 * interrupt counts. A degraded fibre shows here (FEC corrections climbing,
 * BIP errors, uncorrectable codewords, LOS seconds), and so does a stuck ONU
 * that still says O5: the BWmap allocations addressed to it stop, or run far
 * above normal when the Alloc-ID table holds rows the OLT never assigned.
 *
 * One source, /proc/odi_gpon from the odi-oss kernel driver, already read
 * once per scrape for the T-CONTs (provision.h). No fork, and no read of a
 * hardware counter here: those clear when read, so the driver is their one
 * reader, sampling them every second into 64-bit totals. This side only
 * renders lines it finds; on a stock kernel, or an odi-oss kernel from
 * before the counters, there are none and nothing is emitted.
 *
 * The lines, one fact each (odi-oss docs/TOOLS.md has the full format):
 *
 *     ctr <name> <total>               a hardware counter, since sampling began
 *     ctr_saturated <name> <samples>   samples that read the counter full
 *     ctr_samples <n>
 *     alarm_events <los|lof|lom> <n>   clear -> set, as read
 *     alarm_ms <los|lof|lom> <ms>
 *     onu_state_ms <O1..O7> <ms>
 *     onu_transitions <from> <to> <n>
 *     fec_now <ds|us> <0|1>
 *     ds_superframe <n>
 *
 * and the older ones, from before the counters: `ploam ds_rx <n> us_tx <n>`,
 * `ploam_type <ds|us> <0xNN> <n>`, `rei_count`, `to1_expiries`,
 * `to2_expiries`, `irq_count`, `irq_spurious`, `onu_id`.
 *
 * Every family is a table row below, so the names live in one place. A
 * counter name the table does not know is skipped: a newer kernel can add
 * one without breaking an older exporter. Only digits pass as a value and
 * only [0-9A-Za-z_] as a label value; anything else drops that sample.
 *
 * Pure text in, text out, through the provision.h writer: no syscalls, so
 * test/test_gpon_link.c runs it natively against fixtures.
 */
#ifndef ODI_GPON_LINK_H
#define ODI_GPON_LINK_H

#include "provision.h"

/* link_row.arg: the second word of the line, or one of these. */
#define LINK_VALUE ((const char *)0)	/* "key <value>" */
#define LINK_ANY   ((const char *)1)	/* "key <label> <value>", label named by .labels */
#define LINK_ANY2  ((const char *)2)	/* "key <label> <label> <value>", .labels "a,b" */

struct link_row {
	const char *key;	/* the first word of the line */
	const char *arg;	/* a word after the key, its value following it */
	const char *labels;	/* label text, or the label name(s) for LINK_ANY* */
	int ms;			/* the value is milliseconds: render seconds */
};

struct link_fam {
	const char *name;
	const char *type;
	const char *help;
	struct link_row rows[5];	/* ended by a row with no key */
};

static const struct link_fam link_fams[] = {
	{ "gpon_ds_bip_error_blocks_total", "counter",
	  "Downstream frames whose BIP-8 check failed (G.984.3 8.1.3.3). The count the REI PLOAM reports to the OLT.",
	  { { "ctr", "ds_bip_err_blocks", "", 0 } } },
	{ "gpon_ds_bip_error_bits_total", "counter",
	  "Downstream bit errors the BIP-8 checks found.",
	  { { "ctr", "ds_bip_err_bits", "", 0 } } },
	{ "gpon_ds_fec_corrected_bits_total", "counter",
	  "Downstream bits the FEC decoder corrected. Grows with a weak line long before errors get through.",
	  { { "ctr", "ds_fec_corrected_bits", "", 0 } } },
	{ "gpon_ds_fec_corrected_bytes_total", "counter",
	  "Downstream bytes the FEC decoder corrected.",
	  { { "ctr", "ds_fec_corrected_bytes", "", 0 } } },
	{ "gpon_ds_fec_codewords_total", "counter",
	  "Downstream FEC codewords that needed a correction, by result: uncorrectable is data lost.",
	  { { "ctr", "ds_fec_corrected_cw", "result=\"corrected\"", 0 },
	    { "ctr", "ds_fec_uncorrectable_cw", "result=\"uncorrectable\"", 0 } } },
	{ "gpon_ds_superframe_losses_total", "counter",
	  "Losses of downstream superframe synchronisation, as the framer counts them.",
	  { { "ctr", "ds_superframe_loss", "", 0 } } },
	{ "gpon_ds_plend_total", "counter",
	  "Downstream PLend fields (G.984.3 8.1.3.5) that needed the CRC: corrected, uncorrectable, or failed.",
	  { { "ctr", "ds_plend_corrected", "result=\"corrected\"", 0 },
	    { "ctr", "ds_plend_uncorrectable", "result=\"uncorrectable\"", 0 },
	    { "ctr", "ds_plend_fail", "result=\"failed\"", 0 } } },
	{ "gpon_ds_bwmap_length_errors_total", "counter",
	  "Downstream PLend fields whose Blen (the BWmap length) was too long.",
	  { { "ctr", "ds_blen_too_long", "", 0 } } },
	{ "gpon_ds_ploam_total", "counter",
	  "Downstream PLOAM messages as the framer received them, by result.",
	  { { "ctr", "ds_ploam_accepted", "result=\"accepted\"", 0 },
	    { "ctr", "ds_ploam_crc_err", "result=\"crc_error\"", 0 },
	    { "ctr", "ds_ploam_overflow", "result=\"overflow\"", 0 } } },
	{ "gpon_ds_bwmap_allocations_total", "counter",
	  "BWmap allocations the framer accepted for this ONU. Stops when the OLT no longer grants the ONU, though it may still say O5; far above its usual rate, the Alloc-ID table matches allocations that are not this ONU's.",
	  { { "ctr", "ds_bwmap_accepted", "", 0 } } },
	{ "gpon_ds_bwmap_errors_total", "counter",
	  "Downstream BWmaps the framer rejected, by kind.",
	  { { "ctr", "ds_bwmap_crc_err", "kind=\"crc\"", 0 },
	    { "ctr", "ds_bwmap_overflow", "kind=\"overflow\"", 0 },
	    { "ctr", "ds_bwmap_invalid_0", "kind=\"invalid_0\"", 0 },
	    { "ctr", "ds_bwmap_invalid_1", "kind=\"invalid_1\"", 0 } } },
	{ "gpon_ds_activation_requests_total", "counter",
	  "Serial-number and ranging requests the framer received, broadcast ones included.",
	  { { "ctr", "ds_sn_requests", "kind=\"serial_number\"", 0 },
	    { "ctr", "ds_ranging_requests", "kind=\"ranging\"", 0 } } },
	{ "gpon_ds_gem_frames_total", "counter",
	  "Downstream GEM frames, idle and not idle.",
	  { { "ctr", "ds_gem_idle", "kind=\"idle\"", 0 },
	    { "ctr", "ds_gem_non_idle", "kind=\"non_idle\"", 0 } } },
	{ "gpon_ds_gem_hec_corrected_total", "counter",
	  "Downstream GEM headers whose HEC corrected an error.",
	  { { "ctr", "ds_gem_hec_corrected", "", 0 } } },
	{ "gpon_ds_gem_delineation_losses_total", "counter",
	  "Losses of downstream GEM frame delineation.",
	  { { "ctr", "ds_gem_los", "", 0 } } },
	{ "gpon_ds_gem_errors_total", "counter",
	  "Downstream GEM frame errors, by kind.",
	  { { "ctr", "ds_gem_len_mismatch", "kind=\"length_mismatch\"", 0 },
	    { "ctr", "ds_gem_port_mismatch", "kind=\"port_mismatch\"", 0 },
	    { "ctr", "ds_gem_over_interleave", "kind=\"over_interleave\"", 0 } } },
	{ "gpon_ds_eth_frames_total", "counter",
	  "Ethernet frames reassembled from downstream GEM, by destination kind.",
	  { { "ctr", "ds_eth_unicast", "kind=\"unicast\"", 0 },
	    { "ctr", "ds_eth_multicast", "kind=\"multicast\"", 0 } } },
	{ "gpon_ds_eth_multicast_frames_total", "counter",
	  "Downstream multicast Ethernet frames the GEM block forwarded, and those it leaked (passed with no group match).",
	  { { "ctr", "ds_eth_multicast_fwd", "action=\"forwarded\"", 0 },
	    { "ctr", "ds_eth_multicast_leak", "action=\"leaked\"", 0 } } },
	{ "gpon_ds_eth_fcs_errors_total", "counter",
	  "Downstream Ethernet frames with a bad FCS.",
	  { { "ctr", "ds_eth_fcs_err", "", 0 } } },
	{ "gpon_ds_omci_frames_total", "counter",
	  "Downstream OMCI messages the GEM block received.",
	  { { "ctr", "ds_omci", "", 0 } } },
	{ "gpon_us_bursts_total", "counter",
	  "Upstream bursts sent.",
	  { { "ctr", "us_bursts", "", 0 } } },
	{ "gpon_us_ploam_total", "counter",
	  "Upstream PLOAM messages sent, all of them.",
	  { { "ctr", "us_ploam", "", 0 } } },
	{ "gpon_us_ploam_by_source_total", "counter",
	  "Upstream PLOAM messages sent, by where they came from: the CPU queues or the framer itself.",
	  { { "ctr", "us_ploam_cpu_normal", "source=\"cpu_normal\"", 0 },
	    { "ctr", "us_ploam_cpu_urgent", "source=\"cpu_urgent\"", 0 },
	    { "ctr", "us_ploam_no_message", "source=\"auto_no_message\"", 0 },
	    { "ctr", "us_ploam_sn", "source=\"auto_serial_number\"", 0 } } },
	{ "gpon_us_dbru_total", "counter",
	  "Upstream DBRu reports sent.",
	  { { "ctr", "us_dbru", "", 0 } } },
	{ "gpon_us_gem_frames_total", "counter",
	  "Upstream GEM frames sent. A 16-bit hardware count: above 65535 frames a second it saturates (gpon_counter_saturated_samples_total).",
	  { { "ctr", "us_gem_frames", "", 0 } } },
	{ "gpon_us_gem_bytes_total", "counter",
	  "Upstream GEM bytes sent, the 5-byte GEM headers included.",
	  { { "ctr", "us_gem_bytes", "", 0 } } },
	{ "gpon_counter_saturated_samples_total", "counter",
	  "Samples that read a hardware counter full: that second lost counts.",
	  { { "ctr_saturated", LINK_ANY, "counter", 0 } } },
	{ "gpon_counter_samples_total", "counter",
	  "Reads of the GPON line counters the driver made since boot, one a second.",
	  { { "ctr_samples", LINK_VALUE, "", 0 } } },
	{ "gpon_alarm_events_total", "counter",
	  "GPON alarms raised, as the driver read the live bit: every 100 ms and at every interrupt while the ONU is activated, every second otherwise.",
	  { { "alarm_events", LINK_ANY, "alarm", 0 } } },
	{ "gpon_alarm_seconds_total", "counter",
	  "Time each GPON alarm was seen raised, to the 100 ms line poll while the ONU is activated, to the second otherwise.",
	  { { "alarm_ms", LINK_ANY, "alarm", 1 } } },
	{ "gpon_onu_state_seconds_total", "counter",
	  "Time the ONU spent in each activation state, to the second.",
	  { { "onu_state_ms", LINK_ANY, "state", 1 } } },
	{ "gpon_onu_state_transitions_total", "counter",
	  "ONU activation state changes, by state left and state entered.",
	  { { "onu_transitions", LINK_ANY2, "from,to", 0 } } },
	{ "gpon_fec_active", "gauge",
	  "1 when FEC is in use in that direction (ds: the OLT sends it; us: the ONU sends it).",
	  { { "fec_now", LINK_ANY, "direction", 0 } } },
	{ "gpon_ds_superframe", "gauge",
	  "The downstream superframe count (30 bits, one per 125 us frame); standing still means no downstream frames.",
	  { { "ds_superframe", LINK_VALUE, "", 0 } } },
	{ "gpon_ploam_messages_total", "counter",
	  "PLOAM messages the driver handled: received from the OLT, sent to it.",
	  { { "ploam", "ds_rx", "direction=\"ds\"", 0 },
	    { "ploam", "us_tx", "direction=\"us\"", 0 } } },
	{ "gpon_ploam_messages_by_type_total", "counter",
	  "PLOAM messages the driver handled, by direction and message id (G.984.3 9.2).",
	  { { "ploam_type", LINK_ANY2, "direction,type", 0 } } },
	{ "gpon_rei_sent_total", "counter",
	  "REI PLOAM messages sent, one each BER interval in O5.",
	  { { "rei_count", LINK_VALUE, "", 0 } } },
	{ "gpon_ploam_timer_expiries_total", "counter",
	  "G.984.3 activation timers that ran out: TO1 (ranging), TO2 (POPUP).",
	  { { "to1_expiries", LINK_VALUE, "timer=\"to1\"", 0 },
	    { "to2_expiries", LINK_VALUE, "timer=\"to2\"", 0 } } },
	{ "gpon_irq_total", "counter",
	  "GPON block interrupts serviced.",
	  { { "irq_count", LINK_VALUE, "", 0 } } },
	{ "gpon_irq_spurious_total", "counter",
	  "GPON block interrupts with nothing pending.",
	  { { "irq_spurious", LINK_VALUE, "", 0 } } },
	{ "gpon_onu_id", "gauge",
	  "The ONU-ID the OLT assigned; absent until it assigns one.",
	  { { "onu_id", LINK_VALUE, "", 0 } } },
};

#define LINK_NFAMS (sizeof(link_fams) / sizeof(link_fams[0]))

/* The words of the line after its first, at most four. */
struct link_words {
	const char *w[4];
	unsigned long n[4];
	int count;
};

static void link_split(const char *l, unsigned long len, struct link_words *ws)
{
	unsigned long i = 0;

	ws->count = 0;
	while (i < len && l[i] != ' ')
		i++;
	while (i < len && ws->count < 4) {
		while (i < len && l[i] == ' ')
			i++;
		if (i >= len)
			break;
		ws->w[ws->count] = l + i;
		while (i < len && l[i] != ' ')
			i++;
		ws->n[ws->count] = (unsigned long)(l + i - ws->w[ws->count]);
		ws->count++;
	}
}

static int link_word_is(const char *w, unsigned long n, const char *s)
{
	unsigned long k = 0;

	while (k < n && s[k] && w[k] == s[k])
		k++;
	return k == n && !s[k];
}

static int link_label_ok(const char *s, unsigned long n)
{
	unsigned long i;

	for (i = 0; i < n; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'z') ||
		      (s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_'))
			return 0;
	return n > 0;
}

/* A value in milliseconds, as seconds with three decimals. */
static void link_put_ms(const struct prov_out *o, const char *v, unsigned long n)
{
	unsigned long k;

	if (n > 3) {
		prov_putn(o, v, n - 3);
		prov_puts(o, ".");
		prov_putn(o, v + n - 3, 3);
		return;
	}
	prov_puts(o, "0.");
	for (k = n; k < 3; k++)
		prov_puts(o, "0");
	prov_putn(o, v, n);
}

/* The label name number `idx` of "a,b" into o. */
static void link_put_label_name(const struct prov_out *o, const char *names, int idx)
{
	unsigned long s = 0, e;

	while (idx > 0 && names[s]) {
		if (names[s] == ',')
			idx--;
		s++;
	}
	e = s;
	while (names[e] && names[e] != ',')
		e++;
	prov_putn(o, names + s, e - s);
}

/* One sample from the line, if the row matches it. Returns 1 when it did
 * (and, with dry set, only says so). */
/* The values are 64-bit totals from the driver, up to 20 digits: a
 * prov_digits() check (10 digits, for ids and VLANs) dropped any counter
 * past 9999999999, such as the upstream GEM bytes after a few hours. */
static int link_digits(const char *s, unsigned long n)
{
	unsigned long i;

	for (i = 0; i < n; i++)
		if (s[i] < '0' || s[i] > '9')
			return 0;
	return n > 0 && n <= 20;
}

static int link_row_sample(const struct prov_out *o, const char *name,
			   const struct link_row *r, const char *l, unsigned long n,
			   int dry)
{
	struct link_words ws;
	const char *v = 0;
	unsigned long vn = 0;
	int nlabels = 0, i;

	if (!prov_is(l, n, r->key))
		return 0;
	link_split(l, n, &ws);
	if (r->arg == LINK_VALUE) {
		if (ws.count < 1)
			return 0;
		v = ws.w[0];
		vn = ws.n[0];
	} else if (r->arg == LINK_ANY || r->arg == LINK_ANY2) {
		nlabels = r->arg == LINK_ANY ? 1 : 2;
		if (ws.count < nlabels + 1)
			return 0;
		for (i = 0; i < nlabels; i++)
			if (!link_label_ok(ws.w[i], ws.n[i]))
				return 0;
		v = ws.w[nlabels];
		vn = ws.n[nlabels];
	} else {
		for (i = 0; i + 1 < ws.count; i++)
			if (link_word_is(ws.w[i], ws.n[i], r->arg)) {
				v = ws.w[i + 1];
				vn = ws.n[i + 1];
				break;
			}
		if (!v)
			return 0;
	}
	if (!link_digits(v, vn))
		return 0;
	if (dry)
		return 1;

	prov_puts(o, name);
	if (nlabels) {
		prov_puts(o, "{");
		for (i = 0; i < nlabels; i++) {
			if (i)
				prov_puts(o, ",");
			link_put_label_name(o, r->labels, i);
			prov_puts(o, "=\"");
			prov_putn(o, ws.w[i], ws.n[i]);
			prov_puts(o, "\"");
		}
		prov_puts(o, "}");
	} else if (r->labels[0]) {
		prov_puts(o, "{");
		prov_puts(o, r->labels);
		prov_puts(o, "}");
	}
	prov_puts(o, " ");
	if (r->ms)
		link_put_ms(o, v, vn);
	else
		prov_putn(o, v, vn);
	prov_puts(o, "\n");
	return 1;
}

static void link_emit(const struct prov_out *o, const char *proc)
{
	unsigned long f;

	for (f = 0; f < LINK_NFAMS; f++) {
		const struct link_fam *fam = &link_fams[f];
		const struct link_row *r;
		int any = 0;

		for (r = fam->rows; r->key; r++) {
			unsigned long i = 0;

			while (proc[i]) {
				unsigned long n = prov_line_end(proc + i);

				if (link_row_sample(o, fam->name, r, proc + i, n, 1)) {
					if (!any) {
						prov_puts(o, "# HELP ");
						prov_puts(o, fam->name);
						prov_puts(o, " ");
						prov_puts(o, fam->help);
						prov_puts(o, "\n# TYPE ");
						prov_puts(o, fam->name);
						prov_puts(o, " ");
						prov_puts(o, fam->type);
						prov_puts(o, "\n");
					}
					any = 1;
					link_row_sample(o, fam->name, r, proc + i, n, 0);
				}
				i += n;
				if (proc[i])
					i++;
			}
		}
	}
}

#endif /* ODI_GPON_LINK_H */
