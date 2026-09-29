/*
 * The parse of /proc/odi_ramlog_prev, split out from metrics_body.h so it can
 * be tested with a host compiler (test/test_resetinfo.c), the same way wrap.h
 * is: no syscalls, no libc, only byte scanning.
 *
 * odi-oss's kernel (odi_ramlog.c) writes that file on every boot of its own
 * image. Its first two lines are all this reads:
 *
 *     this boot: boot=31 slot=1
 *     previous boot: boot=30 slot=1 build=v1.0.9 crumb=TICK/15028 reason=wdt_client:omcid
 *
 * The boot counter counts boots of the image since the last power cycle. The
 * reason is why the previous boot ended, as the kernel recorded it at the
 * moment it knew: wdt_client:<client>, wdt_mem, wdt_userland, reboot, halt,
 * poweroff, panic, oops, power (DRAM lost its contents, a power cycle) or
 * unknown (nothing recorded one, e.g. a hang the hardware watchdog caught).
 * An older kernel prints no reason= at all; the reason is then absent rather
 * than guessed.
 */

#ifndef ODI_RESETINFO_H
#define ODI_RESETINFO_H

/* The longest a value is kept: the kernel caps a client name at 15, and every
 * reason name is shorter than that. */
#define RESETINFO_VALUE_MAX 24

struct resetinfo {
	char boot[12];                     /* this boot's counter, decimal; "" if absent */
	char reason[RESETINFO_VALUE_MAX];  /* "" if the line carries no reason= */
	char client[RESETINFO_VALUE_MAX];  /* the part after ':' of wdt_client:<name>, else "" */
};

/* Offset just past `key` if the line starting at i begins with it, else 0. */
static unsigned long ri_line_key(const char *buf, unsigned long i, const char *key)
{
	unsigned long k = 0;

	while (key[k] && buf[i + k] == key[k])
		k++;
	return key[k] ? 0 : i + k;
}

/* The same set the kernel sanitizes to, so it is always safe as a label. */
static int ri_value_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '?';
}

/* Copy the token at s (up to a space or end of line, or `stop` if non-zero)
 * into dst when it is made only of `ok` characters. Returns the number of
 * characters taken, or 0 -- dst then "" -- when the token is empty, too long,
 * carries any other character, or runs into the end of the buffer (a read cut
 * short mid-line; a value that may be truncated is not reported). */
static unsigned long ri_token(char *dst, unsigned long cap, const char *s,
			      int (*ok)(char), char stop)
{
	unsigned long n = 0, k;

	dst[0] = 0;
	while (s[n] && s[n] != ' ' && s[n] != '\n' && s[n] != stop) {
		if (!ok(s[n]) || n + 1 >= cap)
			return 0;
		n++;
	}
	if (n == 0 || s[n] == 0)
		return 0;
	for (k = 0; k < n; k++)
		dst[k] = s[k];
	dst[n] = 0;
	return n;
}

static int ri_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* Find `word` inside the line [i, end of line), preceded by a space. */
static const char *ri_find_in_line(const char *buf, unsigned long i, const char *word)
{
	for (; buf[i] && buf[i] != '\n'; i++) {
		unsigned long e;

		if (buf[i] != ' ')
			continue;
		e = ri_line_key(buf, i + 1, word);
		if (e)
			return buf + e;
	}
	return 0;
}

/* Fill `ri` from the text of /proc/odi_ramlog_prev (NUL terminated, only its
 * first lines needed). Returns 1 when at least the boot counter was found. */
static int resetinfo_parse(const char *buf, struct resetinfo *ri)
{
	unsigned long i = 0;

	ri->boot[0] = ri->reason[0] = ri->client[0] = 0;
	while (buf[i]) {
		unsigned long e;

		if ((e = ri_line_key(buf, i, "this boot:"))) {
			const char *v = ri_find_in_line(buf, e, "boot=");

			if (v)
				ri_token(ri->boot, sizeof(ri->boot), v, ri_digit, 0);
		} else if ((e = ri_line_key(buf, i, "previous boot:"))) {
			const char *v = ri_find_in_line(buf, e, "reason=");
			unsigned long n;

			if (v && (n = ri_token(ri->reason, sizeof(ri->reason), v,
					       ri_value_char, ':')) && v[n] == ':' &&
			    !ri_token(ri->client, sizeof(ri->client), v + n + 1,
				      ri_value_char, 0))
				ri->reason[0] = 0; /* a malformed client: no reason at all */
			break; /* the page text follows; nothing more to read */
		}
		while (buf[i] && buf[i] != '\n')
			i++;
		if (buf[i])
			i++;
	}
	return ri->boot[0] != 0;
}

#endif /* ODI_RESETINFO_H */
