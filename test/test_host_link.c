/* Host-native test for src/host_link.h: the host-port auto-negotiation and
 * safe-mode metrics rendered from the odi-oss state files. `make test`
 * runs it. */
#include <stdio.h>
#include <string.h>
#include "../src/host_link.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

static char out[4096];

static const char *render(const char *st, int now, int rec, const char *link)
{
	host_link_render(st, now, rec, link, out, sizeof out);
	return out;
}

static int has(const char *s) { return strstr(out, s) != NULL; }

int main(void)
{
	const char *dflt = "setting=on\nrunning=on\nphase=steady\nfallback=0\ntrial_mode=\n"
			   "event=boot\nmessage=auto-negotiation on\n";
	const char *fb = "setting=off\nrunning=on\nphase=steady\nfallback=1\nevent=fallback\n";

	render(dflt, 0, 0, "1\n");
	ck(has("\ngpon_host_autoneg{state=\"running\"} 1\n") && has("\ngpon_host_autoneg{state=\"setting\"} 1\n"),
	   "the default: running and setting on");
	ck(has("\ngpon_host_link_up 1\n"), "the link up");
	ck(has("\ngpon_host_autoneg_fallback 0\n") && has("\ngpon_host_autoneg_trial 0\n") &&
	   has("\ngpon_host_autoneg_watch 0\n"), "no fallback, no trial, no watch");
	ck(has("\ngpon_safe_mode 0\n") && has("\ngpon_safe_mode_unacknowledged 0\n"),
	   "no safe mode: explicit zeros, the image has the feature");
	ck(has("# TYPE gpon_host_autoneg gauge\n") && has("# TYPE gpon_safe_mode gauge\n") &&
	   has("# TYPE gpon_host_link_up gauge\n"), "every family has its TYPE line");

	render(fb, 0, 0, "1");
	ck(has("gpon_host_autoneg{state=\"running\"} 1\n") && has("gpon_host_autoneg{state=\"setting\"} 0\n"),
	   "the fallback: setting off, running on");
	ck(has("\ngpon_host_autoneg_fallback 1\n"), "gpon_host_autoneg_fallback 1");

	render("setting=off\nrunning=off\nphase=trial\nfallback=0\n", 0, 0, "0\n");
	ck(has("gpon_host_autoneg{state=\"running\"} 0\n") && has("\ngpon_host_autoneg_trial 1\n") &&
	   has("\ngpon_host_link_up 0\n"), "a trial of forced mode with no link");
	render("setting=off\nrunning=off\nphase=watch\nfallback=0\n", 0, 0, 0);
	ck(has("\ngpon_host_autoneg_watch 1\n") && !has("gpon_host_link_up"),
	   "the boot watch; no link parameter, no link series");

	render(dflt, 1, 1, "1\n");
	ck(has("\ngpon_safe_mode 1\n") && has("\ngpon_safe_mode_unacknowledged 1\n"),
	   "a safe-mode boot and its record");
	render(dflt, 0, 1, "1\n");
	ck(has("\ngpon_safe_mode 0\n") && has("\ngpon_safe_mode_unacknowledged 1\n"),
	   "a later boot, the record not yet acknowledged");

	render("", 1, 1, "1\n");
	ck(out[0] == 0, "no state file (older image, stock firmware): nothing at all");
	render(0, 1, 1, "1\n");
	ck(out[0] == 0, "a NULL state: nothing");

	render("setting=sideways\nrunning=\nphase=x\nfallback=2\n", 0, 0, "2\n");
	ck(!has("gpon_host_autoneg{") && !has("gpon_host_autoneg_fallback") && !has("gpon_host_link_up"),
	   "values the file does not define emit nothing");
	ck(has("\ngpon_host_autoneg_trial 0\n") && has("\ngpon_host_autoneg_watch 0\n"),
	   "an unknown phase is neither a trial nor the watch");

	render("xsetting=off\nrunning_old=off\nrunning=on", 0, 0, 0);
	ck(has("gpon_host_autoneg{state=\"running\"} 1\n") && !has("state=\"setting\""),
	   "keys match whole, at the start of a line, and a last line with no newline reads");

	host_link_render(dflt, 0, 0, "1\n", out, 64);
	ck(strlen(out) < 64, "the output is truncated at the buffer, NUL-terminated");

	printf("test_host_link: %s\n", fails ? "FAILED" : "all ok");
	return fails != 0;
}
