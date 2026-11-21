/*
 * gpon_memory_bytes from /proc/meminfo.
 *
 * On a device with about 64 MB this is the metric most likely to explain a
 * reboot, and it costs no fork. The kernel prints kB; the samples are bytes,
 * Prometheus base units, in integer arithmetic:
 *
 *     gpon_memory_bytes{kind="total"} 28188672
 *     gpon_memory_bytes{kind="free"} 1417216
 *     gpon_memory_bytes{kind="available"} 13631488
 *     gpon_memory_bytes{kind="buffers"} 2084864
 *     gpon_memory_bytes{kind="cached"} 9588736
 *
 * "available" is MemAvailable, the value the odi-oss watchdog resets the
 * stick on. A kernel older than 3.14 (the stock firmware) prints no such
 * line, and then there is no "available" sample rather than a guessed one.
 * A key whose line is missing, or has no digits, is left out the same way.
 *
 * Pure: no syscalls, so it is tested on the host (test/test_meminfo.c).
 */
#ifndef ODI_MEMINFO_H
#define ODI_MEMINFO_H

static void meminfo_put(char *out, unsigned long cap, unsigned long *n, const char *s)
{
	while (*s && *n + 1 < cap)
		out[(*n)++] = *s++;
	out[*n] = 0;
}

/* The kB value of the line that starts with key ("MemFree:"), into *kb.
 * 1 when the line is there and has digits. */
static int meminfo_kb(const char *buf, const char *key, unsigned long *kb)
{
	unsigned long i, k, v;

	for (i = 0; buf[i]; i++) {
		if (i && buf[i - 1] != '\n')
			continue;
		for (k = 0; key[k] && buf[i + k] == key[k]; k++)
			;
		if (key[k])
			continue;
		v = 0;
		i += k;
		while (buf[i] == ' ')
			i++;
		if (buf[i] < '0' || buf[i] > '9')
			return 0;
		while (buf[i] >= '0' && buf[i] <= '9')
			v = v * 10 + (unsigned long)(buf[i++] - '0');
		*kb = v;
		return 1;
	}
	return 0;
}

/* The header and one sample per key found; 0 and nothing when none is. */
static unsigned long meminfo_render(const char *buf, char *out, unsigned long cap)
{
	static const char *const keys[] = {
		"MemTotal:", "MemFree:", "MemAvailable:", "Buffers:", "Cached:", 0
	};
	static const char *const names[] = {
		"total", "free", "available", "buffers", "cached", 0
	};
	unsigned long n = 0, kb;
	int k;

	if (cap == 0)
		return 0;
	out[0] = 0;
	for (k = 0; keys[k]; k++) {
		char num[12];
		unsigned long b, d = 0;
		int j;

		if (!meminfo_kb(buf, keys[k], &kb))
			continue;
		if (n == 0)
			meminfo_put(out, cap, &n,
				    "# HELP gpon_memory_bytes Kernel memory, from /proc/meminfo.\n"
				    "# TYPE gpon_memory_bytes gauge\n");
		/* 32-bit unsigned long on the stick, as before: tens of MB fit. */
		b = kb * 1024;
		do {
			num[d++] = (char)('0' + b % 10);
			b /= 10;
		} while (b && d < sizeof(num) - 1);
		meminfo_put(out, cap, &n, "gpon_memory_bytes{kind=\"");
		meminfo_put(out, cap, &n, names[k]);
		meminfo_put(out, cap, &n, "\"} ");
		for (j = (int)d - 1; j >= 0; j--) {
			char c[2] = { num[j], 0 };

			meminfo_put(out, cap, &n, c);
		}
		meminfo_put(out, cap, &n, "\n");
	}
	return n;
}

#endif
