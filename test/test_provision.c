/*
 * Host-native test for src/provision.h: the gpon_provision_* families,
 * rendered from fixtures and compared against goldens.
 *
 *   provision-isp1.txt  `omcicli provision` after the ISP1 provisioning
 *                       session odi-oss replays in its own tests
 *                       (test/fixtures/omci-session-isp1.txt there)
 *   provision-td.txt    the same format with traffic descriptors, which
 *                       neither ISP session creates: the shape is omcid's,
 *                       the values are made up
 *   odi_gpon-*.txt      /proc/odi_gpon, its alloc_ids line in the odi-oss
 *                       kernel format, trimmed to a few other lines
 *
 * The goldens are the .prom files beside them; UPDATE=1 rewrites them. Besides
 * matching them this checks what a golden cannot show: that absent sources
 * emit nothing at all, that the vendor omcicli usage text (what a stick on
 * the stock omci_app answers) emits nothing, that a value outside
 * [0-9a-z_] never reaches a label, and that every family is contiguous and
 * announced by its # TYPE line.
 *
 * Run by `make test`; exit 0 on pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/provision.h"

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
	FILE *f = fopen(path, "rb");
	static char bufs[4][16384];
	static int next;
	char *b = bufs[next++ % 4];
	size_t n;

	if (!f) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		failures++;
		b[0] = 0;
		return b;
	}
	n = fread(b, 1, sizeof bufs[0] - 1, f);
	fclose(f);
	b[n] = 0;
	return b;
}

static void reset(void)
{
	outn = 0;
	out[0] = 0;
}

/* Every sample line belongs to the family its latest # TYPE named, and no
 * family is announced twice: Prometheus rejects both. */
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

static void golden(const char *name, const char *proc, const char *prov)
{
	char path[256];
	const char *want;

	reset();
	if (proc)
		prov_emit_tconts(&o, slurp(proc));
	if (prov)
		prov_emit_omci(&o, slurp(prov));
	check_shape(name);
	snprintf(path, sizeof path, "test/fixtures/%s.prom", name);
	if (getenv("UPDATE")) {
		FILE *f = fopen(path, "wb");

		if (f) {
			fwrite(out, 1, outn, f);
			fclose(f);
		}
		printf("updated %s\n", path);
		return;
	}
	want = slurp(path);
	if (strcmp(want, out)) {
		fprintf(stderr, "FAIL: %s differs from %s; got:\n%s", name, path, out);
		failures++;
	}
}

static void nothing(const char *what, const char *proc, const char *prov)
{
	reset();
	if (proc)
		prov_emit_tconts(&o, proc);
	if (prov)
		prov_emit_omci(&o, prov);
	if (outn) {
		fprintf(stderr, "FAIL: %s emitted:\n%s", what, out);
		failures++;
	}
}

int main(void)
{
	golden("provision-isp1", "test/fixtures/odi_gpon-o5.txt",
	       "test/fixtures/provision-isp1.txt");
	golden("provision-td", "test/fixtures/odi_gpon-noalloc.txt",
	       "test/fixtures/provision-td.txt");

	nothing("an empty /proc/odi_gpon and no omcicli answer", "", "");
	nothing("a /proc/odi_gpon from a kernel with no alloc_ids line",
		"state 5 (O5)\nonu_id 3\nploam ds_rx 1 us_tx 1\n", 0);
	nothing("the vendor omcicli usage text", 0,
		"usage: omcicli <group> <command> [args]\n"
		"  get  sn         serial number\n"
		"  mib  get        [all | classId | tableName] [entityId]\n");
	nothing("a quote in a label value", 0,
		"vlan vid=10 source=vlan\"filter\n");
	nothing("a VID that is not a number", 0,
		"vlan vid=1x source=vlan_filter\n");
	nothing("a GEM port with a direction G.988 does not define", 0,
		"gem me=2 port=657 direction=4 tcont_me=0 us_td=0 ds_td=0\n");
	nothing("a descriptor id that is not a number", 0,
		"td me=1a cir=5 pir=10 cbs=0 pbs=0\n");
	nothing("a negative rate", 0, "td me=1 cir=-5\n");

	if (failures) {
		fprintf(stderr, "test_provision: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_provision: all checks passed\n");
	return 0;
}
