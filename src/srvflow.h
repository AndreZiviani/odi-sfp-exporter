/*
 * gpon_omci_services from `omcicli dump srvflow`, and whether what omcicli
 * printed was an answer at all. Pure text in, a number out: no syscalls, so
 * the host compiler tests it (test/test_srvflow.c).
 *
 * An answer has at least one "SERVID n: Used: 0|1, (...)" row -- the vendor
 * omci_app prints 256 of them, most Used: 0 -- or the count line odi-oss
 * omcid ends its table with ("2 services", "1 service"), which is the whole
 * answer when nothing is installed ("0 services"). When the count line is
 * there, its number is the answer. Anything else is omcicli speaking for
 * itself, into the same pipe: "% no command queue at key 0x800 -- is
 * omci_app running?" when no daemon made the queue, "% send failed" once a
 * queue nobody reads is full, "% the daemon did not answer; ...". Taking any
 * output as an answer reported gpon_omci_up 1 and no services for each of
 * those, while omcid was gone. A row cut before its Used value is no answer
 * either. Nor is a text in which omcicli says the dump may be incomplete
 * (odi-oss omcicli: the daemon did not answer the barrier after the rows,
 * stopped answering mid-stream, sent a reply with an unexpected sequence
 * number (a lost, repeated or reordered chunk, or one not from omcid), or the
 * barrier could not be sent): rows
 * before such a line may not be all of them, and a count from them would
 * say "no services" for a service whose row did not come.
 *
 * The text is fed in as it arrives, in pieces of any size, and only the line
 * being read is kept: the vendor dump is about 18 KB, and the one row in use
 * can be the last (metricsd read the first 8 KB and missed it).
 */
#ifndef ODI_SRVFLOW_H
#define ODI_SRVFLOW_H

/* The part of a line kept: a row is about 70 bytes, and the fields read here
 * are in its first 20. The rest of a longer line is dropped. */
#define SRVFLOW_LINE 128

struct srvflow_scan {
	char line[SRVFLOW_LINE + 1];
	unsigned long len;	/* bytes kept of the line being read */
	int cut;		/* the line being read was longer than that */
	int answer;		/* a row or a count line was seen */
	int incomplete;		/* omcicli said the dump may be incomplete */
	long used;		/* rows that say Used: 1 */
	long count;		/* the number on the count line, or -1 */
};

static int srvflow_starts(const char *s, const char *p)
{
	while (*p)
		if (*s++ != *p++)
			return 0;
	return 1;
}

/* The line is "<digits> service" or "<digits> services", nothing more: its
 * number, or -1. Six digits at most, far above the 256 rows of the table. */
static long srvflow_count_line(const char *s)
{
	unsigned long i = 0;
	long v = 0;

	while (s[i] >= '0' && s[i] <= '9') {
		if (i == 6)
			return -1;
		v = v * 10 + (s[i] - '0');
		i++;
	}
	if (!i || !srvflow_starts(s + i, " service"))
		return -1;
	i += 8;
	if (s[i] == 's')
		i++;
	return s[i] ? -1 : v;
}

/* The Used value of a row, 0 or 1, or -1 when the row has none whole:
 * "Used: " then the digit, then a comma or the end of the line. */
static int srvflow_used_field(const char *l)
{
	for (; *l; l++)
		if (srvflow_starts(l, "Used: ")) {
			char v = l[6];

			if ((v != '0' && v != '1') || (l[7] && l[7] != ','))
				return -1;
			return v - '0';
		}
	return -1;
}

/* omcicli's own lines that say the rows may not be all of them. */
static int srvflow_incomplete_line(const char *l)
{
	return srvflow_starts(l, "% the daemon did not answer") ||
	       srvflow_starts(l, "% the daemon stopped answering") ||
	       srvflow_starts(l, "% reply out of order") ||
	       srvflow_starts(l, "% could not send the completion barrier");
}

static void srvflow_init(struct srvflow_scan *s)
{
	s->len = 0;
	s->cut = 0;
	s->answer = 0;
	s->incomplete = 0;
	s->used = 0;
	s->count = -1;
}

/* One whole line, without its newline. */
static void srvflow_line(struct srvflow_scan *s)
{
	char *l = s->line;
	long c;

	if (s->len && l[s->len - 1] == '\r')
		s->len--;
	l[s->len] = 0;
	if (srvflow_starts(l, "SERVID ")) {
		int v = srvflow_used_field(l);

		if (v >= 0) {
			s->answer = 1;
			s->used += v;
		}
	} else if (!s->cut && (c = srvflow_count_line(l)) >= 0) {
		s->answer = 1;
		s->count = c;
	} else if (srvflow_incomplete_line(l)) {
		s->incomplete = 1;
	}
	s->len = 0;
	s->cut = 0;
}

static void srvflow_feed(struct srvflow_scan *s, const char *p, unsigned long n)
{
	unsigned long i;

	for (i = 0; i < n; i++) {
		if (p[i] == '\n')
			srvflow_line(s);
		else if (s->len < SRVFLOW_LINE)
			s->line[s->len++] = p[i];
		else
			s->cut = 1;
	}
}

/* The services once the text has ended, or -1 when it held no answer, or
 * one that omcicli says may be incomplete. */
static long srvflow_end(struct srvflow_scan *s)
{
	if (s->len || s->cut)
		srvflow_line(s);
	if (!s->answer || s->incomplete)
		return -1;
	return s->count >= 0 ? s->count : s->used;
}

/* The same for a whole text at once (the tests; metricsd feeds). */
__attribute__((unused)) static long srvflow_used(const char *buf)
{
	struct srvflow_scan s;
	unsigned long n = 0;

	while (buf[n])
		n++;
	srvflow_init(&s);
	srvflow_feed(&s, buf, n);
	return srvflow_end(&s);
}

#endif /* ODI_SRVFLOW_H */
