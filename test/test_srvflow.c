/*
 * Host-native test for src/srvflow.h: gpon_omci_services from what
 * `omcicli dump srvflow` prints, and -1 for text that is no answer.
 *
 * The answers are the two real shapes: odi-oss omcid (the used rows, each
 * with a second indented line, and a count line; src/omci/respond/show.c
 * cli_conn() in odi-oss) and the vendor omci_app (256 rows, most Used: 0).
 * The non-answers are what odi-oss omcli prints for itself
 * (src/omci/cli/main.c in odi-oss). Run by `make test`; exit 0 on pass.
 */
#include <stdio.h>
#include <string.h>

#include "../src/srvflow.h"

static int failures;

static void ck(long got, long want, const char *what)
{
	int ok = got == want;

	printf("%s    %s (%ld)\n", ok ? "ok  " : "FAIL", what, got);
	if (!ok)
		failures++;
}

/* The text in pieces of `step` bytes, as metricsd reads it from the pipe. */
static long fed(const char *buf, unsigned long step)
{
	struct srvflow_scan s;
	unsigned long n = strlen(buf), i;

	srvflow_init(&s);
	for (i = 0; i < n; i += step)
		srvflow_feed(&s, buf + i, n - i < step ? n - i : step);
	return srvflow_end(&s);
}

static const char odi_two[] =
	"SERVID 0: Used: 1, (DIR=1, USFID=0, DSFID=1, SERVID=0, UNIMASK=1)\n"
	"    rule_gen 1  stagMode 0  ctagMode 1  tag_count 1  out_tag (pri 0, vid 7)\n"
	"SERVID 3: Used: 1, (DIR=1, USFID=2, DSFID=3, SERVID=3, UNIMASK=1)\n"
	"    rule_gen 1  stagMode 0  ctagMode 1  tag_count 1  out_tag (pri 0, vid 8)\n"
	"2 services\n";

int main(void)
{
	static char vendor[256 * 80 + 1];
	unsigned long n = 0;
	int i;

	ck(srvflow_used(odi_two), 2, "odi-oss: two services");
	ck(srvflow_used("SERVID 0: Used: 1, (DIR=1, USFID=0, DSFID=1, SERVID=0, UNIMASK=1)\n"
			"    rule_gen 1  stagMode 0  ctagMode 1  tag_count 1  out_tag (pri 0, vid 7)\n"
			"1 service\n"), 1, "odi-oss: one service");
	ck(srvflow_used("0 services\n"), 0, "odi-oss: synced, nothing installed");
	ck(srvflow_used("0 services"), 0, "the count line without a newline");
	ck(srvflow_used("0 services\r\n"), 0, "the count line with CRLF");

	for (i = 0; i < 256; i++)
		n += (unsigned long)sprintf(vendor + n,
			"SERVID %d: Used: %d, (DIR=0, USFID=0, DSFID=0, SERVID=0, UNIMASK=0)\n",
			i, i == 1 || i == 7 || i == 200);
	ck(srvflow_used(vendor), 3, "vendor: 256 rows, three used");
	ck((long)n > 8192, 1, "vendor: the dump is longer than the 8 KB metricsd read before");
	vendor[0] = 0;
	for (i = 0, n = 0; i < 256; i++)
		n += (unsigned long)sprintf(vendor + n,
			"SERVID %d: Used: %d, (DIR=0, USFID=0, DSFID=0, SERVID=0, UNIMASK=0)\n",
			i, i == 200);
	ck(fed(vendor, 1), 1, "vendor: the only row in use is row 200, fed a byte at a time");
	ck(fed(vendor, 7), 1, "  seven at a time");
	ck(fed(vendor, 512), 1, "  in the 512-byte reads metricsd makes");
	ck(fed(vendor, sizeof(vendor)), 1, "  whole");
	vendor[0] = 0;
	for (i = 0, n = 0; i < 256; i++)
		n += (unsigned long)sprintf(vendor + n,
			"SERVID %d: Used: 0, (DIR=0, USFID=0, DSFID=0, SERVID=0, UNIMASK=0)\n", i);
	ck(srvflow_used(vendor), 0, "vendor: 256 rows, none used");

	ck(srvflow_used("% no command queue at key 0x800 -- is omci_app running? (-2)\n"),
	   -1, "no queue: not an answer");
	ck(srvflow_used("% send failed\n"), -1, "a full queue nobody reads: not an answer");
	ck(srvflow_used("% the daemon did not answer; the dump may be incomplete\n"),
	   -1, "no answer to the barrier: not an answer");
	ck(srvflow_used("% could not send the completion barrier\n"), -1,
	   "the barrier not sent: not an answer");
	ck(srvflow_used("SERVID 0: Used: 0, (DIR=0)\n% the daemon did not answer; the dump may be incomplete\n"),
	   -1, "an unused row, then the dump may be incomplete: no count");
	ck(srvflow_used("SERVID 0: Used: 1, (DIR=1)\n% the daemon stopped answering mid-stream\n"),
	   -1, "a used row, then the daemon stopped mid-stream: no count either");
	ck(srvflow_used("SERVID 0: Used: 1, (DIR=1)\n1 service\n% could not send the completion barrier\n"),
	   -1, "even with a count line before it");
	ck(srvflow_used("SERVID 0: Used: 1, (DIR=1)\n% note: reply echoes command 2, not 3\n1 service\n"),
	   1, "another line of omcicli's own is no reason to drop the count");
	ck(srvflow_used("\n"), -1, "an empty line: not an answer");
	ck(srvflow_used("Used: 1\n"), -1, "Used: 1 outside a SERVID row");
	ck(srvflow_used("services\n"), -1, "a count line without the count");
	ck(srvflow_used("2 services and more\n"), -1, "a count line with more after it");
	ck(srvflow_used("x SERVID 1: Used: 1\n"), -1, "SERVID not at the start of a line");
	ck(srvflow_used("SERVID 1: Used: 0\n    Used: 1\n0 services\n"), 0,
	   "Used: 1 on a line that is no row is not counted");
	ck(srvflow_used("SERVID 1: Used: 1"), 1, "a row cut before its newline");
	ck(srvflow_used("SERVID 1: Used: "), -1, "a row cut before its value: not an answer");
	ck(srvflow_used("SERVID 1: Used"), -1, "a row cut inside the field: not an answer");
	ck(srvflow_used("SERVID 1: Used: 10, (DIR=0)\n"), -1, "a Used value that is not 0 or 1: not an answer");
	ck(srvflow_used("SERVID 1: Used: 1, (DIR=0)\nSERVID 2: Used: \n"), 1,
	   "a cut row after a whole one adds nothing");
	ck(srvflow_used("3 services\n"), 3, "the count line alone: its number");
	ck(srvflow_used("SERVID 0: Used: 1, (DIR=1)\n1 service\nSERVID 4: Used: 1, (DIR=1)\n2 services\n"), 2,
	   "the count line is the answer when there is one");
	ck(srvflow_used("1234567 services\n"), -1, "a count of more than six digits: not an answer");
	{
		char row[400];

		memset(row, 'x', sizeof(row));
		memcpy(row, "SERVID 9: Used: 1, (", 20);
		row[sizeof(row) - 2] = '\n';
		row[sizeof(row) - 1] = 0;
		ck(fed(row, 5), 1, "a row longer than the line kept still counts");
		memset(row, '1', sizeof(row));
		memcpy(row + sizeof(row) - 12, " services\n", 11);
		ck(fed(row, 64), -1, "a count line longer than the line kept is no count line");
	}

	if (failures) {
		printf("test_srvflow: %d failed\n", failures);
		return 1;
	}
	printf("test_srvflow: all checks passed\n");
	return 0;
}
