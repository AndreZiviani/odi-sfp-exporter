/* Host-native test for src/cpu_stat.h: gpon_cpu_seconds_total rendered from
 * the first line of /proc/stat. `make test` runs it. */
#include <stdio.h>
#include <string.h>
#include "../src/cpu_stat.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

static char out[2048];

static const char *render(const char *in)
{
	unsigned long n = cpu_stat_render(in, out, sizeof out);

	if (n != strlen(out))
		fails++;
	return out;
}

static int has(const char *s) { return strstr(out, s) != NULL; }

static int count(const char *s)
{
	const char *p = out;
	int n = 0;

	while ((p = strstr(p, s)) != NULL) {
		n++;
		p += strlen(s);
	}
	return n;
}

int main(void)
{
	/* A whole /proc/stat of a one-CPU stick, ten columns on the cpu line. */
	const char *full =
		"cpu  4081 0 16404 584416 0 0 252 0 0 0\n"
		"cpu0 4081 0 16404 584416 0 0 252 0 0 0\n"
		"intr 1234567 0 0 0 99\n"
		"ctxt 7654321\n"
		"btime 1000000000\n"
		"processes 812\n"
		"procs_running 1\n"
		"procs_blocked 0\n";
	unsigned long n;

	render(full);
	ck(has("# HELP gpon_cpu_seconds_total ") &&
	   has("\n# TYPE gpon_cpu_seconds_total counter\n"), "HELP and TYPE lines");
	ck(count("# TYPE ") == 1 && count("# HELP ") == 1, "announced once");
	ck(has("\ngpon_cpu_seconds_total{mode=\"user\"} 40.81\n"), "user");
	ck(has("\ngpon_cpu_seconds_total{mode=\"nice\"} 0.00\n"), "nice, zero ticks");
	ck(has("\ngpon_cpu_seconds_total{mode=\"system\"} 164.04\n"), "system");
	ck(has("\ngpon_cpu_seconds_total{mode=\"idle\"} 5844.16\n"), "idle");
	ck(has("\ngpon_cpu_seconds_total{mode=\"iowait\"} 0.00\n"), "iowait");
	ck(has("\ngpon_cpu_seconds_total{mode=\"irq\"} 0.00\n"), "irq");
	ck(has("\ngpon_cpu_seconds_total{mode=\"softirq\"} 2.52\n"), "softirq");
	ck(has("\ngpon_cpu_seconds_total{mode=\"steal\"} 0.00\n"), "steal");
	ck(count("gpon_cpu_seconds_total{") == 8, "eight modes, the guest columns left out");
	ck(!has("cpu0") && !has("intr") && !has("1234567"), "nothing from the other lines");

	render("cpu  7 42 100 18446744073709551615\n");
	ck(has("{mode=\"user\"} 0.07\n"), "one digit of ticks");
	ck(has("{mode=\"nice\"} 0.42\n"), "two digits of ticks");
	ck(has("{mode=\"system\"} 1.00\n"), "three digits of ticks");
	ck(has("{mode=\"idle\"} 184467440737095516.15\n"), "the largest 64-bit count");
	ck(count("gpon_cpu_seconds_total{") == 4, "an older kernel with four columns: four series");

	render("cpu  007 0 0 0\n");
	ck(has("{mode=\"user\"} 0.07\n"), "leading zeros go");

	render("cpu 5 6\n");
	ck(has("{mode=\"user\"} 0.05\n") && has("{mode=\"nice\"} 0.06\n"), "one space after cpu");

	/* A read cut short: the last column may be half a number. */
	render("cpu  4081 0 16404 5844");
	ck(count("gpon_cpu_seconds_total{") == 3 && !has("idle"),
	   "a column cut short by the end of the read is dropped");

	render("cpu  4081 12a 16404 584416\n");
	ck(count("gpon_cpu_seconds_total{") == 1 && has("{mode=\"user\"} 40.81\n"),
	   "a column that is not digits ends the rendering there");
	render("cpu  4081 -5 16404\n");
	ck(count("gpon_cpu_seconds_total{") == 1, "a negative column ends it too");
	render("cpu  4081 184467440737095516150 3\n");
	ck(count("gpon_cpu_seconds_total{") == 1, "21 digits are not a tick count");

	ck(cpu_stat_render("", out, sizeof out) == 0 && out[0] == 0, "empty file: nothing");
	ck(cpu_stat_render("cpu\n", out, sizeof out) == 0, "no columns: nothing");
	ck(cpu_stat_render("cpu  \n", out, sizeof out) == 0, "only spaces: nothing");
	ck(cpu_stat_render("cpu  abc\n", out, sizeof out) == 0, "text: nothing");
	ck(cpu_stat_render("cpu0 4081 0 16404 584416\n", out, sizeof out) == 0,
	   "a per-CPU line first: nothing");
	ck(cpu_stat_render("intr 5 6 7\ncpu  1 2 3 4\n", out, sizeof out) == 0,
	   "the cpu line not first: nothing");
	ck(cpu_stat_render(full, out, 0) == 0, "zero capacity");

	/* Truncation stays inside the buffer and terminated. */
	n = cpu_stat_render(full, out, 20);
	ck(n == 19 && out[19] == 0 && strlen(out) == 19, "truncated at cap, terminated");

	if (fails) {
		printf("test_cpu_stat: %d failure(s)\n", fails);
		return 1;
	}
	printf("test_cpu_stat: all checks passed\n");
	return 0;
}
