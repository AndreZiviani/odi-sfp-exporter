/* Host-native test for src/respawn_state.h: the restart state of the
 * respawned daemons, rendered from the files odi-oss writes to
 * /var/run/respawn. The fixtures in test/fixtures/respawn are in the format
 * respawn.sh writes, plus the cases it never writes on purpose: a malformed
 * file, an unsafe name, a dot-prefixed temporary file. `make test` runs it.
 * With a path as argument it prints the rendering instead (to regenerate the
 * golden after a deliberate change). */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include "../src/respawn_state.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

static char out[16384];

static int has(const char *s) { return strstr(out, s) != NULL; }

/* The scrape as metric_respawn does it: every entry of the directory, in
 * whatever order readdir gives, through the same name check and parse. */
static unsigned long scrape(const char *dir, long uptime)
{
	struct respawn_svc sv[RESPAWN_SVC_MAX], one;
	unsigned long n = 0;
	struct dirent *de;
	DIR *d = opendir(dir);

	out[0] = 0;
	if (!d)
		return 0;
	while ((de = readdir(d))) {
		char path[512], buf[512];
		FILE *f;
		size_t got;

		if (!respawn_name_ok(de->d_name))
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
		f = fopen(path, "r");
		if (!f)
			continue;
		got = fread(buf, 1, sizeof buf - 1, f);
		fclose(f);
		buf[got] = 0;
		if (got && respawn_parse(de->d_name, buf, &one))
			respawn_add(sv, &n, &one);
	}
	closedir(d);
	return respawn_render(sv, n, uptime, out, sizeof out);
}

static int golden(const char *path)
{
	char want[16384];
	size_t got;
	FILE *f = fopen(path, "r");

	if (!f)
		return 0;
	got = fread(want, 1, sizeof want - 1, f);
	fclose(f);
	want[got] = 0;
	return strcmp(want, out) == 0;
}

int main(int argc, char **argv)
{
	struct respawn_svc s;
	char small[64];

	(void)argv;
	scrape("test/fixtures/respawn", 1000);
	if (argc > 1) {
		fputs(out, stdout);
		return 0;
	}
	ck(golden("test/fixtures/respawn.prom"), "the fixture directory renders as its golden at uptime 1000");

	/* The counter: starts minus the first, whatever the state says. */
	ck(has("\ngpon_service_restarts_total{service=\"omcid\"} 0\n"), "a daemon on its first start: 0 restarts");
	ck(has("\ngpon_service_restarts_total{service=\"confd\"} 2\n"), "three starts: 2 restarts");
	ck(has("\ngpon_service_restarts_total{service=\"syslogd\"} 6\n"), "seven starts: 6 restarts");
	ck(has("\ngpon_service_restarts_total{service=\"ntpd\"} 1\n"),
	   "a file with only starts still counts restarts");
	ck(has("# TYPE gpon_service_restarts_total counter\n") && has("# TYPE gpon_service_backoff gauge\n") &&
	   has("# TYPE gpon_service_degraded gauge\n") && has("# TYPE gpon_service_backoff_seconds gauge\n"),
	   "every family has its TYPE line, the counter as a counter");

	/* Backoff, applied to the latest start the way respawn.sh says. */
	ck(has("\ngpon_service_backoff{service=\"dropbear\"} 1\n") &&
	   has("\ngpon_service_backoff_seconds{service=\"dropbear\"} 2\n") &&
	   has("\ngpon_service_degraded{service=\"dropbear\"} 0\n"),
	   "backoff, started 5 s ago of 30: in backoff, a 2 s wait, not degraded");
	ck(has("\ngpon_service_backoff{service=\"syslogd\"} 1\n") &&
	   has("\ngpon_service_degraded{service=\"syslogd\"} 1\n") &&
	   has("\ngpon_service_backoff_seconds{service=\"syslogd\"} 30\n"),
	   "degraded, started 10 s ago: crash-looping, the 30 s cap");
	ck(has("\ngpon_service_backoff{service=\"klogd\"} 0\n") &&
	   has("\ngpon_service_degraded{service=\"klogd\"} 0\n") &&
	   has("\ngpon_service_backoff_seconds{service=\"klogd\"} 0\n") &&
	   has("\ngpon_service_restarts_total{service=\"klogd\"} 4\n"),
	   "the file still says backoff but the daemon ran 800 s of 30: healthy, the count stays");
	ck(has("\ngpon_service_backoff{service=\"omcid\"} 0\n") &&
	   has("\ngpon_service_backoff{service=\"confd\"} 0\n") &&
	   has("\ngpon_service_backoff_seconds{service=\"confd\"} 0\n"), "state ok: not in backoff");

	/* The same files a little later or earlier move the gauges, not the count. */
	scrape("test/fixtures/respawn", 1024);
	ck(has("\ngpon_service_backoff{service=\"dropbear\"} 1\n"), "29 s after the start: still in backoff");
	scrape("test/fixtures/respawn", 1025);
	ck(has("\ngpon_service_backoff{service=\"dropbear\"} 0\n") &&
	   has("\ngpon_service_backoff_seconds{service=\"dropbear\"} 0\n") &&
	   has("\ngpon_service_restarts_total{service=\"dropbear\"} 2\n"),
	   "30 s after the start: recovered by itself, the count unchanged");
	scrape("test/fixtures/respawn", -1);
	ck(has("gpon_service_restarts_total{service=\"confd\"}") && !has("gpon_service_backoff") &&
	   !has("gpon_service_degraded"), "no uptime: the counter only, no guessed backoff");

	/* Malformed and unsafe input. */
	scrape("test/fixtures/respawn", 1000);
	ck(!has("gpon_service_backoff{service=\"ntpd\"}") &&
	   !has("gpon_service_degraded{service=\"ntpd\"}"),
	   "a malformed state, a bad number and a line with no equals sign: no backoff series");
	ck(!has("bad-name") && !has("omcid.tmp") && !has(".omcid"), "unsafe names and temporary files are skipped");
	ck(!has("{service=\"\"}"), "no empty label value");

	ck(respawn_parse("x", "starts=1\nstarts=9\nstate=ok\nstate=degraded\n", &s) && s.starts == 1 &&
	   s.state == RESPAWN_OK, "the first occurrence of a key counts");
	ck(respawn_parse("x", "starts=1234567890\n", &s) && s.starts == -1, "a number past nine digits is absent");
	ck(respawn_parse("x", "xstarts=4\nstarts_total=4\nstarts =4\n", &s) && s.starts == -1,
	   "keys match whole, at the start of a line");
	ck(respawn_parse("x", "starts=4", &s) && s.starts == 4, "a last line with no newline reads");
	ck(respawn_parse("x", "state=OK\n", &s) && s.state == RESPAWN_STATE_NONE, "state values match exactly");
	ck(respawn_parse("x", "starts=\n", &s) && s.starts == -1, "an empty value is absent");
	ck(!respawn_parse("a-b", "starts=1\n", &s) && !respawn_parse("a b", "", &s) &&
	   !respawn_parse("a\"b", "", &s) && !respawn_parse("a\\b", "", &s) && !respawn_parse("", "", &s) &&
	   !respawn_parse(".x", "", &s) && !respawn_parse("a/b", "", &s), "names outside [0-9A-Za-z_] are refused");
	ck(respawn_parse("a_B9", "", &s) && !strcmp(s.name, "a_B9"), "a name of [0-9A-Za-z_] is kept");
	ck(respawn_parse("0123456789012345678901234567890", "", &s) && strlen(s.name) == 31,
	   "the longest name that fits is kept whole");
	ck(!respawn_parse("01234567890123456789012345678901", "", &s), "a name of 32 characters is refused");
	ck(respawn_uptime("1234.56 987.65\n") == 1234 && respawn_uptime("0.10 0.00\n") == 0 &&
	   respawn_uptime("") == -1 && respawn_uptime("x") == -1 && respawn_uptime("12345678901.0") == -1,
	   "uptime is the whole seconds of the first token");
	ck(respawn_parse("x", "", &s) && s.last_start == -1 && s.delay_s == -1 && s.stable_s == -1,
	   "fields the file lacks are absent");

	/* Missing directory, empty directory, no entries: nothing at all. */
	ck(scrape("test/fixtures/no-such-dir", 1000) == 0 && out[0] == 0, "a missing directory emits nothing");
	ck(respawn_render(0, 0, 1000, out, sizeof out) == 0 && out[0] == 0, "no daemons emit nothing");
	ck(respawn_render(0, 0, 1000, out, 0) == 0, "a zero-size buffer is safe");

	/* The output is truncated at the buffer, NUL-terminated. */
	scrape("test/fixtures/respawn", 1000);
	{
		struct respawn_svc sv[1];
		unsigned long n = 0, got;

		respawn_parse("omcid", "state=ok\nstarts=2\nstable_s=30\nlast_start_s=1\n", &sv[0]);
		n = 1;
		got = respawn_render(sv, n, 100, small, sizeof small);
		ck(got < sizeof small && small[got] == 0 && strlen(small) == got,
		   "the output is truncated at the buffer, NUL-terminated");
	}

	/* A full table drops the rest rather than overrunning. */
	{
		struct respawn_svc sv[RESPAWN_SVC_MAX], extra;
		unsigned long n = 0, i;

		respawn_parse("z", "starts=1\n", &extra);
		for (i = 0; i < RESPAWN_SVC_MAX + 3; i++)
			respawn_add(sv, &n, &extra);
		ck(n == RESPAWN_SVC_MAX, "the table holds RESPAWN_SVC_MAX daemons and drops the rest");
	}

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("test_respawn_state: all checks passed\n");
	return 0;
}
