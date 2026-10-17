/*
 * Host-native test for src/transceiver.h: the module alarm and warning flags
 * and the I2C readability series, rendered from the text of `diag pon get
 * transceiver alarm-status` (odi-oss src/diag/src/commands.c, the lines are
 * written by cmd_transceiver_alarms). Checks the three outcomes: lines
 * recognised, read failed, nothing recognised (stock diag).
 *
 * Run by `make test`; exit 0 on pass.
 */
#include <stdio.h>
#include <string.h>

#include "../src/transceiver.h"

static char out[8192];
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

static const char *render(const char *sec)
{
	outn = 0;
	out[0] = 0;
	xcvr_emit(&o, sec);
	return out;
}

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		failures++;
}

static int has(const char *s) { return strstr(out, s) != NULL; }

static int count(const char *s)
{
	int n = 0;
	const char *p = out;

	while ((p = strstr(p, s))) {
		n++;
		p += strlen(s);
	}
	return n;
}

/* The ten alarm names, the ten warning names, then the LOS line, as diag
 * prints them. */
#define CLEAR_ALARMS \
	"Alarm Temperature high, status: clear\n" \
	"Alarm Temperature low, status: clear\n" \
	"Alarm Voltage high, status: clear\n" \
	"Alarm Voltage low, status: clear\n" \
	"Alarm Bias Current high, status: clear\n" \
	"Alarm Bias Current low, status: clear\n" \
	"Alarm Tx Power high, status: clear\n" \
	"Alarm Tx Power low, status: clear\n" \
	"Alarm Rx Power high, status: clear\n" \
	"Alarm Rx Power low, status: clear\n"
#define CLEAR_WARNINGS \
	"Warning Temperature high, status: clear\n" \
	"Warning Temperature low, status: clear\n" \
	"Warning Voltage high, status: clear\n" \
	"Warning Voltage low, status: clear\n" \
	"Warning Bias Current high, status: clear\n" \
	"Warning Bias Current low, status: clear\n" \
	"Warning Tx Power high, status: clear\n" \
	"Warning Tx Power low, status: clear\n" \
	"Warning Rx Power high, status: clear\n" \
	"Warning Rx Power low, status: clear\n"

int main(void)
{
	/* provision.h's emitters are used by other tests, not this one. */
	(void)prov_emit_tconts;
	(void)prov_emit_omci;

	render(CLEAR_ALARMS CLEAR_WARNINGS "Optical LOS, status: clear\n");
	ck(has("gpon_transceiver_up 1\n"), "all clear: the module answered");
	ck(count("gpon_transceiver_alarm{") == 20, "twenty flag series");
	ck(has("gpon_transceiver_alarm{kind=\"alarm\",condition=\"rx_power_low\"} 0\n") &&
	   has("gpon_transceiver_alarm{kind=\"warning\",condition=\"bias_current_high\"} 0\n"),
	   "names lowercased with underscores, kind from the line");
	ck(has("gpon_transceiver_rx_los 0\n"), "optical LOS clear");
	ck(count("# TYPE gpon_transceiver_alarm gauge\n") == 1 &&
	   count("# TYPE gpon_transceiver_up gauge\n") == 1 &&
	   count("# TYPE gpon_transceiver_rx_los gauge\n") == 1,
	   "every family announced once, as a gauge");

	render("Alarm Temperature high, status: clear\n"
	       "Alarm Rx Power low, status: occur\n"
	       "Warning Rx Power low, status: occur\n"
	       "Optical LOS, status: occur\n");
	ck(has("condition=\"rx_power_low\"} 1\n") && has("kind=\"warning\",condition=\"rx_power_low\"} 1\n"),
	   "occur is 1, for alarm and for warning");
	ck(has("gpon_transceiver_rx_los 1\n") && has("gpon_transceiver_up 1\n"), "optical LOS occur");

	render("Alarm Rx Power low, status: weird\n");
	ck(has("condition=\"rx_power_low\"} 1\n"), "anything but clear is asserted, as gpon_alarm does");

	render("Alarm Rx Power low, status: clear\r\nOptical LOS, status: clear\r\n");
	ck(has("condition=\"rx_power_low\"} 0\n") && has("gpon_transceiver_rx_los 0\n"),
	   "CRLF line ends");

	render("% transceiver alarm status read failed\n");
	ck(has("gpon_transceiver_up 0\n") && !has("gpon_transceiver_alarm") &&
	   !has("gpon_transceiver_rx_los"),
	   "read failed: up 0 and no flag series, never a clear 0");

	render("");
	ck(outn == 0, "an empty section (diag printed nothing) emits nothing");
	render("% Command not found\n");
	ck(outn == 0, "a diag that does not know the command emits nothing");

	render("Alarm Bad\"Name, status: occur\nAlarm Ok Name, status: clear\n");
	ck(!has("Bad") && has("condition=\"ok_name\"} 0\n"), "a name with a quote is skipped, the next line is not");
	render("Alarm Name Without Comma status: occur\n");
	ck(outn == 0, "a line with no comma is skipped");
	render("Alarm AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA, status: occur\n");
	ck(outn == 0, "a name longer than 31 characters is skipped");
	render("Optical Power, status: occur\n");
	ck(outn == 0, "only Optical LOS is the LOS series");

	if (failures) {
		fprintf(stderr, "test_transceiver: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_transceiver: all checks passed\n");
	return 0;
}
