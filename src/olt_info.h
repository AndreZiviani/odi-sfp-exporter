/*
 * What the OLT wrote about itself, as one info metric: gpon_olt_info
 * {vendor,version,equipment_id}, from the OLT-G entity (OMCI class 131) as
 * `omcicli mib get 131` prints it, in the vendor's dump format that omcid
 * renders (odi-oss src/omci/respond/show.c, v_oltg):
 *
 *     XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 *     OltG
 *     XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
 *     =================================
 *     EntityId: 0x00
 *     OltVendorId: 0x48575443
 *     EquipId:
 *     Version: 10
 *     ToDInfo:
 *     ...
 *     1 row
 *
 * OltVendorId is `0x` and eight hex digits, the four ASCII bytes of the
 * vendor id; EquipId and Version are printed as text, byte for byte up to the
 * first zero (20 and 14 bytes at most), so an empty one is an empty value. The
 * other tool's form (`omcli mib`: padded names, hex values) is not read.
 *
 * The three lines have to be there in that order, one right after the other,
 * with the ToDInfo line after them, and each end in a newline, and none of
 * the four keys may come again after them (see olt_parse). The text is the OLT's: a newline inside EquipId
 * breaks the sequence and the family is absent, rather than letting a later
 * line that is really part of a value stand for the next attribute; a read cut
 * off in a value is not mistaken for a short value either. A value is trimmed;
 * one that is not printable ASCII, or would not be a safe label
 * (label_safe), goes out as its hex instead of being dropped, so a vendor that
 * writes binary still shows. Anything else (`0 rows`, no OLT-G yet, the stock
 * firmware's usage text, a cut) is "absent" and emits nothing.
 *
 * olt_state_key() is the cache key the caller uses to read the entity once
 * rather than on every scrape. Pure text in, text out, through the provision.h
 * writer: no syscalls, so test/test_olt_info.c runs it natively.
 */
#ifndef ODI_OLT_INFO_H
#define ODI_OLT_INFO_H

#include "image_info.h"

#define OLT_BYTES 48
/* The widths omcid prints, G.988: the renderer stops at these (show.c, v_str). */
#define OLT_EQUIP_MAX   20
#define OLT_VERSION_MAX 14

/* The three labels, already rendered: text, or hex, or empty. */
struct olt_info {
	char vendor[2 * OLT_BYTES + 1];
	char version[2 * OLT_BYTES + 1];
	char equip[2 * OLT_BYTES + 1];
};

static int olt_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Hex `h` (n chars) to a label in out: the ASCII up to the first zero byte,
 * or that prefix as lowercase hex when a byte is not printable or the text
 * would not be a safe label. 0 when h is not an even-length hex string of at
 * most OLT_BYTES bytes. */
static int olt_label(const char *h, unsigned long n, char *out)
{
	unsigned char b[OLT_BYTES];
	unsigned long i, len = 0;
	int plain = 1;

	if (n == 0 || (n & 1) || n > 2 * OLT_BYTES)
		return 0;
	for (i = 0; i < n; i += 2) {
		int hi = olt_hexval(h[i]), lo = olt_hexval(h[i + 1]);

		if (hi < 0 || lo < 0)
			return 0;
		b[i / 2] = (unsigned char)(hi * 16 + lo);
	}
	while (len < n / 2 && b[len])
		len++;
	for (i = 0; i < len; i++)
		if (b[i] < 0x20 || b[i] > 0x7e)
			plain = 0;
	if (plain) {
		for (i = 0; i < len; i++)
			out[i] = (char)b[i];
		out[len] = 0;
		if (len == 0 || label_safe(out))
			return 1;
	}
	for (i = 0; i < len; i++) {
		out[2 * i] = "0123456789abcdef"[b[i] >> 4];
		out[2 * i + 1] = "0123456789abcdef"[b[i] & 15];
	}
	out[2 * len] = 0;
	return 1;
}

/* A text value (trimmed, n bytes) to a label in out: as it is when it is
 * printable and safe, empty when empty, else its bytes as lowercase hex. 0 when
 * longer than max, which omcid never prints. */
static int olt_text(const char *v, unsigned long n, unsigned long max, char *out)
{
	unsigned long i;
	int plain = 1;

	if (n > max)
		return 0;
	for (i = 0; i < n; i++)
		if ((unsigned char)v[i] < 0x20 || (unsigned char)v[i] > 0x7e)
			plain = 0;
	for (i = 0; i < n; i++)
		out[i] = v[i];
	out[n] = 0;
	if (plain && (n == 0 || label_safe(out)))
		return 1;
	for (i = 0; i < n; i++) {
		out[2 * i] = "0123456789abcdef"[(unsigned char)v[i] >> 4];
		out[2 * i + 1] = "0123456789abcdef"[(unsigned char)v[i] & 15];
	}
	out[2 * n] = 0;
	return 1;
}

/* The line at p, "<key>:" and a value: the value, trimmed of blanks and CR,
 * and the start of the next line. 0 when the line is another one or has no
 * newline. */
static int olt_line(const char *p, const char *key, const char **v,
		    unsigned long *vn, const char **next)
{
	unsigned long k = 0, s, e;

	while (key[k] && p[k] == key[k])
		k++;
	if (key[k] || p[k] != ':')
		return 0;
	s = k + 1;
	e = s;
	while (p[e] && p[e] != '\n')
		e++;
	if (!p[e])
		return 0;
	*next = p + e + 1;
	while (s < e && (p[s] == ' ' || p[s] == '\t'))
		s++;
	while (e > s && (p[e - 1] == ' ' || p[e - 1] == '\t' || p[e - 1] == '\r'))
		e--;
	*v = p + s;
	*vn = e - s;
	return 1;
}

/* 1 when text carries the OltVendorId, EquipId and Version lines, framed by
 * ToDInfo, in o. */
static int olt_parse(const char *text, struct olt_info *o)
{
	const char *p = text, *v, *next, *q;
	unsigned long n;

	for (;;) {
		if (olt_line(p, "OltVendorId", &v, &n, &next))
			break;
		while (*p && *p != '\n')
			p++;
		if (!*p)
			return 0;
		p++;
	}
	if (n != 10 || v[0] != '0' || v[1] != 'x' || !olt_label(v + 2, 8, o->vendor))
		return 0;
	if (!(olt_line(next, "EquipId", &v, &n, &next) &&
	      olt_text(v, n, OLT_EQUIP_MAX, o->equip) &&
	      olt_line(next, "Version", &v, &n, &next) &&
	      olt_text(v, n, OLT_VERSION_MAX, o->version) &&
	      olt_line(next, "ToDInfo", &v, &n, &next)))
		return 0;
	/* omcid prints each of these once. Another one after the block is a
	 * value that held a newline and lines of its own, with the real ones
	 * behind it: refuse the block rather than believe the injected ones. */
	for (p = next; *p; ) {
		if (olt_line(p, "OltVendorId", &v, &n, &q) ||
		    olt_line(p, "EquipId", &v, &n, &q) ||
		    olt_line(p, "Version", &v, &n, &q) ||
		    olt_line(p, "ToDInfo", &v, &n, &q))
			return 0;
		while (*p && *p != '\n')
			p++;
		if (*p)
			p++;
	}
	return 1;
}

static void olt_emit(const struct prov_out *o, const struct olt_info *i)
{
	prov_header(o, "gpon_olt_info",
		    "Always 1. Labels are what the OLT wrote about itself in the OMCI "
		    "OLT-G entity (class 131), from `omcicli mib get 131`: vendor, "
		    "version and equipment_id as text, or as hex when not printable.");
	prov_puts(o, "gpon_olt_info{vendor=\"");
	prov_puts(o, i->vendor);
	prov_puts(o, "\",version=\"");
	prov_puts(o, i->version);
	prov_puts(o, "\",equipment_id=\"");
	prov_puts(o, i->equip);
	prov_puts(o, "\"} 1\n");
}

/* A number that changes whenever the ONU changes activation state: the sum of
 * the counts on the `onu_transitions` lines of /proc/odi_gpon/status. The OLT
 * can only be replaced or reconfigured across a re-registration, so the
 * OLT-G read under one key stays valid until the key moves. 0 when there are
 * no such lines (no odi-oss status file): the caller then does not cache. */
static unsigned long long olt_state_key(const char *status)
{
	static const char w[] = "onu_transitions";
	unsigned long long sum = 0;
	unsigned long i = 0;

	while (status[i]) {
		unsigned long le = i, k = 0, d;
		unsigned long long v = 0;

		while (status[le] && status[le] != '\n')
			le++;
		while (w[k] && i + k < le && status[i + k] == w[k])
			k++;
		if (!w[k] && status[i + k] == ' ') {
			d = le;
			while (d > i && status[d - 1] >= '0' && status[d - 1] <= '9')
				d--;
			for (; d < le; d++)
				v = v * 10 + (unsigned long long)(status[d] - '0');
			sum += v;
		}
		i = status[le] ? le + 1 : le;
	}
	return sum;
}

/*
 * The OLT-G read is one more omcicli fork, so it is not done on every scrape.
 * olt_plan() decides whether to run it and olt_done() records the result; the
 * caller emits the cached info only while c->ok. All times are whole seconds of
 * /proc/uptime, the clock the exporter already uses elsewhere (the respawn
 * metrics); an unreadable one (negative) counts as 0, which holds a retry or an
 * expiry back rather than forking more.
 *
 *  - The info is kept under olt_state_key(). A new key (the ONU changed
 *    state, so the OLT may have) drops it at once and the next scrape reads
 *    again; until a read succeeds the family is absent, never the old OLT.
 *  - It also expires after OLT_MAX_AGE_S, for a counter that restarted at
 *    the same sum (odi-oss zeroes its counters in odi_gpon_init(), at driver
 *    load, so only a driver reload does that). An hour is one short fork in
 *    3600 s, and bounds how long an unnoticed change stays unseen. With no key
 *    (key 0: no status file, so no way to see a change) the age is
 *    OLT_RETRY_S instead.
 *  - A failed attempt (no answer, the 2 s bound hit, an answer that does not
 *    parse: no OLT-G yet, or the stock firmware printing something else) is
 *    not repeated for OLT_RETRY_S unless the key changed. A minute is four
 *    scrapes at 15 s, so a hung omcicli costs its bound on one scrape in
 *    four at worst, and at 5 minutes it is one try per scrape, the least
 *    that still finds the entity once the OLT has provisioned it.
 */
#define OLT_RETRY_S   60
#define OLT_MAX_AGE_S 3600

struct olt_cache {
	int ok;				/* info is valid */
	unsigned long long key;		/* the key it was read under */
	long read_at;
	int tried;			/* an attempt was made, and when, under which key */
	long tried_at;
	unsigned long long tried_key;
	struct olt_info info;
};

/* 1 when the caller should run omcicli now. May drop the cached info. */
static int olt_plan(struct olt_cache *c, unsigned long long key, long now, int omci_ok)
{
	if (now < 0)
		now = 0;
	if (c->ok && (key != c->key ||
		      now - c->read_at >= (key ? OLT_MAX_AGE_S : OLT_RETRY_S))) {
		c->ok = 0;
		c->tried = 0;
	}
	if (c->ok || !omci_ok)
		return 0;
	if (c->tried && key == c->tried_key && now >= c->tried_at &&
	    now - c->tried_at < OLT_RETRY_S)
		return 0;
	return 1;
}

/* Field by field: a struct assignment compiles to a memcpy call, and this
 * build has no libc to link one from. */
static void olt_copy(char *d, const char *s)
{
	unsigned long i = 0;

	for (; s[i]; i++)
		d[i] = s[i];
	d[i] = 0;
}

/* The result of the run olt_plan() asked for: ok and *i when the answer
 * parsed, else ok 0. */
static void olt_done(struct olt_cache *c, unsigned long long key, long now, int ok,
		     const struct olt_info *i)
{
	if (now < 0)
		now = 0;
	c->tried = 1;
	c->tried_at = now;
	c->tried_key = key;
	c->ok = ok;
	if (ok) {
		olt_copy(c->info.vendor, i->vendor);
		olt_copy(c->info.version, i->version);
		olt_copy(c->info.equip, i->equip);
		c->key = key;
		c->read_at = now;
	}
}

#endif
