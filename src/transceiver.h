/*
 * The module's own alarm and warning flags, as metrics: the SFF-8472 A2h
 * bytes 110-117 that odi-oss `diag pon get transceiver alarm-status` prints,
 * one line each:
 *
 *     Alarm Temperature high, status: clear
 *     Alarm Rx Power low, status: occur
 *     Warning Bias Current high, status: clear
 *     Optical LOS, status: clear
 *
 * The module raises these against its OWN limits (A2h bytes 0-39, which differ
 * from one module to the next), so they are the right signal for "the optics
 * are out of range", where the fixed thresholds in prometheus/alerts.yml are
 * class B+ defaults. A line the module is receiving at -32 dBm, working, sits
 * below those defaults and may be well above its own flag.
 *
 * Three outcomes, so a missing series means something:
 *
 *  - lines recognised: gpon_transceiver_up 1, the flags, the optical LOS bit;
 *  - the read failed (diag prints "read failed"): gpon_transceiver_up 0 and
 *    nothing else. The DDM readings vanish in that case too (the same I2C
 *    bus), and this is the only series that says why;
 *  - nothing recognised (the stock diag has no such command, an older odi-oss
 *    diag neither): nothing at all. An image that cannot say is not "down".
 *
 * The condition label is the name diag prints, lowercased with underscores:
 * `rx_power_low`, `temperature_high`. Only [0-9A-Za-z_] survives; a line whose
 * name has anything else is skipped, as is a name over 31 characters. Pure
 * text in, text out, through the provision.h writer: no syscalls, so
 * test/test_transceiver.c runs it natively.
 */
#ifndef ODI_TRANSCEIVER_H
#define ODI_TRANSCEIVER_H

#include "provision.h"

#define XCVR_COND_MAX 32

/* 1 and the status word when the line starts with `key`, then the name up to
 * a comma and a "status:" word; fills name (normalised) and *occur. */
static int xcvr_parse_line(const char *l, unsigned long n, const char *key,
			   char *name, int *occur)
{
	unsigned long i = 0, k = 0, e, st, w;

	while (key[k] && i < n && l[i] == key[k]) {
		i++;
		k++;
	}
	if (key[k])
		return 0;
	e = i;
	while (e < n && l[e] != ',')
		e++;
	if (e >= n || e == i)
		return 0;
	for (k = 0; i + k < e; k++) {
		char c = l[i + k];

		if (k + 1 >= XCVR_COND_MAX)
			return 0;
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		else if (c == ' ')
			c = '_';
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_'))
			return 0;
		name[k] = c;
	}
	name[k] = 0;
	st = e + 1;
	while (st < n && l[st] == ' ')
		st++;
	if (st + 7 > n || l[st] != 's' || l[st + 1] != 't' || l[st + 2] != 'a' ||
	    l[st + 3] != 't' || l[st + 4] != 'u' || l[st + 5] != 's' || l[st + 6] != ':')
		return 0;
	st += 7;
	while (st < n && l[st] == ' ')
		st++;
	w = st;
	while (w < n && l[w] != ' ' && l[w] != '\r')
		w++;
	/* Anything but "clear" is asserted, as gpon_alarm does it. */
	*occur = !(w - st == 5 && l[st] == 'c' && l[st + 1] == 'l' && l[st + 2] == 'e' &&
		   l[st + 3] == 'a' && l[st + 4] == 'r');
	return 1;
}

static int xcvr_has(const char *sec, const char *word)
{
	unsigned long i, k;

	for (i = 0; sec[i]; i++) {
		for (k = 0; word[k] && sec[i + k] == word[k]; k++)
			;
		if (!word[k])
			return 1;
	}
	return 0;
}

/* One family: every line starting `key`, labelled kind="<kind>". */
static int xcvr_emit_kind(const struct prov_out *o, const char *sec, const char *key,
			  const char *kind, int *header)
{
	unsigned long i = 0;
	int any = 0;

	while (sec[i]) {
		unsigned long n = prov_line_end(sec + i);
		char name[XCVR_COND_MAX];
		int occur;

		if (xcvr_parse_line(sec + i, n, key, name, &occur)) {
			if (!*header) {
				prov_header(o, "gpon_transceiver_alarm",
					    "1 when the transceiver module itself reports that "
					    "condition, against the limits stored in the module "
					    "(SFF-8472 A2h 112-117): kind alarm is the outer "
					    "limit, warning the inner one.");
				*header = 1;
			}
			prov_puts(o, "gpon_transceiver_alarm{kind=\"");
			prov_puts(o, kind);
			prov_puts(o, "\",condition=\"");
			prov_puts(o, name);
			prov_puts(o, occur ? "\"} 1\n" : "\"} 0\n");
			any = 1;
		}
		i += n;
		if (sec[i])
			i++;
	}
	return any;
}

static void xcvr_emit(const struct prov_out *o, const char *sec)
{
	int header = 0, up, los_seen = 0;
	unsigned long i = 0;

	if (xcvr_has(sec, "read failed")) {
		prov_header(o, "gpon_transceiver_up",
			    "1 when the transceiver module answered on I2C in this scrape, "
			    "0 when the read failed (the DDM readings are then absent).");
		prov_puts(o, "gpon_transceiver_up 0\n");
		return;
	}
	/* Both calls run, so the alarm lines and the warning lines are each
	 * contiguous. */
	up = xcvr_emit_kind(o, sec, "Alarm ", "alarm", &header);
	up |= xcvr_emit_kind(o, sec, "Warning ", "warning", &header);

	while (sec[i]) {
		unsigned long n = prov_line_end(sec + i);
		char name[XCVR_COND_MAX];
		int occur;

		/* "Optical LOS, status: clear": no word after the key, so the key
		 * stops before the comma. */
		if (xcvr_parse_line(sec + i, n, "Optical ", name, &occur) &&
		    name[0] == 'l' && name[1] == 'o' && name[2] == 's' && !name[3]) {
			if (!los_seen) {
				prov_header(o, "gpon_transceiver_rx_los",
					    "1 when the module reports loss of optical signal "
					    "(SFF-8472 A2h byte 110 bit 1). Not the GPON LOS "
					    "alarm of the framer, which is gpon_alarm.");
				los_seen = 1;
			}
			prov_puts(o, occur ? "gpon_transceiver_rx_los 1\n"
					   : "gpon_transceiver_rx_los 0\n");
			up = 1;
		}
		i += n;
		if (sec[i])
			i++;
	}
	if (!up)
		return;
	prov_header(o, "gpon_transceiver_up",
		    "1 when the transceiver module answered on I2C in this scrape, "
		    "0 when the read failed (the DDM readings are then absent).");
	prov_puts(o, "gpon_transceiver_up 1\n");
}

/*
 * The module's limits and calibration, from two more commands of the odi-oss
 * diag (`pon get transceiver limits`, `monitoring-type`), read from the module
 * itself and fixed for its life, so a few lines each:
 *
 *     Rx Power alarm low: -30.000000  dBm
 *     Temperature warning high: 90.000000 C
 *
 *     Monitoring type: 0x68
 *     Digital diagnostics: implemented
 *     Calibration: internal
 *     Rx power type: average
 *
 * gpon_transceiver_limit{measure,level,side} carries the limit in the unit of
 * the matching reading (Celsius, volts, mA, dBm): the line to draw next to
 * gpon_rx_power_dbm and the others. A limit that is not a plain decimal (the
 * module stores 0 for a limit it does not use, which reads "-inf" in dBm) is
 * skipped. gpon_transceiver_monitoring_info{diagnostics,calibration,
 * rx_power_type} is 1, from a fixed vocabulary of words (anything else drops
 * the series): the conversions of the diag assume internal calibration, and
 * this is where that stops being an assumption. Nothing when the diag does
 * not know the commands or the read failed.
 */
static int xcvr_word_in(const char *w, unsigned long n, const char *const *list)
{
	unsigned long i, k;

	for (i = 0; list[i]; i++) {
		for (k = 0; k < n && list[i][k] == w[k]; k++)
			;
		if (k == n && !list[i][k])
			return 1;
	}
	return 0;
}

/* The text after "key" in the line, trimmed of a trailing CR/space; 0 when
 * the line does not start with key. */
static int xcvr_after(const char *l, unsigned long n, const char *key,
		      const char **v, unsigned long *vn)
{
	unsigned long k = 0;

	while (key[k] && k < n && l[k] == key[k])
		k++;
	if (key[k])
		return 0;
	*v = l + k;
	*vn = n - k;
	while (*vn && ((*v)[*vn - 1] == '\r' || (*v)[*vn - 1] == ' '))
		(*vn)--;
	return *vn > 0;
}

static void xcvr_emit_monitor(const struct prov_out *o, const char *sec)
{
	static const char *const diag[] = { "implemented", "not implemented", 0 };
	static const char *const cal[] = { "internal", "external", "internal and external", "none", 0 };
	static const char *const rxt[] = { "average", "OMA", 0 };
	const char *dv = 0, *cv = 0, *rv = 0;
	unsigned long dn = 0, cn = 0, rn = 0, i = 0, k;

	while (sec[i]) {
		unsigned long n = prov_line_end(sec + i);

		xcvr_after(sec + i, n, "Digital diagnostics: ", &dv, &dn);
		xcvr_after(sec + i, n, "Calibration: ", &cv, &cn);
		xcvr_after(sec + i, n, "Rx power type: ", &rv, &rn);
		i += n;
		if (sec[i])
			i++;
	}
	if (!dv || !cv || !rv || !xcvr_word_in(dv, dn, diag) ||
	    !xcvr_word_in(cv, cn, cal) || !xcvr_word_in(rv, rn, rxt))
		return;
	prov_header(o, "gpon_transceiver_monitoring_info",
		    "Always 1. How the module says its diagnostics work (SFF-8472 A0h "
		    "byte 92). The conversions of the readings assume calibration "
		    "internal; with another value they may be wrong.");
	prov_puts(o, "gpon_transceiver_monitoring_info{diagnostics=\"");
	for (k = 0; k < dn; k++) {
		char c = dv[k] == ' ' ? '_' : dv[k];

		prov_putn(o, &c, 1);
	}
	prov_puts(o, "\",calibration=\"");
	for (k = 0; k < cn; k++) {
		char c = cv[k] == ' ' ? '_' : cv[k];

		prov_putn(o, &c, 1);
	}
	prov_puts(o, "\",rx_power_type=\"");
	for (k = 0; k < rn; k++) {
		char c = rv[k] >= 'A' && rv[k] <= 'Z' ? (char)(rv[k] - 'A' + 'a') : rv[k];

		prov_putn(o, &c, 1);
	}
	prov_puts(o, "\"} 1\n");
}

/* A value "[-]d.d" followed by a space and a unit or the end: its length, 0
 * when it is not a plain decimal. */
static unsigned long xcvr_decimal(const char *v, unsigned long n)
{
	unsigned long i = 0, d = 0, f = 0;

	if (i < n && v[i] == '-')
		i++;
	while (i < n && v[i] >= '0' && v[i] <= '9') {
		i++;
		d++;
	}
	if (!d || i >= n || v[i] != '.')
		return 0;
	i++;
	while (i < n && v[i] >= '0' && v[i] <= '9') {
		i++;
		f++;
	}
	if (!f || (i < n && v[i] != ' '))
		return 0;
	return i;
}

static void xcvr_emit_limits(const struct prov_out *o, const char *sec)
{
	static const char *const measure[] = {
		"Temperature ", "Voltage ", "Bias Current ", "Tx Power ", "Rx Power ", 0 };
	static const char *const mname[] = {
		"temperature", "voltage", "bias_current", "tx_power", "rx_power" };
	static const char *const lvl[] = {
		"alarm high: ", "alarm low: ", "warning high: ", "warning low: ", 0 };
	static const char *const lname[] = {
		"alarm\",side=\"high", "alarm\",side=\"low", "warning\",side=\"high",
		"warning\",side=\"low" };
	int header = 0, m, l;

	for (m = 0; measure[m]; m++) {
		for (l = 0; lvl[l]; l++) {
			unsigned long i = 0;
			char key[32];
			unsigned long a = 0, b = 0;

			while (measure[m][a])
				key[a] = measure[m][a], a++;
			while (lvl[l][b])
				key[a + b] = lvl[l][b], b++;
			key[a + b] = 0;
			while (sec[i]) {
				unsigned long n = prov_line_end(sec + i);
				const char *v;
				unsigned long vn, dl;

				if (xcvr_after(sec + i, n, key, &v, &vn) &&
				    (dl = xcvr_decimal(v, vn)) != 0) {
					if (!header) {
						prov_header(o, "gpon_transceiver_limit",
							    "The module own alarm and warning limit for "
							    "a reading, in that reading unit (C, V, mA, "
							    "dBm), from SFF-8472 A2h 0-39. The module "
							    "raises gpon_transceiver_alarm against these.");
						header = 1;
					}
					prov_puts(o, "gpon_transceiver_limit{measure=\"");
					prov_puts(o, mname[m]);
					prov_puts(o, "\",level=\"");
					prov_puts(o, lname[l]);
					prov_puts(o, "\"} ");
					prov_putn(o, v, dl);
					prov_puts(o, "\n");
				}
				i += n;
				if (sec[i])
					i++;
			}
		}
	}
}

#endif /* ODI_TRANSCEIVER_H */
