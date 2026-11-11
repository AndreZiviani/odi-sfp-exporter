/*
 * CPU time, from the first line of /proc/stat:
 *
 *     cpu  4081 0 16404 584416 0 0 252 0 0 0
 *
 * the ticks all CPUs together spent in user, nice, system, idle, iowait,
 * irq, softirq and steal since boot (proc(5)), rendered as
 *
 *     gpon_cpu_seconds_total{mode="user"} 40.81
 *     gpon_cpu_seconds_total{mode="system"} 164.04
 *     gpon_cpu_seconds_total{mode="idle"} 5844.16
 *     ...
 *
 * The load average says nothing on this stick (docs/METRICS.md: pinned at
 * 2.00), so this is the measure of how busy the CPU is: the rate of a mode
 * is the share of the time spent in it.
 *
 * A tick is 1/USER_HZ of a second, and USER_HZ is 100: the kernel converts
 * its own HZ to it before printing, and for MIPS it is a constant of the
 * kernel source in every version, not a build option. A hundredth needs no
 * division, only a decimal point two digits from the end, so the value is
 * the kernel's own digits and never passes through a number.
 *
 * Only as many modes as the line has: an older kernel prints fewer columns
 * and gets fewer series. The guest columns after steal are not exported,
 * the kernel counts that time in user and nice already. A first line that is
 * not the `cpu` total, or a column that is not plain digits, ends the
 * rendering there; with no column at all nothing is emitted.
 *
 * Pure: no syscalls, so it is tested on the host (test/test_cpu_stat.c).
 */
#ifndef ODI_CPU_STAT_H
#define ODI_CPU_STAT_H

#define CPU_STAT_DIGITS 20	/* a 64-bit tick count */

static const char *const cpu_stat_modes[] = {
	"user", "nice", "system", "idle", "iowait", "irq", "softirq", "steal",
};

#define CPU_STAT_NMODES (sizeof(cpu_stat_modes) / sizeof(cpu_stat_modes[0]))

/* Append s (n bytes) to out at *len, truncating at cap (one byte kept for
 * the NUL). */
static void cpu_stat_putn(char *out, unsigned long cap, unsigned long *len,
			  const char *s, unsigned long n)
{
	unsigned long k;

	for (k = 0; k < n && *len + 1 < cap; k++)
		out[(*len)++] = s[k];
	out[*len] = 0;
}

static void cpu_stat_put(char *out, unsigned long cap, unsigned long *len, const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	cpu_stat_putn(out, cap, len, s, n);
}

/* Ticks (n digits) as seconds: the point two digits from the end. Leading
 * zeros go, so that 007 reads 0.07. */
static void cpu_stat_put_seconds(char *out, unsigned long cap, unsigned long *len,
				 const char *v, unsigned long n)
{
	while (n > 1 && v[0] == '0') {
		v++;
		n--;
	}
	if (n > 2) {
		cpu_stat_putn(out, cap, len, v, n - 2);
		cpu_stat_put(out, cap, len, ".");
		cpu_stat_putn(out, cap, len, v + n - 2, 2);
		return;
	}
	cpu_stat_put(out, cap, len, n == 2 ? "0." : "0.0");
	cpu_stat_putn(out, cap, len, v, n);
}

/* Render gpon_cpu_seconds_total from the contents of /proc/stat (only its
 * first line is looked at; a read cut short after it is fine) into out
 * (NUL-terminated, truncated at cap). Returns the length, 0 when the line is
 * not a `cpu` total with at least one column. */
static unsigned long cpu_stat_render(const char *buf, char *out, unsigned long cap)
{
	static const char HEAD[] =
		"# HELP gpon_cpu_seconds_total CPU time since boot by mode, from the "
		"first line of /proc/stat. The rate of a mode is the share of the "
		"time spent in it.\n"
		"# TYPE gpon_cpu_seconds_total counter\n";
	unsigned long len = 0, i = 3, m;

	if (cap == 0)
		return 0;
	out[0] = 0;
	if (buf[0] != 'c' || buf[1] != 'p' || buf[2] != 'u' || buf[3] != ' ')
		return 0;
	for (m = 0; m < CPU_STAT_NMODES; m++) {
		unsigned long s, n;

		while (buf[i] == ' ')
			i++;
		s = i;
		while (buf[i] >= '0' && buf[i] <= '9')
			i++;
		n = i - s;
		/* a column ends at a space or at the end of the line; one cut
		 * short by the end of the read is not a value */
		if (n == 0 || n > CPU_STAT_DIGITS || (buf[i] != ' ' && buf[i] != '\n'))
			break;
		if (m == 0)
			cpu_stat_put(out, cap, &len, HEAD);
		cpu_stat_put(out, cap, &len, "gpon_cpu_seconds_total{mode=\"");
		cpu_stat_put(out, cap, &len, cpu_stat_modes[m]);
		cpu_stat_put(out, cap, &len, "\"} ");
		cpu_stat_put_seconds(out, cap, &len, buf + s, n);
		cpu_stat_put(out, cap, &len, "\n");
	}
	return len;
}

#endif /* ODI_CPU_STAT_H */
