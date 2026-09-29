/*
 * Host-native test for src/confighash.h -- the md5sum output parse and the
 * stat comparison that decides when the cached config hash is stale.
 *
 * Built with the HOST compiler, like test_wrap.c: confighash.h has no
 * syscalls and no asm. `make test` builds and runs it.
 */
#include <stdio.h>
#include <string.h>
#include "../src/confighash.h"

static int failures = 0;

#define CHECK(cond, fmt, ...) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d: " fmt "\n", \
				__FILE__, __LINE__, __VA_ARGS__); \
			failures++; \
		} \
	} while (0)

static void test_parse_all_three(void)
{
	char out[CONFIG_NFILES][CONFIG_HASH_LEN + 1];
	/* The shape busybox md5sum prints: digest, two spaces, the path as
	 * given on the command line. */
	const char *buf =
		"0123456789abcdef0123456789abcdef  /var/config/lastgood.xml\n"
		"fedcba9876543210fedcba9876543210  /var/config/lastgood_hs.xml\n"
		"00112233445566778899aabbccddeeff  /var/config/odi.conf\n";
	unsigned mask = config_parse_md5sum(buf, out);

	CHECK(mask == 7, "mask = %u, want 7", mask);
	CHECK(strcmp(out[0], "0123456789ab") == 0, "lastgood.xml -> %s", out[0]);
	CHECK(strcmp(out[1], "fedcba987654") == 0, "lastgood_hs.xml -> %s", out[1]);
	CHECK(strcmp(out[2], "001122334455") == 0, "odi.conf -> %s", out[2]);
}

/* lastgood.xml must not match lastgood_hs.xml or the other way round, and
 * the order md5sum prints in must not matter. */
static void test_parse_names_exact(void)
{
	char out[CONFIG_NFILES][CONFIG_HASH_LEN + 1];
	const char *buf =
		"fedcba9876543210fedcba9876543210  /var/config/lastgood_hs.xml\n"
		"0123456789abcdef0123456789abcdef  /var/config/lastgood.xml\n";
	unsigned mask = config_parse_md5sum(buf, out);

	CHECK(mask == 3, "mask = %u, want 3", mask);
	CHECK(strcmp(out[0], "0123456789ab") == 0, "lastgood.xml -> %s", out[0]);
	CHECK(strcmp(out[1], "fedcba987654") == 0, "lastgood_hs.xml -> %s", out[1]);

	mask = config_parse_md5sum(
		"0123456789abcdef0123456789abcdef  /var/config/xlastgood.xml\n"
		"0123456789abcdef0123456789abcdef  /var/config/lastgood.xml.new\n",
		out);
	CHECK(mask == 0, "a name that only contains a config name matched: %u", mask);
}

/* stderr shares the pipe: an error line for a file that vanished between the
 * stat and the fork, and a truncated last line, are skipped. */
static void test_parse_rejects_noise(void)
{
	char out[CONFIG_NFILES][CONFIG_HASH_LEN + 1];
	const char *buf =
		"md5sum: /var/config/odi.conf: No such file or directory\n"
		"0123456789abcdef0123456789abcdef  /var/config/lastgood.xml\n"
		"0123456789ABCDEF0123456789abcdef  /var/config/lastgood_hs.xml\n"
		"0123456789abcdef01234";
	unsigned mask = config_parse_md5sum(buf, out);

	CHECK(mask == 1, "mask = %u, want 1 (only lastgood.xml)", mask);
	CHECK(config_parse_md5sum("", out) == 0, "%s", "empty output parsed as a hash");
	/* No trailing newline on the last good line. */
	mask = config_parse_md5sum(
		"00112233445566778899aabbccddeeff  /var/config/odi.conf", out);
	CHECK(mask == 4, "mask = %u, want 4", mask);
}

static void test_stamp_eq(void)
{
	struct config_stamp a = { 1, 42, 1000, 1790000000, 0, 1790000000, 0 };
	struct config_stamp b = a;
	struct config_stamp none1 = { 0, 0, 0, 0, 0, 0, 0 };
	struct config_stamp none2 = { 0, 7, 7, 7, 7, 7, 7 };

	CHECK(config_stamp_eq(&a, &b), "%s", "identical stamps differ");
	b.ino = 43;		/* rename over it, same second, same size */
	CHECK(!config_stamp_eq(&a, &b), "%s", "a new inode was not a change");
	b = a;
	b.size = 1001;
	CHECK(!config_stamp_eq(&a, &b), "%s", "a new size was not a change");
	b = a;
	b.ctime++;
	CHECK(!config_stamp_eq(&a, &b), "%s", "a new ctime was not a change");
	b = a;
	b.present = 0;
	CHECK(!config_stamp_eq(&a, &b), "%s", "a vanished file was not a change");
	CHECK(config_stamp_eq(&none1, &none2), "%s", "two absent files differ");
	config_stamp_copy(&b, &a);
	CHECK(config_stamp_eq(&a, &b), "%s", "config_stamp_copy lost a field");
}

int main(void)
{
	test_parse_all_three();
	test_parse_names_exact();
	test_parse_rejects_noise();
	test_stamp_eq();

	if (failures) {
		fprintf(stderr, "%d test(s) failed\n", failures);
		return 1;
	}
	printf("test_confighash: all tests passed\n");
	return 0;
}
