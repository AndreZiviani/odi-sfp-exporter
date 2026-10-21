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
	unsigned long i = 0, d = 0, n = 0, v = 0, k;
	int neg = 0;
	char num[16];

	if (cap == 0)
		return 0;
	out[0] = 0;
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
	{
		unsigned long whole = v / 1000, frac = v % 1000, w = 0;
		char rev[8];

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
		num[w++] = '\n';
		num[w] = 0;
	}

	for (k = 0; HEAD[k] && n + 1 < cap; k++)
		out[n++] = HEAD[k];
	for (k = 0; num[k] && n + 1 < cap; k++)
		out[n++] = num[k];
	out[n] = 0;
	return n;
}

#endif /* ODI_SOC_TEMP_H */
