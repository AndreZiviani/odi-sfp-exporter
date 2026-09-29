/*
 * Host-native test for src/resetinfo.h -- the parse of /proc/odi_ramlog_prev
 * behind gpon_boot_count and gpon_last_reset_reason. Built with the HOST
 * compiler, like test_wrap.c: resetinfo.h has no syscalls and no asm.
 * `make test` builds and runs it.
 */
#include <stdio.h>
#include <string.h>
#include "../src/resetinfo.h"

static int failures = 0;

#define CHECK(cond, fmt, ...) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: " fmt "\n", \
				__FILE__, __LINE__, __VA_ARGS__); \
			failures++; \
		} \
	} while (0)

#define THIS "this boot: boot=31 slot=1\n"
#define PAGES "---- page A: first 3984 bytes ----\nodi_wdt: reason=bogus\n"

static void expect(const char *text, int ok, const char *boot,
		   const char *reason, const char *client)
{
	struct resetinfo ri;
	int got = resetinfo_parse(text, &ri);

	CHECK(got == ok, "parse returned %d, want %d, for [%s]", got, ok, text);
	CHECK(strcmp(ri.boot, boot) == 0, "boot [%s], want [%s]", ri.boot, boot);
	CHECK(strcmp(ri.reason, reason) == 0, "reason [%s], want [%s], for [%s]",
	      ri.reason, reason, text);
	CHECK(strcmp(ri.client, client) == 0, "client [%s], want [%s]", ri.client, client);
}

int main(void)
{
	/* Every reason the kernel writes, on the line it writes it. */
	expect(THIS "previous boot: boot=30 slot=1 build=v1.0.9 crumb=TICK/15028 reason=wdt_client:omcid\n" PAGES,
	       1, "31", "wdt_client", "omcid");
	expect(THIS "previous boot: boot=30 slot=1 build=v1.0.9 crumb=TICK/15028 reason=wdt_mem\n" PAGES,
	       1, "31", "wdt_mem", "");
	expect(THIS "previous boot: boot=30 slot=1 build=v1.0.9 crumb=TICK/15028 reason=reboot\n" PAGES,
	       1, "31", "reboot", "");
	expect(THIS "previous boot: boot=30 slot=? build=? crumb=K1EN/1 reason=unknown\n",
	       1, "31", "unknown", "");
	expect("this boot: boot=1 slot=0\n"
	       "previous boot: none (page A magic 0x55555555, page B magic 0x55555555) reason=power\n",
	       1, "1", "power", "");
	expect(THIS "previous boot: no metadata block (an older image) crumb=0x00000000/0 reason=unknown\n",
	       1, "31", "unknown", "");

	/* An older kernel: a boot counter, no reason= -- the reason is absent,
	 * and a reason= in the page text below is never taken for one. */
	expect(THIS "previous boot: boot=30 slot=1 build=v1.0.8 crumb=TICK/15028\n" PAGES,
	       1, "31", "", "");

	/* Anything that could not be a label value is dropped, not escaped. */
	expect(THIS "previous boot: boot=30 reason=wdt_client:om\"cid\n", 1, "31", "", "");
	expect(THIS "previous boot: boot=30 reason=pa{nic\n", 1, "31", "", "");
	expect(THIS "previous boot: boot=30 reason=wdt_client:\n", 1, "31", "", "");
	expect(THIS "previous boot: boot=30 reason=wdt_client:0123456789abcdefghijklmnopq\n",
	       1, "31", "", "");

	/* A read cut short mid-value reports nothing that may be truncated. */
	expect(THIS "previous boot: boot=30 slot=1 reason=wdt_cli", 1, "31", "", "");
	expect("this boot: boot=3", 0, "", "", "");

	/* Not the file at all. */
	expect("", 0, "", "", "");
	expect("garbage\nmore garbage\n", 0, "", "", "");
	expect("this boot: slot=1\n", 0, "", "", "");

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("test_resetinfo: all checks passed\n");
	return 0;
}
