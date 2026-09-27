/*
 * Host-native test for src/wrap.h -- the octet-counter wraparound extension
 * and the digit parsing/formatting it depends on.
 *
 * Deliberately built with the HOST compiler, not the MIPS cross toolchain:
 * wrap.h has no syscalls and no asm, so it compiles unchanged with plain gcc,
 * and this needs neither Docker nor qemu-user to run. `make test` builds and
 * runs it.
 */
#include <stdio.h>
#include <string.h>
#include "../src/wrap.h"

static int failures = 0;

#define CHECK(cond, fmt, ...) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: " fmt "\n", \
				__FILE__, __LINE__, __VA_ARGS__); \
			failures++; \
		} \
	} while (0)

static unsigned long long roundtrip(unsigned long long v)
{
	char buf[20];
	unsigned long n = format_u64(buf, v);

	return parse_u64(buf, n);
}

static void test_format_parse_roundtrip(void)
{
	unsigned long long values[] = {
		0, 1, 9, 10, 999, 4294967295ULL, 4294967296ULL,
		4881693552ULL, 18446744073709551615ULL
	};
	unsigned long i;

	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++)
		CHECK(roundtrip(values[i]) == values[i],
		      "roundtrip(%llu) != itself", values[i]);

	/* format_u64 must not print leading zeros or a sign. */
	{
		char buf[20];
		unsigned long n = format_u64(buf, 42);

		CHECK(n == 2 && buf[0] == '4' && buf[1] == '2',
		      "format_u64(42) -> %.*s (n=%lu)", (int)n, buf, n);
	}
}

/* A device with the fixed 64-bit driver: the raw reading only ever grows, so
 * extend_octets() must track it exactly with no distortion. */
static void test_no_wrap_passthrough(void)
{
	struct octet_state st;
	unsigned long long raws[] = { 0, 100, 4294967296ULL, 9000000000ULL };
	unsigned long i;

	memset(&st, 0, sizeof(st));
	for (i = 0; i < sizeof(raws) / sizeof(raws[0]); i++) {
		unsigned long long got = extend_octets(&st, raws[i]);

		CHECK(got == raws[i],
		      "extend_octets no-wrap step %lu: got %llu want %llu",
		      i, got, raws[i]);
	}
}

/*
 * A device stuck on a plain 32-bit register: the fixture sequence a busy
 * port produces, crossing 2^32 between two scrapes.
 */
static void test_single_wrap(void)
{
	struct octet_state st;
	unsigned long long expect;

	memset(&st, 0, sizeof(st));

	CHECK(extend_octets(&st, 4294967200ULL) == 4294967200ULL,
	      "baseline scrape: got %llu", st.extended);

	/* Register wrapped: 4294967200 -> ... -> 2^32 -> 100. */
	expect = 4294967200ULL + ((100ULL + (1ULL << 32)) - 4294967200ULL);
	CHECK(extend_octets(&st, 100ULL) == expect,
	      "post-wrap scrape: got %llu want %llu", st.extended, expect);
	CHECK(st.extended == 4294967296ULL + 100ULL,
	      "post-wrap total should be one full 32-bit span past raw: got %llu",
	      st.extended);

	/* And it keeps counting normally afterwards. */
	CHECK(extend_octets(&st, 5000ULL) == 4294967296ULL + 5000ULL,
	      "scrape after the wrap: got %llu", st.extended);
}

/* Two ports must not share state. */
static void test_per_port_independence(void)
{
	struct octet_state st[MIB_MAX_PORTS];
	unsigned long i;

	for (i = 0; i < MIB_MAX_PORTS; i++)
		memset(&st[i], 0, sizeof(st[i]));

	extend_octets(&st[0], 4294967290ULL);
	extend_octets(&st[2], 10ULL);

	CHECK(extend_octets(&st[0], 10ULL) == 4294967296ULL + 10ULL,
	      "port 0 should have wrapped independently: got %llu",
	      st[0].extended);
	CHECK(extend_octets(&st[2], 20ULL) == 20ULL,
	      "port 2 should be a plain small counter still: got %llu",
	      st[2].extended);
}

static void test_port_index(void)
{
	CHECK(port_index("2", 1) == 2, "port_index(\"2\") = %d", port_index("2", 1));
	CHECK(port_index("0", 1) == 0, "port_index(\"0\") = %d", port_index("0", 1));
	CHECK(port_index("99", 2) == -1, "port_index(\"99\") should be refused, got %d",
	      port_index("99", 2));
	CHECK(port_index("", 0) == -1, "port_index(\"\") should be refused, got %d",
	      port_index("", 0));
}

int main(void)
{
	test_format_parse_roundtrip();
	test_no_wrap_passthrough();
	test_single_wrap();
	test_per_port_independence();
	test_port_index();

	if (failures) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	printf("test_wrap: all tests passed\n");
	return 0;
}
