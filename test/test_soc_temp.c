/* Host-native test for src/soc_temp.h: the hwmon name match and the die
 * temperature rendered from temp1_input. `make test` runs it. */
#include <stdio.h>
#include <string.h>
#include "../src/soc_temp.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

static char out[1024];

static int renders(const char *in, const char *value_line)
{
	unsigned long n = soc_temp_render(in, out, sizeof out);

	return n && n == strlen(out) && strstr(out, value_line) != NULL;
}

int main(void)
{
	unsigned long n;

	ck(soc_temp_name_match("rtl9602c_soc\n"), "name with newline");
	ck(soc_temp_name_match("rtl9602c_soc"), "name without newline");
	ck(!soc_temp_name_match("rtl9602c_soc2\n"), "a longer name is not the sensor");
	ck(!soc_temp_name_match("rtl9602c_so\n"), "a prefix is not the sensor");
	ck(!soc_temp_name_match("rtl9602c_soc\nx"), "text after the newline is not the sensor");
	ck(!soc_temp_name_match("acpitz\n"), "another sensor");
	ck(!soc_temp_name_match(""), "empty");

	ck(renders("62000\n", "\ngpon_soc_temperature_celsius 62.000\n"), "62 C");
	ck(renders("62000", "\ngpon_soc_temperature_celsius 62.000\n"), "no trailing newline");
	ck(renders("66500\n", "\ngpon_soc_temperature_celsius 66.500\n"), "66.5 C");
	ck(renders("7\n", "\ngpon_soc_temperature_celsius 0.007\n"), "7 millidegrees");
	ck(renders("0\n", "\ngpon_soc_temperature_celsius 0.000\n"), "zero");
	ck(renders("127000\n", "\ngpon_soc_temperature_celsius 127.000\n"), "127 C, the sensor ceiling");
	ck(renders("-12345\n", "\ngpon_soc_temperature_celsius -12.345\n"), "negative");
	ck(renders("-0\n", "\ngpon_soc_temperature_celsius 0.000\n"), "negative zero has no sign");
	ck(strstr(out, "# TYPE gpon_soc_temperature_celsius gauge\n") != NULL &&
	   strstr(out, "# HELP gpon_soc_temperature_celsius ") != NULL, "HELP and TYPE lines");

	ck(soc_temp_render("", out, sizeof out) == 0 && out[0] == 0, "empty file: nothing");
	ck(soc_temp_render("\n", out, sizeof out) == 0, "newline only: nothing");
	ck(soc_temp_render("abc\n", out, sizeof out) == 0, "text: nothing");
	ck(soc_temp_render("62000 \n", out, sizeof out) == 0, "trailing junk: nothing");
	ck(soc_temp_render("62.5\n", out, sizeof out) == 0, "a decimal point: nothing");
	ck(soc_temp_render("62000\n\n", out, sizeof out) == 0, "two lines: nothing");
	ck(soc_temp_render("12345678\n", out, sizeof out) == 0, "eight digits: nothing");
	ck(soc_temp_render("-\n", out, sizeof out) == 0, "a bare minus: nothing");
	ck(soc_temp_render("62000\n", out, 0) == 0, "zero capacity");

	/* Truncation stays inside the buffer and terminated. */
	n = soc_temp_render("62000\n", out, 20);
	ck(n == 19 && out[19] == 0 && strlen(out) == 19, "truncated at cap, terminated");

	/* Limits: temp1_max / temp1_crit. */
	n = soc_temp_render_limit("95000\n", 0, out, sizeof out);
	ck(n && n == strlen(out) && strstr(out, "\ngpon_soc_temperature_max_celsius 95.000\n") != NULL, "max 95 C");
	ck(strstr(out, "# TYPE gpon_soc_temperature_max_celsius gauge\n") != NULL &&
	   strstr(out, "unverified scale") != NULL, "max HELP and TYPE, unverified scale");
	n = soc_temp_render_limit("110000", 1, out, sizeof out);
	ck(n && strstr(out, "\ngpon_soc_temperature_crit_celsius 110.000\n") != NULL, "crit 110 C, no newline");
	ck(strstr(out, "# TYPE gpon_soc_temperature_crit_celsius gauge\n") != NULL &&
	   strstr(out, "unverified scale") != NULL, "crit HELP and TYPE, unverified scale");
	ck(soc_temp_render_limit(NULL, 0, out, sizeof out) == 0 && out[0] == 0, "max absent: nothing");
	ck(soc_temp_render_limit(NULL, 1, out, sizeof out) == 0, "crit absent: nothing");
	ck(soc_temp_render_limit("", 0, out, sizeof out) == 0, "max empty: nothing");
	ck(soc_temp_render_limit("hot\n", 0, out, sizeof out) == 0, "max garbage: nothing");
	ck(soc_temp_render_limit("95.5\n", 1, out, sizeof out) == 0, "crit decimal: nothing");
	ck(soc_temp_render_limit("95000 \n", 1, out, sizeof out) == 0, "crit trailing junk: nothing");
	ck(soc_temp_render_limit("95000\n", 0, out, 0) == 0, "limit zero capacity");

	/* Alarms: temp1_max_alarm / temp1_crit_alarm. */
	n = soc_temp_render_alarms("0\n", "0\n", out, sizeof out);
	ck(n && n == strlen(out) &&
	   strstr(out, "\ngpon_soc_temperature_alarm{level=\"max\"} 0\n") != NULL &&
	   strstr(out, "\ngpon_soc_temperature_alarm{level=\"crit\"} 0\n") != NULL, "both alarms 0");
	ck(strstr(out, "# TYPE gpon_soc_temperature_alarm gauge\n") != NULL &&
	   strstr(out, "unverified scale") != NULL, "alarm HELP and TYPE, unverified scale");
	soc_temp_render_alarms("1", "0", out, sizeof out);
	ck(strstr(out, "{level=\"max\"} 1\n") != NULL && strstr(out, "{level=\"crit\"} 0\n") != NULL, "max set, no newline");
	soc_temp_render_alarms("1\n", "1\n", out, sizeof out);
	ck(strstr(out, "{level=\"max\"} 1\n") != NULL && strstr(out, "{level=\"crit\"} 1\n") != NULL, "both set");
	n = soc_temp_render_alarms("1\n", NULL, out, sizeof out);
	ck(n && strstr(out, "{level=\"max\"} 1\n") != NULL && strstr(out, "level=\"crit\"") == NULL, "crit file absent: only max");
	n = soc_temp_render_alarms(NULL, "0\n", out, sizeof out);
	ck(n && strstr(out, "level=\"max\"") == NULL && strstr(out, "{level=\"crit\"} 0\n") != NULL, "max file absent: only crit");
	n = soc_temp_render_alarms("2\n", "1\n", out, sizeof out);
	ck(n && strstr(out, "level=\"max\"") == NULL && strstr(out, "{level=\"crit\"} 1\n") != NULL, "garbage value skipped, other kept");
	ck(soc_temp_render_alarms(NULL, NULL, out, sizeof out) == 0 && out[0] == 0, "both absent: nothing");
	ck(soc_temp_render_alarms("x\n", "\n", out, sizeof out) == 0, "both garbage: nothing");
	ck(soc_temp_render_alarms("01\n", "1 \n", out, sizeof out) == 0, "multi-char or trailing junk: nothing");
	ck(soc_temp_render_alarms("0\n\n", "", out, sizeof out) == 0, "two lines, empty: nothing");
	ck(soc_temp_render_alarms("0\n", "0\n", out, 0) == 0, "alarm zero capacity");
	n = soc_temp_render_alarms("0\n", "1\n", out, 40);
	ck(n == 39 && strlen(out) == 39, "alarm truncated at cap, terminated");

	return fails ? 1 : 0;
}
