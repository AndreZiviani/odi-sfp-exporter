/*
 * What the OLT provisioned, as metrics: gpon_provision_*.
 *
 * gpon_omci_services says whether a service exists; these say which one. An
 * ISP plan change -- a new speed tier, a moved VLAN, another T-CONT -- shows
 * up here as a label or a value that changed, where every other metric keeps
 * reporting a healthy O5.
 *
 * Two sources, neither of them a new transport:
 *
 *  - /proc/odi_gpon, its `alloc_ids <n> <id>...` line: the Alloc-IDs the OLT
 *    assigned by PLOAM (Assign_Alloc-ID), from the odi-oss kernel driver. A
 *    read of a /proc file, no fork. These are the T-CONTs.
 *  - `omcicli provision`, one short fork like `omcicli dump srvflow`, bounded
 *    the same way: omcid's summary of its MIB, one line per item --
 *
 *      gem me=<id> port=<n> direction=<1|2|3> tcont_me=<id> us_td=<id> ds_td=<id>
 *      vlan vid=<n> source=<vlan_filter|ext_vlan_filter|ext_vlan_treatment>
 *      td me=<id> cir=<B/s> pir=<B/s> cbs=<B> pbs=<B>
 *      summary rows=<n> ... gem_ports=<n> ... traffic_descriptors=<n> ... mib_data_sync=<n>
 *
 *    (and `tcont` lines, which are omcid's view and not exported: the kernel
 *    list above is the OLT's own assignment).
 *
 * Either source missing -- a stock kernel, the vendor omci_app, which has no
 * `provision` command -- means those families are simply absent, never
 * emitted empty or as zero.
 *
 * Cardinality is the provisioning itself: a handful of GEM ports, T-CONTs,
 * VLANs and descriptors per ONU, each a label value that only changes when
 * the ISP changes the service.
 *
 * Pure text in, text out, through a caller-supplied writer: no syscalls, so
 * test/test_provision.c runs it natively against fixtures (the same reason
 * wrap.h is its own header).
 */
#ifndef ODI_PROVISION_H
#define ODI_PROVISION_H

typedef void (*prov_put_fn)(void *ctx, const char *s, unsigned long n);

struct prov_out {
	prov_put_fn put;
	void *ctx;
};

static unsigned long prov_len(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static void prov_puts(const struct prov_out *o, const char *s)
{
	o->put(o->ctx, s, prov_len(s));
}

static void prov_putn(const struct prov_out *o, const char *s, unsigned long n)
{
	o->put(o->ctx, s, n);
}

static void prov_header(const struct prov_out *o, const char *name,
			const char *help)
{
	prov_puts(o, "# HELP ");
	prov_puts(o, name);
	prov_puts(o, " ");
	prov_puts(o, help);
	prov_puts(o, "\n# TYPE ");
	prov_puts(o, name);
	prov_puts(o, " gauge\n");
}

/* The line starting at `s` runs to the next newline or NUL. */
static unsigned long prov_line_end(const char *s)
{
	unsigned long i = 0;

	while (s[i] && s[i] != '\n')
		i++;
	return i;
}

/* Whether the line at `s` (length `n`) starts with the word `w` and a space. */
static int prov_is(const char *s, unsigned long n, const char *w)
{
	unsigned long k = 0;

	while (w[k]) {
		if (k >= n || s[k] != w[k])
			return 0;
		k++;
	}
	return k < n && s[k] == ' ';
}

/* The value of " key=" in the line: its start and length, 0 when absent.
 * Only [0-9a-z_] counts -- a value is a decimal or a lower-case word, and
 * anything else is refused rather than put into a label. */
static int prov_val(const char *s, unsigned long n, const char *key,
		    unsigned long *vs, unsigned long *vl)
{
	unsigned long kl = prov_len(key), i, j;

	for (i = 0; i + kl + 1 < n; i++) {
		if (s[i] != ' ')
			continue;
		for (j = 0; j < kl && s[i + 1 + j] == key[j]; j++)
			;
		if (j != kl || s[i + 1 + kl] != '=')
			continue;
		j = i + 2 + kl;
		*vs = j;
		while (j < n && ((s[j] >= '0' && s[j] <= '9') ||
				 (s[j] >= 'a' && s[j] <= 'z') || s[j] == '_'))
			j++;
		if (j < n && s[j] != ' ')
			return 0;
		*vl = j - *vs;
		return *vl > 0;
	}
	return 0;
}

static int prov_digits(const char *s, unsigned long n)
{
	unsigned long i;

	for (i = 0; i < n; i++)
		if (s[i] < '0' || s[i] > '9')
			return 0;
	return n > 0 && n <= 10;
}

/* ------------------------------------------------ /proc/odi_gpon: T-CONTs */

static void prov_emit_tconts(const struct prov_out *o, const char *proc)
{
	unsigned long i = 0;

	while (proc[i]) {
		unsigned long n = prov_line_end(proc + i);
		const char *l = proc + i;

		if (prov_is(l, n, "alloc_ids")) {
			unsigned long p = 10, s, cnt;

			s = p;
			while (p < n && l[p] >= '0' && l[p] <= '9')
				p++;
			if (!prov_digits(l + s, p - s))
				return;
			cnt = p - s;
			prov_header(o, "gpon_provision_tconts",
				    "T-CONTs (Alloc-IDs) the OLT assigned by PLOAM, from /proc/odi_gpon.");
			prov_puts(o, "gpon_provision_tconts ");
			prov_putn(o, l + s, cnt);
			prov_puts(o, "\n");
			if (p >= n)
				return;
			prov_header(o, "gpon_provision_tcont_info",
				    "Always 1, one series per Alloc-ID the OLT assigned; a plan change that adds or moves a T-CONT changes the label.");
			while (p < n) {
				while (p < n && l[p] == ' ')
					p++;
				s = p;
				while (p < n && l[p] >= '0' && l[p] <= '9')
					p++;
				if (!prov_digits(l + s, p - s))
					break;
				prov_puts(o, "gpon_provision_tcont_info{alloc_id=\"");
				prov_putn(o, l + s, p - s);
				prov_puts(o, "\"} 1\n");
			}
			return;
		}
		i += n;
		if (proc[i])
			i++;
	}
}

/* ------------------------------------------- omcicli provision: the rest */

/* One family from one kind of line: `word` picks the lines, and emit writes
 * one sample from each. The header goes out before the first sample only,
 * so a family with no lines is absent rather than empty. */
typedef int (*prov_line_fn)(const struct prov_out *o, const char *l, unsigned long n,
			    int dry);

static void prov_family(const struct prov_out *o, const char *text,
			const char *word, const char *name, const char *help,
			prov_line_fn fn)
{
	unsigned long i = 0;
	int any = 0;

	while (text[i]) {
		unsigned long n = prov_line_end(text + i);

		if (prov_is(text + i, n, word) && fn(o, text + i, n, 1)) {
			if (!any)
				prov_header(o, name, help);
			any = 1;
			fn(o, text + i, n, 0);
		}
		i += n;
		if (text[i])
			i++;
	}
}

static int prov_gem(const struct prov_out *o, const char *l, unsigned long n, int dry)
{
	unsigned long ps, pl, ds, dl;
	const char *dir;

	if (!prov_val(l, n, "port", &ps, &pl) || !prov_digits(l + ps, pl) ||
	    !prov_val(l, n, "direction", &ds, &dl) || dl != 1)
		return 0;
	dir = l[ds] == '1' ? "upstream" : l[ds] == '2' ? "downstream" :
	      l[ds] == '3' ? "bidirectional" : 0;
	if (!dir)
		return 0;
	if (dry)
		return 1;
	prov_puts(o, "gpon_provision_gem_port_info{gem_port=\"");
	prov_putn(o, l + ps, pl);
	prov_puts(o, "\",direction=\"");
	prov_puts(o, dir);
	prov_puts(o, "\"} 1\n");
	return 1;
}

static int prov_vlan(const struct prov_out *o, const char *l, unsigned long n, int dry)
{
	unsigned long vs, vl, ss, sl;

	if (!prov_val(l, n, "vid", &vs, &vl) || !prov_digits(l + vs, vl) ||
	    !prov_val(l, n, "source", &ss, &sl))
		return 0;
	if (dry)
		return 1;
	prov_puts(o, "gpon_provision_vlan_info{vlan=\"");
	prov_putn(o, l + vs, vl);
	prov_puts(o, "\",source=\"");
	prov_putn(o, l + ss, sl);
	prov_puts(o, "\"} 1\n");
	return 1;
}

static int prov_td_rate(const struct prov_out *o, const char *l, unsigned long n,
			int dry, const char *key, const char *metric)
{
	unsigned long ms, ml, vs, vl;

	if (!prov_val(l, n, "me", &ms, &ml) || !prov_digits(l + ms, ml) ||
	    !prov_val(l, n, key, &vs, &vl) || !prov_digits(l + vs, vl))
		return 0;
	if (dry)
		return 1;
	prov_puts(o, metric);
	prov_puts(o, "{descriptor=\"");
	prov_putn(o, l + ms, ml);
	prov_puts(o, "\"} ");
	prov_putn(o, l + vs, vl);
	prov_puts(o, "\n");
	return 1;
}

static int prov_td_cir(const struct prov_out *o, const char *l, unsigned long n, int dry)
{
	return prov_td_rate(o, l, n, dry, "cir",
			    "gpon_provision_traffic_descriptor_cir_bytes_per_second");
}

static int prov_td_pir(const struct prov_out *o, const char *l, unsigned long n, int dry)
{
	return prov_td_rate(o, l, n, dry, "pir",
			    "gpon_provision_traffic_descriptor_pir_bytes_per_second");
}

/* One number off the summary line, as a plain gauge. */
static void prov_summary(const struct prov_out *o, const char *text,
			 const char *key, const char *name, const char *help)
{
	unsigned long i = 0;

	while (text[i]) {
		unsigned long n = prov_line_end(text + i), vs, vl;

		if (prov_is(text + i, n, "summary") &&
		    prov_val(text + i, n, key, &vs, &vl) &&
		    prov_digits(text + i + vs, vl)) {
			prov_header(o, name, help);
			prov_puts(o, name);
			prov_puts(o, " ");
			prov_putn(o, text + i + vs, vl);
			prov_puts(o, "\n");
			return;
		}
		i += n;
		if (text[i])
			i++;
	}
}

static void prov_emit_omci(const struct prov_out *o, const char *text)
{
	prov_summary(o, text, "gem_ports", "gpon_provision_gem_ports",
		     "GEM ports the OLT created (GEM port network CTP), from `omcicli provision`.");
	prov_family(o, text, "gem", "gpon_provision_gem_port_info",
		    "Always 1, one series per GEM port the OLT created, with its direction.",
		    prov_gem);
	prov_family(o, text, "vlan", "gpon_provision_vlan_info",
		    "Always 1, one series per VLAN the OLT provisioned and where: vlan_filter (VLAN tagging filter data), ext_vlan_filter or ext_vlan_treatment (extended VLAN tagging operation, the VID matched or the VID set).",
		    prov_vlan);
	prov_summary(o, text, "traffic_descriptors", "gpon_provision_traffic_descriptors",
		     "Traffic descriptors (ME 280) the OLT created: its rate limits.");
	prov_family(o, text, "td", "gpon_provision_traffic_descriptor_cir_bytes_per_second",
		    "Committed information rate of each traffic descriptor the OLT created, bytes per second (G.988 9.2.12).",
		    prov_td_cir);
	prov_family(o, text, "td", "gpon_provision_traffic_descriptor_pir_bytes_per_second",
		    "Peak information rate of each traffic descriptor the OLT created, bytes per second (G.988 9.2.12).",
		    prov_td_pir);
	prov_summary(o, text, "rows", "gpon_provision_mib_entities",
		     "Managed entities the OLT created or wrote, held by omcid.");
	prov_summary(o, text, "mib_data_sync", "gpon_provision_mib_data_sync",
		     "The MIB data sync counter: moves on every Create, Set or Delete from the OLT (one byte, so it wraps), back to 0 on a MIB reset.");
}

#endif /* ODI_PROVISION_H */
