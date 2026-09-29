/*
 * The config-store identity: which files it is made of, the stat fields that
 * decide whether a cached hash is still good, and the parse of what
 * `md5sum` prints.
 *
 * Nothing in here makes a syscall, so it compiles with the host cc and is
 * unit-tested by test/test_confighash.c (`make test`). The stat and the fork
 * live in metrics_body.h.
 *
 * Why these three files: /var/config is the jffs2 config partition, 240 KB,
 * shared by both firmware slots and never touched by a reflash. lastgood.xml
 * holds the service settings (VLAN, LOID, PLOAM password), lastgood_hs.xml
 * the hardware identity (GPON serial, MAC), and odi.conf the odi-oss only
 * keys. Losing or silently changing any of them is an outage. The hash is
 * what an alert compares; the contents, secrets included, never leave the
 * stick.
 */

#ifndef ODI_CONFIGHASH_H
#define ODI_CONFIGHASH_H

#define CONFIG_DIR "/var/config/"
#define CONFIG_NFILES 3

static const char *const config_names[CONFIG_NFILES] = {
	"lastgood.xml", "lastgood_hs.xml", "odi.conf"
};

/* A prefix of the md5, not all 32 digits: enough to tell two versions of one
 * file apart, short enough to read on a dashboard. `md5sum FILE | cut -c1-12`
 * on the stick gives the same string. */
#define CONFIG_HASH_LEN 12

/*
 * What decides that a file may have changed. mtime alone is not enough: jffs2
 * keeps whole seconds, so two writes inside one second look identical by
 * mtime. A write through temp file and rename (odi.conf, and flash) also
 * changes the inode, and any write moves ctime and usually the size, so a
 * change has to dodge all four to be missed. A missed change is caught by the
 * next write that does not.
 */
struct config_stamp {
	int present;
	unsigned long long ino;
	long long size;
	long mtime;
	unsigned long mtime_nsec;
	long ctime;
	unsigned long ctime_nsec;
};

static int config_stamp_eq(const struct config_stamp *a,
			   const struct config_stamp *b)
{
	if (a->present != b->present)
		return 0;
	if (!a->present)
		return 1;
	return a->ino == b->ino && a->size == b->size &&
	       a->mtime == b->mtime && a->mtime_nsec == b->mtime_nsec &&
	       a->ctime == b->ctime && a->ctime_nsec == b->ctime_nsec;
}

/* Field by field: a struct assignment compiles to a memcpy call, and there is
 * no libc here to provide one. */
static void config_stamp_copy(struct config_stamp *dst,
			      const struct config_stamp *src)
{
	dst->present = src->present;
	dst->ino = src->ino;
	dst->size = src->size;
	dst->mtime = src->mtime;
	dst->mtime_nsec = src->mtime_nsec;
	dst->ctime = src->ctime;
	dst->ctime_nsec = src->ctime_nsec;
}

static int config_is_hex(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/* Whether the `len` bytes at `s` end with "/" + name, or are exactly name. */
static int config_path_is(const char *s, unsigned long len, const char *name)
{
	unsigned long n = 0, k;

	while (name[n])
		n++;
	if (len < n)
		return 0;
	for (k = 0; k < n; k++)
		if (s[len - n + k] != name[k])
			return 0;
	return len == n || s[len - n - 1] == '/';
}

/*
 * Parse `md5sum PATH...` output: one "<32 hex>  <path>" line per file it
 * could read. Anything else -- an error for a file that vanished between the
 * stat and the fork, which goes to the same pipe -- is skipped. For every
 * config file named on a line, the first CONFIG_HASH_LEN digits go into
 * out[i] and bit i is set in the returned mask.
 */
static unsigned config_parse_md5sum(const char *buf,
				    char out[CONFIG_NFILES][CONFIG_HASH_LEN + 1])
{
	unsigned long i = 0;
	unsigned mask = 0;

	while (buf[i]) {
		unsigned long ls = i, le = i, k, ps;
		int f;

		while (buf[le] && buf[le] != '\n')
			le++;

		for (k = 0; k < 32; k++)
			if (ls + k >= le || !config_is_hex(buf[ls + k]))
				goto next;
		/* Two spaces in text mode, space and '*' in binary mode. */
		if (ls + 34 > le || buf[ls + 32] != ' ' ||
		    (buf[ls + 33] != ' ' && buf[ls + 33] != '*'))
			goto next;
		ps = ls + 34;

		for (f = 0; f < CONFIG_NFILES; f++) {
			if (!config_path_is(buf + ps, le - ps, config_names[f]))
				continue;
			for (k = 0; k < CONFIG_HASH_LEN; k++)
				out[f][k] = buf[ls + k];
			out[f][CONFIG_HASH_LEN] = 0;
			mask |= 1u << f;
			break;
		}
next:
		i = buf[le] ? le + 1 : le;
	}
	return mask;
}

#endif /* ODI_CONFIGHASH_H */
