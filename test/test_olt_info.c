/*
 * Host-native test for src/olt_info.h: gpon_olt_info rendered from what
 * `omcicli mib get 131` prints, and the cache key from /proc/odi_gpon/status.
 *
 *   olt-g-hwtc.txt     the real block of a stick on a Huawei OLT (odi-oss
 *                      v1.16.0-beta.4): vendor HWTC, version 10, an empty
 *                      equipment ID
 *   olt-g-binary.txt   made up: a vendor id with a byte outside ASCII, an
 *                      equipment ID with a quote in it, a version with a
 *                      high byte
 *   olt-g-zero.txt     made up: an all-zero vendor id, CRLF, padded values
 *   olt-g-inject.txt   made up: an equipment ID that holds a newline and a
 *                      Version line of its own
 *   olt-g-inject2.txt  made up: an equipment ID of newline, `Version:`, newline,
 *                      `ToDInfo:` (18 bytes, it fits), the real lines behind
 *   olt-g-missing.txt  the answer for an unknown class name
 *   olt-g-empty.txt    the OLT-G banner and `0 rows`
 *   olt-g-cut.txt      the real block cut off in the middle of the version
 *
 * The goldens are the .prom files beside them; UPDATE=1 rewrites them. Run
 * by `make test`, also by hand under -fsanitize=address,undefined; exit 0 on
 * pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/olt_info.h"

static char out[4096];
static unsigned long outn;
static int failures;

static void put(void *ctx, const char *s, unsigned long n)
{
	(void)ctx;
	if (outn + n >= sizeof out)
		return;
	memcpy(out + outn, s, n);
	outn += n;
	out[outn] = 0;
}

static const struct prov_out o = { put, 0 };

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		failures++;
}

/* The fixture into a buffer of exactly its size plus the zero, so a parse
 * that leaves the text is a stack or heap overflow for the sanitizer. */
static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char tmp[4096];
	size_t n;
	char *b;

	if (!f) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		exit(1);
	}
	n = fread(tmp, 1, sizeof tmp - 1, f);
	fclose(f);
	b = malloc(n + 1);
	memcpy(b, tmp, n);
	b[n] = 0;
	return b;
}

static int render(const char *path)
{
	char *t = slurp(path);
	struct olt_info i;
	int ok = olt_parse(t, &i);

	outn = 0;
	out[0] = 0;
	if (ok)
		olt_emit(&o, &i);
	free(t);
	return ok;
}

static void golden(const char *fix, const char *gold)
{
	char *want;

	if (getenv("UPDATE")) {
		FILE *f = fopen(gold, "wb");

		fwrite(out, 1, outn, f);
		fclose(f);
		return;
	}
	want = slurp(gold);
	ck(!strcmp(out, want), fix);
	if (strcmp(out, want))
		fprintf(stderr, "--- want\n%s--- got\n%s", want, out);
	free(want);
}

/* One scrape through the decision, the way emit_olt_info runs it: returns
 * whether it forked; `read` is what the run would give (1 a good answer,
 * 0 none or unparsable); *emitted is whether the family would be there. */
static int scrape(struct olt_cache *c, unsigned long long key, long now, int omci_ok,
		  int read, int *emitted)
{
	int fork = olt_plan(c, key, now, omci_ok);

	if (fork) {
		struct olt_info got;

		memset(&got, 0, sizeof got);
		strcpy(got.vendor, "HWTC");
		olt_done(c, key, now, read, &got);
	}
	*emitted = c->ok;
	return fork;
}

static void cache_tests(void)
{
	struct olt_cache c;
	int e, i;

	memset(&c, 0, sizeof c);
	ck(scrape(&c, 14, 100, 1, 1, &e) == 1 && e, "first read forks and emits");
	ck(scrape(&c, 14, 115, 1, 1, &e) == 0 && e, "same key: no fork, still emitted");
	ck(scrape(&c, 14, 3000, 1, 1, &e) == 0 && e, "same key 48 minutes on: still kept");
	ck(scrape(&c, 14, 3700, 1, 1, &e) == 1 && e, "after OLT_MAX_AGE_S: read again");

	ck(scrape(&c, 15, 3715, 1, 0, &e) == 1 && !e,
	   "key change with a failed read: forks, nothing emitted");
	ck(scrape(&c, 15, 3730, 1, 1, &e) == 0 && !e,
	   "same key after the failure: held back, still nothing emitted");
	ck(scrape(&c, 16, 3731, 1, 1, &e) == 1 && e,
	   "key changes again: retried at once, good read emitted");

	ck(scrape(&c, 17, 3740, 0, 1, &e) == 0 && !e,
	   "key change with omcid not answering: no fork, nothing emitted");
	ck(scrape(&c, 17, 3755, 1, 1, &e) == 1 && e, "omcid back: read at once");

	/* the entity is absent: retries held back, then once a minute */
	memset(&c, 0, sizeof c);
	ck(scrape(&c, 14, 0, 1, 0, &e) == 1 && !e, "absent: first try forks");
	for (i = 15; i < 60; i += 15)
		ck(scrape(&c, 14, i, 1, 0, &e) == 0 && !e, "absent: held back inside the retry interval");
	ck(scrape(&c, 14, 60, 1, 0, &e) == 1 && !e, "absent: tried again after OLT_RETRY_S");
	ck(scrape(&c, 14, 300, 1, 1, &e) == 1 && e, "5 minute scrape: one try per scrape, found");
	ck(scrape(&c, 14, 600, 1, 1, &e) == 0 && e, "5 minute scrape: then kept");

	/* no key: at most one fork a minute, cached for that minute */
	memset(&c, 0, sizeof c);
	ck(scrape(&c, 0, 10, 1, 1, &e) == 1 && e, "key 0: read and emitted");
	ck(scrape(&c, 0, 25, 1, 1, &e) == 0 && e, "key 0: kept inside the minute");
	ck(scrape(&c, 0, 70, 1, 1, &e) == 1 && e, "key 0: read again after a minute");
	memset(&c, 0, sizeof c);
	ck(scrape(&c, 0, 10, 1, 0, &e) == 1 && !e, "key 0, stock firmware: one try");
	ck(scrape(&c, 0, 25, 1, 0, &e) == 0 && !e, "key 0, stock firmware: held back");
	ck(scrape(&c, 0, 70, 1, 0, &e) == 1 && !e, "key 0, stock firmware: one try a minute");

	/* unreadable uptime: no extra forks */
	memset(&c, 0, sizeof c);
	ck(scrape(&c, 14, -1, 1, 0, &e) == 1, "no uptime: first try");
	ck(scrape(&c, 14, -1, 1, 0, &e) == 0, "no uptime: held back");
}

int main(void)
{
	struct olt_info i;

	ck(render("test/fixtures/olt-g-hwtc.txt"), "hwtc: entity found");
	golden("hwtc matches its golden", "test/fixtures/olt-g-hwtc.prom");
	ck(strstr(out, "gpon_olt_info{vendor=\"HWTC\",version=\"10\",equipment_id=\"\"} 1\n") != NULL,
	   "hwtc: vendor HWTC, version 10, empty equipment id");

	ck(render("test/fixtures/olt-g-binary.txt"), "binary: entity found");
	golden("binary matches its golden", "test/fixtures/olt-g-binary.prom");
	ck(strstr(out, "vendor=\"4142ff\"") != NULL,
	   "binary: a non-ASCII vendor byte gives the hex up to the first zero");
	ck(strstr(out, "equipment_id=\"4f4c542d32303022\"") != NULL,
	   "binary: a quote in the text gives hex, not a broken label");
	ck(strstr(out, "version=\"ff7632\"") != NULL,
	   "binary: a high byte in the text gives hex");

	ck(render("test/fixtures/olt-g-zero.txt"), "zero: entity found");
	ck(strstr(out, "gpon_olt_info{vendor=\"\",version=\"R2.1\",equipment_id=\"\"} 1\n") != NULL,
	   "zero: all-zero vendor is empty, CRLF and padding trimmed");

	ck(!render("test/fixtures/olt-g-inject.txt") && outn == 0,
	   "a newline in a value breaks the sequence: nothing is emitted");
	ck(!render("test/fixtures/olt-g-inject2.txt") && outn == 0,
	   "injected Version and ToDInfo lines with the real ones behind: refused");
	ck(!render("test/fixtures/olt-g-missing.txt") && outn == 0,
	   "missing: nothing is emitted");
	ck(!render("test/fixtures/olt-g-empty.txt") && outn == 0,
	   "0 rows: nothing is emitted");
	ck(!render("test/fixtures/olt-g-cut.txt") && outn == 0,
	   "cut off in the version: nothing is emitted");

	ck(!olt_parse("", &i), "empty text");
	ck(!olt_parse("OltVendorId: 0x48575443\nEquipId: \nVersion: 10\n", &i),
	   "no ToDInfo line after the version: refused");
	ck(!olt_parse("OltVendorId: 0x4857544\nEquipId: \nVersion: 10\nToDInfo:\n", &i),
	   "seven hex digits are refused");
	ck(!olt_parse("OltVendorId: 0x4857544g\nEquipId: \nVersion: 10\nToDInfo:\n", &i),
	   "a non-hex character is refused");
	ck(!olt_parse("OltVendorId 48575443\nEquipId \nVersion 10\nToDInfo:\n", &i),
	   "the omcli mib form is not read");
	ck(!olt_parse("OltVendorId: 0x48575443\nVersion: 10\nEquipId: \nToDInfo:\n", &i),
	   "attributes in another order are refused");
	ck(olt_parse("OltVendorId: 0x48575443\nEquipId: 01234567890123456789\nVersion: 01234567890123\nToDInfo:\n", &i) &&
	   !strcmp(i.equip, "01234567890123456789") && !strcmp(i.version, "01234567890123"),
	   "20 bytes of equipment id and 14 of version are accepted");
	ck(!olt_parse("OltVendorId: 0x48575443\nEquipId: 012345678901234567890\nVersion: 1\nToDInfo:\n", &i),
	   "21 bytes of equipment id are refused");
	ck(!olt_parse("OltVendorId: 0x48575443\nEquipId: \nVersion: 012345678901234\nToDInfo:\n", &i),
	   "15 bytes of version are refused");
	ck(olt_parse("OltVendorId: 0x48575443\nEquipId: \nVersion: a\\b\nToDInfo:\n", &i) &&
	   !strcmp(i.version, "615c62"), "a backslash gives hex");
	ck(olt_parse("OltVendorId: 0x5c000000\nEquipId: \nVersion: 1\nToDInfo:\n", &i) &&
	   !strcmp(i.vendor, "5c"), "a backslash in the vendor id gives hex");
	ck(!olt_parse("OltVendorId: 0x48575443\nEquipId: \nVersion: 1\nToDInfo:\nEquipId: x\n", &i),
	   "a second EquipId behind the block: refused");
	ck(olt_parse("OltVendorId: 0x48575443\nEquipId: \nVersion: 1\nToDInfo:\n\tVersion: 2\n", &i),
	   "an indented line behind the block is not an attribute");
	ck(olt_parse("OltVendorId: 0x48575443\nOltVendorId: 0x41414141\nEquipId: \nVersion: 10\nToDInfo:\n", &i) == 0,
	   "a second vendor line between the others breaks the sequence");

	cache_tests();

	ck(olt_state_key("state 5 (O5)\nonu_transitions O1 O2 1\nonu_transitions O2 O3 1\n"
			 "onu_transitions O4 O5 12\n") == 14, "state key sums the counts");
	ck(olt_state_key("state 5 (O5)\nonu_id 3\n") == 0, "no transitions: key 0");
	ck(olt_state_key("") == 0, "empty status: key 0");

	return failures ? 1 : 0;
}
