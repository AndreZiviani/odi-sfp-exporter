/*
 * The SoC die temperature, from the hwmon sensor the odi-oss kernel registers.
 *
 * odi-oss exposes the die temperature as a standard hwmon device whose name
 * file reads `rtl9602c_soc` and whose temp1_input is in millidegrees C. The
 * hwmonN index is assigned at probe time, so the exporter looks at
 * SOC_TEMP_HWMON_COUNT of them by name rather than assume a number. Nothing
 * here lists a directory: each candidate is one bounded read of a sysfs file.
 *
 * Rendered as
 *
 *     gpon_soc_temperature_celsius 62.000
 *
 * The value is the kernel's, to the millidegree, and it is not calibrated:
 * the scale and offset are inferred, so it is a die temperature good to a few
 * degrees. A trend, not a measurement (docs/METRICS.md).
 *
 * The kernel also exports the limits and alarms in the same directory:
 * temp1_max and temp1_crit (millidegrees, odi-oss defaults on the same
 * unverified scale) and temp1_max_alarm and temp1_crit_alarm (0 or 1),
 * rendered as gpon_soc_temperature_{max,crit}_celsius and
 * gpon_soc_temperature_alarm{level="max"|"crit"}. Each appears only when its
 * file exists and parses; an older image without them emits none.
 *
 * No sensor (the stock firmware, an older odi-oss image, an unreadable value,
 * anything that is not a plain integer) emits nothing: an absent series is
 * honest, a guessed 0 is not.
 *
 * Pure: no syscalls, so it is tested on the host (test/test_soc_temp.c).
 */
#ifndef ODI_SOC_TEMP_H
#define ODI_SOC_TEMP_H

#define SOC_TEMP_HWMON_COUNT 8
#define SOC_TEMP_NAME "rtl9602c_soc"

/* 1 when the contents of a hwmon `name` file are exactly the sensor name,
 * with or without the trailing newline. */
static int soc_temp_name_match(const char *buf)
{
	const char *n = SOC_TEMP_NAME;
	unsigned long i = 0;

	while (n[i] && buf[i] == n[i])
		i++;
	if (n[i])
		return 0;
	return buf[i] == 0 || (buf[i] == '\n' && buf[i + 1] == 0);
}

/* Parse the contents of a millidegree file (an optional minus sign and at
 * most seven digits, then an optional newline) into degrees text such as
 * "-12.345" in num (at least 16 bytes). Returns 1, or 0 for anything else. */
static int soc_temp_celsius(const char *buf, char *num)
{
	unsigned long i = 0, d = 0, v = 0, k, w = 0, whole, frac;
	char rev[8];
	int neg = 0;

	if (buf[i] == '-') {
		neg = 1;
		i++;
	}
	while (buf[i] >= '0' && buf[i] <= '9') {
		if (d == 7)
			return 0;
		v = v * 10 + (unsigned long)(buf[i] - '0');
		d++;
		i++;
	}
	if (d == 0 || !(buf[i] == 0 || (buf[i] == '\n' && buf[i + 1] == 0)))
		return 0;

	/* whole degrees, a point, three decimals */
	whole = v / 1000;
	frac = v % 1000;
	if (neg && v)
		num[w++] = '-';
	k = 0;
	do {
		rev[k++] = (char)('0' + whole % 10);
		whole /= 10;
	} while (whole);
	while (k)
		num[w++] = rev[--k];
	num[w++] = '.';
	num[w++] = (char)('0' + frac / 100);
	num[w++] = (char)('0' + frac / 10 % 10);
	num[w++] = (char)('0' + frac % 10);
	num[w] = 0;
	return 1;
}

/* Append s to out at *n, truncating at cap (one byte kept for the NUL). */
static void soc_temp_put(char *out, unsigned long cap, unsigned long *n, const char *s)
{
	unsigned long k;

	for (k = 0; s[k] && *n + 1 < cap; k++)
		out[(*n)++] = s[k];
	out[*n] = 0;
}

/* Render the metric for the contents of temp1_input (millidegrees C, an
 * optional minus sign and at most seven digits, then a newline) into out
 * (NUL-terminated, truncated at cap). Returns the length, 0 for a value that
 * is not one. */
static unsigned long soc_temp_render(const char *buf, char *out, unsigned long cap)
{
	static const char HEAD[] =
		"# HELP gpon_soc_temperature_celsius SoC die temperature, from the "
		"odi-oss hwmon sensor rtl9602c_soc. The scale and offset are "
		"inferred, not calibrated: good to a few degrees, a trend more "
		"than a measurement.\n"
		"# TYPE gpon_soc_temperature_celsius gauge\n"
		"gpon_soc_temperature_celsius ";
	unsigned long n = 0;
	char num[16];

	if (cap == 0)
		return 0;
	out[0] = 0;
	if (!soc_temp_celsius(buf, num))
		return 0;
	soc_temp_put(out, cap, &n, HEAD);
	soc_temp_put(out, cap, &n, num);
	soc_temp_put(out, cap, &n, "\n");
	return n;
}

/* Render one limit gauge from temp1_max (crit = 0) or temp1_crit (crit = 1),
 * same file format as temp1_input. 0 and nothing for a value that is not one
 * or a file that is absent (buf NULL). */
static unsigned long soc_temp_render_limit(const char *buf, int crit, char *out,
					   unsigned long cap)
{
	static const char MAX_HEAD[] =
		"# HELP gpon_soc_temperature_max_celsius SoC die temperature warning "
		"limit (hwmon temp1_max). An odi-oss default on an unverified "
		"scale, not a vendor figure.\n"
		"# TYPE gpon_soc_temperature_max_celsius gauge\n"
		"gpon_soc_temperature_max_celsius ";
	static const char CRIT_HEAD[] =
		"# HELP gpon_soc_temperature_crit_celsius SoC die temperature "
		"critical limit (hwmon temp1_crit). An odi-oss default on an "
		"unverified scale, not a vendor figure.\n"
		"# TYPE gpon_soc_temperature_crit_celsius gauge\n"
		"gpon_soc_temperature_crit_celsius ";
	unsigned long n = 0;
	char num[16];

	if (cap == 0)
		return 0;
	out[0] = 0;
	if (!buf || !soc_temp_celsius(buf, num))
		return 0;
	soc_temp_put(out, cap, &n, crit ? CRIT_HEAD : MAX_HEAD);
	soc_temp_put(out, cap, &n, num);
	soc_temp_put(out, cap, &n, "\n");
	return n;
}

/* An alarm file: exactly "0" or "1", with or without the newline. Returns the
 * digit, or -1 for anything else (including NULL). */
static int soc_temp_alarm_value(const char *buf)
{
	if (!buf || (buf[0] != '0' && buf[0] != '1'))
		return -1;
	if (!(buf[1] == 0 || (buf[1] == '\n' && buf[2] == 0)))
		return -1;
	return buf[0] - '0';
}

/* Render gpon_soc_temperature_alarm{level="max"|"crit"} from the contents of
 * temp1_max_alarm and temp1_crit_alarm (either may be NULL for an absent or
 * unreadable file). One HELP/TYPE pair, and a sample only for each file that
 * parses; nothing at all when neither does. Returns the length. */
static unsigned long soc_temp_render_alarms(const char *max_buf, const char *crit_buf,
					    char *out, unsigned long cap)
{
	static const char HEAD[] =
		"# HELP gpon_soc_temperature_alarm 1 while the SoC die temperature is "
		"above the limit of that level (hwmon temp1_max_alarm / "
		"temp1_crit_alarm). The limits are odi-oss defaults on an "
		"unverified scale.\n"
		"# TYPE gpon_soc_temperature_alarm gauge\n";
	int m = soc_temp_alarm_value(max_buf), c = soc_temp_alarm_value(crit_buf);
	unsigned long n = 0;
	char one[2] = { 0, 0 };

	if (cap == 0)
		return 0;
	out[0] = 0;
	if (m < 0 && c < 0)
		return 0;
	soc_temp_put(out, cap, &n, HEAD);
	if (m >= 0) {
		one[0] = (char)('0' + m);
		soc_temp_put(out, cap, &n, "gpon_soc_temperature_alarm{level=\"max\"} ");
		soc_temp_put(out, cap, &n, one);
		soc_temp_put(out, cap, &n, "\n");
	}
	if (c >= 0) {
		one[0] = (char)('0' + c);
		soc_temp_put(out, cap, &n, "gpon_soc_temperature_alarm{level=\"crit\"} ");
		soc_temp_put(out, cap, &n, one);
		soc_temp_put(out, cap, &n, "\n");
	}
	return n;
}

#endif /* ODI_SOC_TEMP_H */
