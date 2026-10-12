/*
 * Host-native test for src/gpon_link.h: the GPON link families, rendered
 * from /proc/odi_gpon and compared against a golden.
 *
 *   odi_gpon-counters.txt  a whole /proc/odi_gpon of an odi-oss kernel with
 *                          the link counters: its counter block is the
 *                          golden of odi-oss test/odi_gpon_ctr_test.c, byte
 *                          for byte, between the older lines and a full
 *                          PLOAM ring
 *
 * The golden is odi_gpon-counters.prom; UPDATE=1 rewrites it. Besides
 * matching it this checks what a golden cannot show: that a /proc file with
 * none of these lines (a stock kernel, an older odi-oss one) emits nothing,
 * that a counter name the table does not know is skipped, that a value or
 * label that is not plain digits or [0-9A-Za-z_] never gets out, that
 * milliseconds come out as seconds, and that every family is contiguous
 * and announced once by its # TYPE line.
 *
 * Run by `make test`; exit 0 on pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gpon_link.h"

static char out[65536];
static unsigned long outn;
static int failures;

static void put(void *ctx, const char *s, unsigned long n)
{
	(void)ctx;
	if (outn + n >= sizeof out) {
		fprintf(stderr, "FAIL: output overflow\n");
		failures++;
		return;
	}
	memcpy(out + outn, s, n);
	outn += n;
	out[outn] = 0;
}

static const struct prov_out o = { put, 0 };

static char *slurp(const char *path)
{
	static char buf[65536];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (!f) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		failures++;
		buf[0] = 0;
		return buf;
	}
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return buf;
}

static void render(const char *proc)
{
	outn = 0;
	out[0] = 0;
	link_emit(&o, proc);
}

/* Every sample line belongs to the family its latest # TYPE named, and no
 * family is announced twice: Prometheus rejects both. */
static void check_shape(const char *what)
{
	char seen[128][96], cur[96] = "";
	int nseen = 0;
	const char *l = out;

	while (*l) {
		const char *e = strchr(l, '\n');
		size_t n = e ? (size_t)(e - l) : strlen(l);

		if (!strncmp(l, "# TYPE ", 7)) {
			size_t k = 7;

			while (k < n && l[k] != ' ')
				k++;
			snprintf(cur, sizeof cur, "%.*s", (int)(k - 7), l + 7);
			for (int i = 0; i < nseen; i++)
				if (!strcmp(seen[i], cur)) {
					fprintf(stderr, "FAIL: %s: family %s announced twice\n", what, cur);
					failures++;
				}
			if (nseen < 128)
				snprintf(seen[nseen++], sizeof seen[0], "%s", cur);
		} else if (l[0] != '#') {
			size_t k = 0;

			while (k < n && l[k] != '{' && l[k] != ' ')
				k++;
			if (!cur[0] || strlen(cur) != k || strncmp(l, cur, k)) {
				fprintf(stderr, "FAIL: %s: sample outside its family: %.*s\n",
					what, (int)n, l);
				failures++;
			}
		}
		l += n;
		if (*l)
			l++;
	}
}

static void golden(const char *name, const char *proc_path)
{
	char path[256], *want;
	static char got[65536];

	render(slurp(proc_path));
	check_shape(name);
	memcpy(got, out, outn + 1);
	snprintf(path, sizeof path, "test/fixtures/%s.prom", name);
	if (getenv("UPDATE")) {
		FILE *f = fopen(path, "wb");

		if (f) {
			fwrite(got, 1, outn, f);
			fclose(f);
		}
		printf("updated %s\n", path);
		return;
	}
	want = slurp(path);
	if (strcmp(want, got)) {
		fprintf(stderr, "FAIL: %s differs from %s; got:\n%s", name, path, got);
		failures++;
	}
}

static void nothing(const char *what, const char *proc)
{
	render(proc);
	if (outn) {
		fprintf(stderr, "FAIL: %s emitted:\n%s", what, out);
		failures++;
	}
}

static void has(const char *what, const char *proc, const char *line)
{
	render(proc);
	check_shape(what);
	if (!strstr(out, line)) {
		fprintf(stderr, "FAIL: %s: no \"%s\" in:\n%s", what, line, out);
		failures++;
	}
}

static void every_table_name_known(void)
{
	unsigned long f;

	for (f = 0; f < LINK_NFAMS; f++) {
		const char *t = link_fams[f].type;
		unsigned long n = strlen(link_fams[f].name);
		int counter = !strcmp(t, "counter");

		if (!counter && strcmp(t, "gauge")) {
			fprintf(stderr, "FAIL: %s: type %s\n", link_fams[f].name, t);
			failures++;
		}
		if (counter != (n > 6 && !strcmp(link_fams[f].name + n - 6, "_total"))) {
			fprintf(stderr, "FAIL: %s: a counter is named *_total, a gauge is not\n",
				link_fams[f].name);
			failures++;
		}
	}
}

int main(void)
{
	/* provision.h comes along for its writer; its emitters are tested in
	 * test_provision.c. */
	(void)prov_emit_tconts;
	(void)prov_emit_omci;

	every_table_name_known();
	golden("odi_gpon-counters", "test/fixtures/odi_gpon-counters.txt");

	nothing("an empty /proc/odi_gpon", "");
	nothing("an odi-oss /proc/odi_gpon from before the counters, no PLOAM line",
		"state 5 (O5)\nsn 0000000000000000\nalloc_ids 2 282 794\n");
	nothing("a counter this exporter does not know", "ctr ds_something_new 12\n");
	nothing("a negative ONU-ID (none assigned)", "onu_id -1\n");
	nothing("a value that is not a number", "ctr ds_bip_err_blocks 12a\n");
	nothing("a quote in a label value", "alarm_events lo\"s 1\n");
	nothing("a label with no value after it", "alarm_events los\n");
	nothing("a line that only starts like a key", "ctr_samplesx 3\nploam_ring 3 entries\n");

	has("milliseconds under a second", "alarm_ms los 5\n",
	    "gpon_alarm_seconds_total{alarm=\"los\"} 0.005\n");
	has("milliseconds of whole seconds", "onu_state_ms O5 86400000\n",
	    "gpon_onu_state_seconds_total{state=\"O5\"} 86400.000\n");
	has("a total past 2^32", "ctr ds_gem_idle 4843967295\n",
	    "gpon_ds_gem_frames_total{kind=\"idle\"} 4843967295\n");
	has("the PLOAM pair, ds", "ploam ds_rx 120 us_tx 40\n",
	    "gpon_ploam_messages_total{direction=\"ds\"} 120\n");
	has("the PLOAM pair, us", "ploam ds_rx 120 us_tx 40\n",
	    "gpon_ploam_messages_total{direction=\"us\"} 40\n");

	if (failures) {
		fprintf(stderr, "test_gpon_link: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_gpon_link: all checks passed\n");
	return 0;
}
