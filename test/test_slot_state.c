/* Host-native test for src/slot_state.h: the boot-slot metrics rendered from
 * odi-oss /var/run/odi-slot contents. `make test` runs it. */
#include <stdio.h>
#include <string.h>
#include "../src/slot_state.h"

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
	slot_state_render(in, out, sizeof out);
	return out;
}

static int has(const char *s) { return strstr(out, s) != NULL; }

int main(void)
{
	const char *trial =
		"running=1\nsw_active=1\nsw_tryactive=2\nprimary_copy=2\n"
		"primary_sw_commit=0\nfallback_copy=1\nfallback_sw_commit=0\n"
		"next_boot=0\nuncommitted=1\n";

	render(trial);
	ck(has("\ngpon_boot_slot{slot=\"1\"} 1\n"), "a trial: boot slot 1");
	ck(has("\ngpon_committed_slot{copy=\"primary\",slot=\"0\"} 1\n"), "primary copy commits 0");
	ck(has("\ngpon_committed_slot{copy=\"fallback\",slot=\"0\"} 1\n"), "fallback copy commits 0");
	ck(has("\ngpon_uncommitted 1\n"), "gpon_uncommitted 1");
	ck(has("# TYPE gpon_boot_slot gauge\n") && has("# TYPE gpon_committed_slot gauge\n") &&
	   has("# TYPE gpon_uncommitted gauge\n"), "every family has its TYPE line");

	render("running=0\nprimary_sw_commit=0\nfallback_sw_commit=0\nuncommitted=0\n");
	ck(has("gpon_boot_slot{slot=\"0\"} 1\n") && has("\ngpon_uncommitted 0\n"),
	   "committed slot 0: gpon_uncommitted 0");

	/* The keys are matched whole, at the start of a line. */
	render("xrunning=1\nrunning_old=0\nuncommitted=1\n");
	ck(!has("gpon_boot_slot"), "a key only matches at the start of a line, whole");

	/* Unknown values emit nothing, not a guessed 0. */
	render("running=\nsw_active=\nprimary_sw_commit=\nfallback_sw_commit=\nuncommitted=\n");
	ck(out[0] == 0, "every value empty (state unknown): nothing at all, no headers");
	render("running=1\nprimary_sw_commit=0\nfallback_copy=\nfallback_sw_commit=\nuncommitted=0\n");
	ck(has("copy=\"primary\"") && !has("copy=\"fallback\""),
	   "no valid fallback copy: only the primary series");
	render("running=2\nprimary_sw_commit=10\nuncommitted=yes\n");
	ck(out[0] == 0, "values that are not exactly 0 or 1 are dropped");
	render("running=1");
	ck(has("gpon_boot_slot{slot=\"1\"} 1\n"), "a last line with no newline still reads");
	render("");
	ck(out[0] == 0, "an empty file: nothing");

	/* Truncation never overruns and stays terminated. */
	{
		char small[40];
		unsigned long n;

		memset(small, 'x', sizeof small);
		n = slot_state_render(trial, small, sizeof small);
		ck(n == sizeof small - 1 && small[sizeof small - 1] == 0,
		   "output is truncated at cap and NUL-terminated");
	}

	printf("%s\n", fails ? "FAILED" : "test_slot_state: all ok");
	return fails ? 1 : 0;
}
