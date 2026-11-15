/* Host-native test for src/history.h: the minute history line and its ring.
 * test/fixtures/diag-reply.txt is a full diag reply in the stock format, the
 * same one odi-oss pins for its diag (src/diag/test/exporter.golden). `make
 * test` runs it. */
#include <stdio.h>
#include <string.h>
#include "../src/history.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

static char diag[16384], los[16384], line[HIST_LINE];

static void slurp(const char *path, char *buf, unsigned long cap)
{
	FILE *f = fopen(path, "rb");
	size_t n = f ? fread(buf, 1, cap - 1, f) : 0;

	buf[n] = 0;
	if (f)
		fclose(f);
	if (!n) {
		printf("FAIL    cannot read %s\n", path);
		fails++;
	}
}

/* Replace the first "from" in s by "to", in place (buffer is big enough). */
static void swap(char *s, const char *from, const char *to)
{
	char *p = strstr(s, from);

	if (!p)
		return;
	memmove(p + strlen(to), p + strlen(from), strlen(p + strlen(from)) + 1);
	memcpy(p, to, strlen(to));
}

/* Whether the line has the pair "k=v" as a whole field. */
static int has(const char *l, const char *kv)
{
	char pad[HIST_LINE + 4], want[64];

	snprintf(pad, sizeof pad, " %s ", l);
	snprintf(want, sizeof want, " %s ", kv);
	return strstr(pad, want) != NULL;
}

static int lacks(const char *l, const char *k)
{
	char pad[HIST_LINE + 4], want[64];

	snprintf(pad, sizeof pad, " %s", l);
	snprintf(want, sizeof want, " %s=", k);
	return strstr(pad, want) == NULL;
}

static const char *status =
	"state 5 (O5)\n"
	"onu_id 3\n"
	"alarm_events los 2\n"
	"alarm_events lof 3\n"
	"alarm_events lom 0\n"
	"onu_transitions O1 O2 1\n"
	"onu_transitions O4 O5 3\n"
	"onu_transitions O5 O1 1\n"
	"onu_state_ms O1 1000\n"
	"onu_state_ms O5 86000000\n"
	"onu_transitions O5 O6 1\n";
static const char *counters =
	"ctr_samples 100\n"
	"ctr ds_bip_err_blocks 4\n"
	"ctr ds_bip_err_bits 12345678901234\n"
	"ctr ds_fec_uncorrectable_cw 7\n";
static const char *stat =
	"cpu  1000 0 500 8000 100 0 50 0 0 0\n"
	"cpu0 1000 0 500 8000 100 0 50 0 0 0\n";
static const char *meminfo =
	"MemTotal:          56000 kB\n"
	"MemFree:           20000 kB\n"
	"MemAvailable:      31000 kB\n";

static unsigned long build(const char *d, const char *st, const char *ctr, long wall)
{
	struct hist_in in = { d, st, ctr, stat, meminfo, "86512.33 170000.10\n", wall };
	unsigned long n = hist_build_line(&in, line, sizeof line);

	if (n != strlen(line))
		fails++;
	printf("        %s\n", line);
	return n;
}

static struct hist_ring ring;
static char out[HIST_KEEP * HIST_LINE + 1];

/* The ring as metricsd writes it. */
static unsigned long render(void)
{
	unsigned long k, n = 0;

	out[0] = 0;
	for (k = 0; k < ring.n; k++)
		n += (unsigned long)sprintf(out + n, "%s\n", hist_ring_line(&ring, k));
	return n;
}

int main(void)
{
	char buf[HIST_LINE];
	unsigned long i, n;

	(void)extend_octets;	/* wrap.h, used by metricsd but not here */
	(void)port_index;
	ck(hist_ring_last_up(&ring) == -1, "empty ring: no last up");
	slurp("test/fixtures/diag-reply.txt", diag, sizeof diag);

	build(diag, status, counters, 1763150000);
	ck(has(line, "t=1763150000"), "wall clock");
	ck(strncmp(line, "t=1763150000 up=86512 st=5 al=LOF rx=", 37) == 0, "key order");
	ck(has(line, "up=86512"), "uptime, whole seconds");
	ck(has(line, "st=5"), "ONU state from diag");
	ck(has(line, "al=LOF"), "LOF asserted (the reply has it occur)");
	ck(has(line, "rx=-22.92"), "rx power, two decimals");
	ck(has(line, "tx=2.29"), "tx power, rounded up");
	ck(has(line, "tc=42.36"), "temperature");
	ck(has(line, "bi=14.35"), "bias");
	ck(has(line, "di=3000"), "fibre port octets in (port 2 only)");
	ck(has(line, "do=18005"), "fibre port octets out");
	ck(has(line, "los=2") && has(line, "lof=3") && has(line, "lom=0"), "alarm events");
	ck(has(line, "o5i=3"), "into O5");
	ck(has(line, "o5o=2"), "out of O5");
	ck(has(line, "bip=12345678901234"), "BIP bits, a 64-bit total kept whole");
	ck(has(line, "fecu=7"), "uncorrectable FEC");
	ck(has(line, "cb=1550"), "cpu busy (all but idle and iowait)");
	ck(has(line, "ct=9650"), "cpu total");
	ck(has(line, "ma=31000"), "MemAvailable");

	build(diag, 0, 0, 1763150000);
	ck(lacks(line, "los") && lacks(line, "o5i") && lacks(line, "bip"),
	   "stock slot: no driver keys");
	ck(has(line, "st=5") && has(line, "rx=-22.92"), "stock slot: diag still answers");

	build(0, status, counters, -1);
	ck(lacks(line, "t"), "clock unknown: no t");
	ck(lacks(line, "rx") && lacks(line, "al") && lacks(line, "di"), "diag failed: no diag keys");
	ck(has(line, "st=5"), "diag failed: state from the driver");
	ck(has(line, "ma=31000"), "diag failed: the rest is there");

	memcpy(los, diag, strlen(diag) + 1);
	swap(los, "Rx Power: -22.924298  dBm", "% transceiver read failed");
	swap(los, "Alarm LOS, status: clear", "Alarm LOS, status: occur");
	swap(los, "Operation State(O5)", "Operation State(O1)");
	build(los, status, counters, 1763150000);
	ck(lacks(line, "rx"), "LOS: no rx reading");
	ck(has(line, "al=LOS,LOF"), "LOS: alarms");
	ck(has(line, "st=1"), "LOS: state from diag before the driver");

	swap(los, "Alarm LOF, status: occur", "Alarm LOF, status: clear");
	swap(los, "Alarm LOS, status: occur", "Alarm LOS, status: clear");
	build(los, 0, 0, 1);
	ck(has(line, "al=-"), "no alarm: al=-");

	{
		struct hist_val v = { {0}, 0 };

		hist_fig(&v, "Tx Power: -0.004 dBm", 20);
		ck(v.set && strcmp(v.s, "0.00") == 0, "rounds -0.004 to 0.00, no minus zero");
		v.set = 0;
		hist_fig(&v, "Tx Power: -0.005 dBm", 20);
		ck(v.set && strcmp(v.s, "-0.01") == 0, "rounds -0.005 away from zero");
		v.set = 0;
		hist_fig(&v, "Temperature: 7 C", 16);
		ck(v.set && strcmp(v.s, "7.00") == 0, "whole number gets two decimals");
		v.set = 0;
		hist_fig(&v, "Rx Power: 1e3 dBm", 17);
		ck(!v.set, "not a plain decimal: unset");
	}

	/* The ring: loads what a file holds, keeps HIST_KEEP, oldest first. */
	hist_ring_load(&ring, "up=1\ngarbage\n\nt=5 up=2\nup=x\nup=3junk\n");
	n = render();
	ck(n == strlen(out) && strcmp(out, "up=1\nt=5 up=2\n") == 0, "load keeps lines with up=");
	ck(hist_ring_last_up(&ring) == 2, "the newest up= is found");
	for (i = 3; i <= HIST_KEEP + 2; i++) {
		snprintf(buf, sizeof buf, "up=%lu", i);
		hist_ring_push(&ring, buf, strlen(buf));
	}
	n = render();
	ck(ring.n == HIST_KEEP, "ring holds HIST_KEEP lines");
	ck(strncmp(out, "up=3\nup=4\n", 10) == 0, "oldest dropped first");
	snprintf(buf, sizeof buf, "\nup=%d\n", HIST_KEEP + 2);
	ck(n >= strlen(buf) && strcmp(out + n - strlen(buf), buf) == 0, "newest last");
	{
		char big[HIST_LINE + 8];

		memset(big, 'x', sizeof big);
		hist_ring_push(&ring, big, sizeof big);
		n = render();
		ck(strncmp(out, "up=3\n", 5) == 0, "an over-long line is dropped, not cut");
	}
	ck(build(diag, status, counters, 1763150000) < HIST_LINE, "a full line fits a slot");

	/* No counter block (the driver could not take its snapshot): the
	 * transitions are unknown, not 0. */
	build(diag, "state 5 (O5)\nalarm_now los 0\n", counters, 1);
	ck(lacks(line, "o5i") && lacks(line, "o5o"), "no counter block: no o5i/o5o");
	build(diag, "state 5 (O5)\nonu_state_ms O5 100\n", counters, 1);
	ck(has(line, "o5i=0") && has(line, "o5o=0"), "block without transitions: 0");

	/* A long line (every total at 20 digits, the alarm list cut short)
	 * still fits; one that would not is refused whole rather than cut. */
	{
		static char big_diag[16384], big_status[2048], big_ctr[512], big_stat[512], big_mem[128];
		struct hist_in in;
		char small[64];
		const char *z20 = "18446744073709551615";

		snprintf(big_diag, sizeof big_diag,
			 "RTK.0> pon get transceiver rx-power\nRx Power: -999999.999 dBm\n"
			 "RTK.0> pon get transceiver tx-power\nTx Power: -999999.999 dBm\n"
			 "RTK.0> pon get transceiver temperature\nTemperature: -999999.999 C\n"
			 "RTK.0> pon get transceiver bias-current\nBias Current: 999999.999 mA\n"
			 "RTK.0> gpon get onu-state\nONU state: Operation State(O5)\n"
			 "RTK.0> gpon get alarm-status\n"
			 "Alarm LOS, status: occur\nAlarm LOF, status: occur\nAlarm LOM, status: occur\n"
			 "Alarm SF, status: occur\nAlarm SD, status: occur\nAlarm TF, status: occur\n"
			 "Alarm SUF, status: occur\nAlarm MEM, status: occur\nAlarm DACT, status: occur\n"
			 "Alarm DIS, status: occur\nAlarm MIS, status: occur\nAlarm PEE, status: occur\n"
			 "Alarm RDI, status: occur\nAlarm LCDG, status: occur\nAlarm XXXXXXXXXX, status: occur\n"
			 "RTK.0> mib dump counter port all\nPort: 2\n"
			 "ifInOctets : %s\nifOutOctets : %s\n", z20, z20);
		snprintf(big_status, sizeof big_status,
			 "state 5 (O5)\nalarm_events los %s\nalarm_events lof %s\nalarm_events lom %s\n"
			 "onu_state_ms O5 1\nonu_transitions O4 O5 %s\nonu_transitions O5 O1 %s\n",
			 z20, z20, z20, "4294967295", "4294967295");
		snprintf(big_ctr, sizeof big_ctr, "ctr ds_bip_err_bits %s\nctr ds_fec_uncorrectable_cw %s\n", z20, z20);
		snprintf(big_stat, sizeof big_stat, "cpu  %s 0 0 %s 0\n", "9000000000000000000", "9000000000000000000");
		snprintf(big_mem, sizeof big_mem, "MemAvailable: %s kB\n", z20);
		in.diag = big_diag;
		in.gpon_status = big_status;
		in.gpon_counters = big_ctr;
		in.stat = big_stat;
		in.meminfo = big_mem;
		in.uptime = "18446744073709551615.00 1\n";
		in.wall = 0x7fffffffL;
		n = hist_build_line(&in, line, sizeof line);
		printf("        (%lu bytes) %s\n", n, line);
		ck(n > 400 && n < HIST_LINE && n == strlen(line), "the longest line fits HIST_LINE");
		n = hist_build_line(&in, small, sizeof small);
		ck(n == 0 && small[0] == 0, "a line too long for the buffer is refused, not cut");
	}

	printf("history: %s\n", fails ? "FAILED" : "all ok");
	return fails != 0;
}
