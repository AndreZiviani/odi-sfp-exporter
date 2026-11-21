/* Host-native test for src/meminfo.h: gpon_memory_bytes from /proc/meminfo,
 * with and without the MemAvailable line. `make test` runs it. */
#include <stdio.h>
#include <string.h>
#include "../src/meminfo.h"

static int fails;

static void ck(int cond, const char *what)
{
	printf("%s    %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond)
		fails++;
}

static char out[1024];

int main(void)
{
	/* The head of /proc/meminfo as a 6.18 kernel prints it. */
	static const char odi[] =
		"MemTotal:          27528 kB\n"
		"MemFree:            1384 kB\n"
		"MemAvailable:      13312 kB\n"
		"Buffers:            2036 kB\n"
		"Cached:             9364 kB\n"
		"SwapCached:            0 kB\n"
		"Active:             8000 kB\n";
	/* 2.6.30, the stock firmware: no MemAvailable. */
	static const char stock[] =
		"MemTotal:          27528 kB\n"
		"MemFree:            1384 kB\n"
		"Buffers:            2036 kB\n"
		"Cached:             9364 kB\n"
		"SwapCached:            0 kB\n";
	unsigned long n, kb = 0;

	n = meminfo_render(odi, out, sizeof out);
	ck(n == strlen(out), "the length returned is the text written");
	ck(strstr(out, "# TYPE gpon_memory_bytes gauge\n") != NULL, "one header");
	ck(strstr(out, "gpon_memory_bytes{kind=\"available\"} 13631488\n") != NULL,
	   "MemAvailable in bytes (13312 kB)");
	ck(strstr(out, "gpon_memory_bytes{kind=\"total\"} 28188672\n") != NULL, "MemTotal in bytes");
	ck(strstr(out, "gpon_memory_bytes{kind=\"free\"} 1417216\n") != NULL, "MemFree in bytes");
	ck(strstr(out, "gpon_memory_bytes{kind=\"buffers\"} 2084864\n") != NULL, "Buffers in bytes");
	ck(strstr(out, "gpon_memory_bytes{kind=\"cached\"} 9588736\n") != NULL, "Cached in bytes");
	ck(strstr(out, "SwapCached") == NULL && strstr(out, "kind=\"cached\"} 0") == NULL,
	   "Cached is not read from SwapCached");

	meminfo_render(stock, out, sizeof out);
	ck(strstr(out, "available") == NULL, "no MemAvailable line, no available sample");
	ck(strstr(out, "gpon_memory_bytes{kind=\"free\"} 1417216\n") != NULL, "the rest still there");

	ck(meminfo_render("Cached: x kB\n", out, sizeof out) == 0 && out[0] == 0,
	   "a line without digits gives nothing, not 0");
	ck(meminfo_render("", out, sizeof out) == 0, "an empty file gives nothing");
	ck(meminfo_kb("XMemFree: 5 kB\nMemFree: 7 kB\n", "MemFree:", &kb) && kb == 7,
	   "a key is matched only at the start of a line");

	if (fails)
		printf("test_meminfo: %d failed\n", fails);
	else
		printf("test_meminfo: all checks passed\n");
	return fails ? 1 : 0;
}
