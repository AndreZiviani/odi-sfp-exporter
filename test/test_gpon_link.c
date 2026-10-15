/*
 * Host-native test for src/gpon_link.h: the GPON link families, rendered
 * from /proc/odi_gpon/ and compared against a golden.
 *
 *   odi_gpon-counters/     a whole /proc/odi_gpon/ of an odi-oss kernel with
 *                          the link counters, one file per file of the
 *                          directory: counters is the golden of odi-oss
 *                          test/odi_gpon_ctr_test.c, byte for byte, status
 *                          holds the sampler lines of that test too, and
 *                          ploam ends in a full PLOAM ring
 *
 * The golden is odi_gpon-counters.prom; UPDATE=1 rewrites it. Besides
 * matching it this checks what a golden cannot show: that a /proc file with
 * none of these lines (a stock kernel, an older odi-oss one) emits nothing,
 * that a counter name the table does not know is skipped, that a value or
 * label that is not plain digits or [0-9A-Za-z_] never gets out, that
 * milliseconds come out as seconds, and that every family is contiguous
 * and announced once by its # TYPE line. The directory is read the way the
 * exporter reads it, through gpon_proc_load(), and the whole provision part
 * of a scrape (gpon_provision_scrape()) runs here with none of the files
 * there: the stock firmware has no /proc/odi_gpon, and the omcicli families
 * must come out as they do anywhere else.
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

/* /proc/odi_gpon/<file> read from <fixture_dir>/<file>, the way read_file()
 * reads the real one; a file the directory lacks (or one named in hide)
 * fails like a file that is not there. */
static const char *fixture_dir;
static const char *hide;

static long fake_read(const char *path, char *buf, unsigned long cap)
{
	char p[256];
	const char *name = path + sizeof(GPON_PROC_DIR);	/* past the slash */
	FILE *f;
	size_t n;

	if (hide && !strcmp(name, hide))
		return -2;
	snprintf(p, sizeof p, "%s/%s", fixture_dir, name);
	f = fopen(p, "rb");
	if (!f)
		return -2;
	n = fread(buf, 1, cap - 1, f);
	fclose(f);
	buf[n] = 0;
	return (long)n;
}

static char *load_dir(const char *dir)
{
	static char buf[65536];

	fixture_dir = dir;
	gpon_proc_load(fake_read, buf, sizeof buf);
	return buf;
}

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

static void golden(const char *name, const char *proc_dir)
{
	char path[256], *want;
	static char got[65536];

	render(load_dir(proc_dir));
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

/* The omcicli answer of a scrape, from the provision fixture. */
static const char *provision_text;

static long fake_omcicli(char *buf, unsigned long cap)
{
	unsigned long n = strlen(provision_text);

	if (n >= cap)
		n = cap - 1;
	memcpy(buf, provision_text, n);
	return (long)n;
}

static long fake_omcicli_fail(char *buf, unsigned long cap)
{
	(void)buf;
	(void)cap;
	return -1;
}

static void scrape(gpon_omcicli_fn om, int omci_ok)
{
	static char buf[8192];

	outn = 0;
	out[0] = 0;
	gpon_provision_scrape(&o, fake_read, om, omci_ok, buf, sizeof buf);
}

static void expect(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s; got:\n%s", what, out);
		failures++;
	}
}

static void scrape_tests(void)
{
	static char omci_only[16384];

	provision_text = slurp("test/fixtures/provision-isp1.txt");
	/* What omcicli alone gives, the reference for a scrape with no /proc. */
	outn = 0;
	out[0] = 0;
	prov_emit_omci(&o, provision_text);
	memcpy(omci_only, out, outn + 1);
	expect(outn > 0, "the omcicli fixture renders something");

	/* The stock firmware: no /proc/odi_gpon at all. */
	fixture_dir = "test/fixtures/no-such-dir";
	scrape(fake_omcicli, 1);
	expect(!strcmp(out, omci_only),
	       "no /proc/odi_gpon files: exactly the omcicli families, nothing else");
	check_shape("scrape without /proc/odi_gpon");
	scrape(fake_omcicli, 0);
	expect(outn == 0, "no /proc/odi_gpon files and no omcid: nothing, no error");
	scrape(fake_omcicli_fail, 1);
	expect(outn == 0, "no /proc/odi_gpon files and a failed omcicli: nothing");

	/* An older odi-oss kernel: /proc/odi_gpon is one file, so every
	 * /proc/odi_gpon/<file> open fails the same way. Covered by the same
	 * reader answer; here the opposite cases: only some files present. */
	fixture_dir = "test/fixtures/odi_gpon-counters";
	hide = "counters";
	scrape(fake_omcicli, 1);
	expect(strstr(out, "gpon_provision_tconts") && strstr(out, "gpon_alarm_events_total") &&
	       strstr(out, "gpon_ploam_messages_total") && !strstr(out, "gpon_ds_bip_error_blocks_total") &&
	       strstr(out, "gpon_provision_gem_ports"),
	       "no counters file: only the counter metrics are gone");
	check_shape("scrape without counters");
	hide = "status";
	scrape(fake_omcicli, 1);
	expect(!strstr(out, "gpon_provision_tconts") && strstr(out, "gpon_ds_bip_error_blocks_total") &&
	       strstr(out, "gpon_ploam_messages_total") && strstr(out, "gpon_provision_gem_ports"),
	       "no status file: no T-CONTs, the rest stays");
	hide = "ploam";
	scrape(fake_omcicli, 1);
	expect(strstr(out, "gpon_provision_tconts") && strstr(out, "gpon_ds_bip_error_blocks_total") &&
	       !strstr(out, "gpon_ploam_messages_total") && strstr(out, "gpon_provision_gem_ports"),
	       "no ploam file: only the PLOAM metrics are gone");
	hide = "flows";
	scrape(fake_omcicli, 1);
	expect(strstr(out, "gpon_provision_tconts") && strstr(out, "gpon_ds_bip_error_blocks_total") &&
	       strstr(out, "gpon_ploam_messages_total") && !strstr(out, "gpon_gem_") &&
	       strstr(out, "gpon_provision_gem_ports"),
	       "no flows file (an older odi-oss kernel): only the per-flow metrics are gone");
	check_shape("scrape without flows");
	hide = 0;
	scrape(fake_omcicli, 1);
	expect(strstr(out, "gpon_provision_tconts") && strstr(out, "gpon_ds_bip_error_blocks_total") &&
	       strstr(out, "gpon_ploam_messages_total") && strstr(out, "gpon_provision_gem_ports"),
	       "every file present: all the families");
	expect(strstr(out, "gpon_gem_frames_total{direction=\"ds\",gem_port=\"1434\"} 20000\n") &&
	       strstr(out, "gpon_gem_bytes_total{direction=\"us\",gem_port=\"1566\"} 4294994796\n"),
	       "every file present: the per-flow families too");
	check_shape("scrape with every file");
}

int main(void)
{
	/* provision.h comes along for its writer; its emitters are tested in
	 * test_provision.c. */
	(void)prov_emit_tconts;
	(void)prov_emit_omci;

	every_table_name_known();
	golden("odi_gpon-counters", "test/fixtures/odi_gpon-counters");
	scrape_tests();

	nothing("an empty /proc/odi_gpon/ text", "");
	nothing("an odi-oss /proc/odi_gpon/status from before the counters, no PLOAM line",
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
	has("a total of 12 digits", "ctr ds_gem_idle 810127539369\n",
	    "gpon_ds_gem_frames_total{kind=\"idle\"} 810127539369\n");
	has("upstream bytes past 10 digits", "ctr us_gem_bytes 10648444509\n",
	    "gpon_us_gem_bytes_total 10648444509\n");
	has("the largest 64-bit total", "ctr ds_gem_idle 18446744073709551615\n",
	    "gpon_ds_gem_frames_total{kind=\"idle\"} 18446744073709551615\n");
	has("milliseconds past 2^32", "onu_state_ms O5 4320000000\n",
	    "gpon_onu_state_seconds_total{state=\"O5\"} 4320000.000\n");
	nothing("21 digits, more than a 64-bit total", "ctr ds_gem_idle 184467440737095516150\n");
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
