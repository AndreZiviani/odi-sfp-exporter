/*
 * Host-native test for src/gpon_flow.h: the per-GEM-port families, rendered
 * from /proc/odi_gpon/flows and compared against a golden.
 *
 *   fixtures/odi_gpon-counters/flows  the golden of odi-oss
 *                                 test/odi_gpon_flow_test.c, byte for byte
 *
 * The golden is fixtures/odi_gpon-flows.prom; UPDATE=1 rewrites it. Besides
 * matching it this checks what a golden cannot show: that a file with no flow
 * line (a stock kernel, an older odi-oss one) emits nothing, that a name the
 * table does not know is skipped, that a value that is not plain digits
 * never gets out, that a total past 10 digits and past 2^32 comes out whole,
 * that a second flow on the same port in the same direction is dropped, that
 * the same port in the two directions is two series, that a last line the
 * read cut short is ignored, and that every family is contiguous and
 * announced once by its # TYPE line.
 *
 * Run by `make test`; exit 0 on pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gpon_flow.h"

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
	flow_emit(&o, proc);
}

/* Every sample line belongs to the family its latest # TYPE named, and no
 * family is announced twice: Prometheus rejects both. Nor may a series, the
 * name and labels, appear twice. */
static void check_shape(const char *what)
{
	char seen[64][96], cur[96] = "";
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
			if (nseen < 64)
				snprintf(seen[nseen++], sizeof seen[0], "%s", cur);
		} else if (l[0] != '#') {
			size_t k = 0;
			const char *m;

			while (k < n && l[k] != '{' && l[k] != ' ')
				k++;
			if (!cur[0] || strlen(cur) != k || strncmp(l, cur, k)) {
				fprintf(stderr, "FAIL: %s: sample outside its family: %.*s\n",
					what, (int)n, l);
				failures++;
			}
			/* the series, up to the value, must not appear earlier */
			for (m = out; m < l; ) {
				const char *me = strchr(m, '\n');
				size_t mn = me ? (size_t)(me - m) : strlen(m);
				size_t a = 0, b = 0;

				while (a < mn && m[a] != ' ')
					a++;
				while (b < n && l[b] != ' ')
					b++;
				if (m[0] != '#' && a == b && !strncmp(m, l, a)) {
					fprintf(stderr, "FAIL: %s: series twice: %.*s\n", what, (int)a, l);
					failures++;
				}
				m += mn;
				if (*m)
					m++;
			}
		}
		l += n;
		if (*l)
			l++;
	}
}

static void golden(const char *name, const char *txt)
{
	char path[256], *want;
	static char got[65536];

	render(slurp(txt));
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

static void hasnot(const char *what, const char *proc, const char *text)
{
	render(proc);
	check_shape(what);
	if (strstr(out, text)) {
		fprintf(stderr, "FAIL: %s: \"%s\" in:\n%s", what, text, out);
		failures++;
	}
}

int main(void)
{
	/* provision.h comes along for its writer; its emitters are tested in
	 * test_provision.c. */
	(void)prov_emit_tconts;
	(void)prov_emit_omci;

	golden("odi_gpon-flows", "test/fixtures/odi_gpon-counters/flows");

	nothing("an empty file", "");
	nothing("a stock-like status text", "state 5 (O5)\nalloc_ids 2 282 794\nctr ds_gem_idle 5\n");
	nothing("flow tallies of 0 and no flow", "flow_ack_timeouts\n");
	nothing("a line that only starts like a direction", "ds_superframe 5\nds1 1 gem 2 gem_frames 3\n");
	nothing("a flow with no GEM port", "ds 1 gem gem_frames 3\n");
	nothing("a flow index that is not a number", "ds x gem 1434 gem_frames 3\n");
	nothing("a GEM port that is not a number", "ds 1 gem 14a4 gem_frames 3\n");
	nothing("a name this exporter does not know", "ds 1 gem 1434 something_new 7\n");
	nothing("a value that is not a number", "ds 1 gem 1434 gem_frames 12a bytes -3\n");
	nothing("a value past 20 digits", "ds 1 gem 1434 bytes 123456789012345678901\n");
	nothing("a name with no value after it", "ds 1 gem 1434 gem_frames\n");
	nothing("a last line the read cut short", "ds 1 gem 1434 gem_frames 12");
	nothing("a quote in a port", "us 1 gem 14\"4 gem_frames 3\n");

	has("a total past 2^32 and 10 digits", "us 4 gem 1566 bytes 1234567890123456\n",
	    "gpon_gem_bytes_total{direction=\"us\",gem_port=\"1566\"} 1234567890123456\n");
	has("the 20-digit total", "ds 0 gem 1 bytes 18446744073709551615\n",
	    "gpon_gem_bytes_total{direction=\"ds\",gem_port=\"1\"} 18446744073709551615\n");
	has("rx", "ds 1 gem 1434 gem_frames 20 bytes 3 eth_rx 19 eth_fwd 18\n",
	    "gpon_gem_eth_frames_total{direction=\"ds\",gem_port=\"1434\",kind=\"rx\"} 19\n");
	has("fwd", "ds 1 gem 1434 gem_frames 20 bytes 3 eth_rx 19 eth_fwd 18\n",
	    "gpon_gem_eth_frames_total{direction=\"ds\",gem_port=\"1434\",kind=\"fwd\"} 18\n");
	has("tx", "us 4 gem 1566 gem_frames 330 eth_frames 50 bytes 7\n",
	    "gpon_gem_eth_frames_total{direction=\"us\",gem_port=\"1566\",kind=\"tx\"} 50\n");
	has("a downstream line has no tx", "ds 1 gem 1434 gem_frames 20 bytes 3 eth_rx 19 eth_fwd 18\n",
	    "gpon_gem_frames_total{direction=\"ds\",gem_port=\"1434\"} 20\n");
	hasnot("a downstream line has no tx", "ds 1 gem 1434 gem_frames 20 eth_frames 5\n", "kind=\"tx\"");
	has("the fields in any order", "us 4 gem 1566 bytes 7 eth_frames 50 gem_frames 330\n",
	    "gpon_gem_frames_total{direction=\"us\",gem_port=\"1566\"} 330\n");
	has("a name the table does not know beside known ones",
	    "ds 1 gem 1434 later 9 gem_frames 20\n",
	    "gpon_gem_frames_total{direction=\"ds\",gem_port=\"1434\"} 20\n");

	/* A port on two flows of one direction is one series: the first line. */
	has("the first of two flows on one port", "ds 1 gem 1434 gem_frames 20\nds 2 gem 1434 gem_frames 99\n",
	    "gpon_gem_frames_total{direction=\"ds\",gem_port=\"1434\"} 20\n");
	hasnot("the second of two flows on one port", "ds 1 gem 1434 gem_frames 20\nds 2 gem 1434 gem_frames 99\n",
	       " 99\n");
	has("the same port downstream and upstream", "ds 1 gem 1434 gem_frames 20\nus 1 gem 1434 gem_frames 7\n",
	    "gpon_gem_frames_total{direction=\"us\",gem_port=\"1434\"} 7\n");
	has("the same port downstream and upstream, ds", "ds 1 gem 1434 gem_frames 20\nus 1 gem 1434 gem_frames 7\n",
	    "gpon_gem_frames_total{direction=\"ds\",gem_port=\"1434\"} 20\n");

	has("the tallies", "flow_ack_timeouts 3\nflow_saturated 1\n",
	    "gpon_gem_flow_count_faults_total{kind=\"ack_timeout\"} 3\n");
	has("the tallies, saturated", "flow_ack_timeouts 3\nflow_saturated 1\n",
	    "gpon_gem_flow_count_faults_total{kind=\"saturated\"} 1\n");
	nothing("a tally with no number", "flow_saturated x\nflow_ack_timeouts 1 2\n");

	if (failures) {
		fprintf(stderr, "test_gpon_flow: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_gpon_flow: all checks passed\n");
	return 0;
}
