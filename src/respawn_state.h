/*
 * The restart state of the respawned daemons, from the files odi-oss keeps
 * for them (odi-oss rootfs/skeleton/etc/scripts/respawn.sh and docs/SETTINGS.md,
 * "Restart budget"): one file per daemon, /var/run/respawn/<name>, KEY=value
 * lines written at every start of that daemon by a rename:
 *
 *     state=ok|backoff|degraded   as of the latest start
 *     starts=N                    starts since boot, the first included
 *     fast=N                      consecutive short runs before this start
 *     last_run_s=N                how long the previous run lived (absent on the first)
 *     delay_s=N                   the wait before this start
 *     cap_s=N                     the longest wait this daemon gets
 *     stable_s=N                  a run this long counts as healthy
 *     last_start_s=N              uptime of this start, in seconds
 *
 * rendered, one series per daemon, as
 *
 *     gpon_service_restarts_total{service="omcid"} 3     starts - 1
 *     gpon_service_backoff{service="omcid"} 0
 *     gpon_service_degraded{service="omcid"} 0
 *     gpon_service_backoff_seconds{service="omcid"} 0
 *
 * The state is written at a start and not rewritten when the daemon stops
 * dying, so a file that says backoff or degraded keeps saying it for as long
 * as the daemon runs. respawn.sh tells readers what to do: the daemon is
 * healthy again once uptime - last_start_s >= stable_s. The three backoff
 * series apply that rule, so they fall to 0 by themselves; they need the
 * uptime, state, last_start_s and stable_s, and are omitted when any of them
 * is missing or malformed. gpon_service_restarts_total needs only starts.
 * starts counts the first start of the boot, so the restart count is one
 * less; it begins again from 0 with the boot, like any counter.
 *
 * A malformed line is skipped, and a value that is not a plain decimal is
 * taken as absent. A daemon whose name has a character outside [0-9A-Za-z_]
 * (the dot-prefixed temporary files of the rename included) is skipped
 * whole, so the name is always safe as a label value. No directory, no
 * files, nothing emitted: the stock firmware has none.
 *
 * Pure: no syscalls, so it is tested on the host (test/test_respawn_state.c).
 */
#ifndef ODI_RESPAWN_STATE_H
#define ODI_RESPAWN_STATE_H

#define RESPAWN_DIR_PATH  "/var/run/respawn"
#define RESPAWN_NAME_MAX  32   /* longest name kept, with the NUL */
#define RESPAWN_SVC_MAX   32   /* most daemons reported */
#define RESPAWN_NUM_MAX   9    /* most digits of a value: fits a long */

enum { RESPAWN_STATE_NONE = -1, RESPAWN_OK = 0, RESPAWN_BACKOFF = 1, RESPAWN_DEGRADED = 2 };

struct respawn_svc {
	char name[RESPAWN_NAME_MAX];
	long starts;      /* -1 when absent or malformed */
	int state;        /* RESPAWN_STATE_NONE when absent or not one of the three */
	long delay_s;     /* -1 when absent */
	long stable_s;    /* -1 when absent */
	long last_start;  /* -1 when absent */
};

/* A name that is safe as a label value: [0-9A-Za-z_], 1 to NAME_MAX-1 long. */
static int respawn_name_ok(const char *s)
{
	unsigned long n = 0;

	for (; s[n]; n++) {
		char c = s[n];

		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
		      (c >= 'a' && c <= 'z') || c == '_') || n + 1 >= RESPAWN_NAME_MAX)
			return 0;
	}
	return n > 0;
}

/* The decimal at v up to the end of its line, or -1: a plain run of 1 to
 * NUM_MAX digits and nothing else. */
static long respawn_num(const char *v)
{
	unsigned long n = 0;
	long r = 0;

	while (v[n] >= '0' && v[n] <= '9') {
		if (n >= RESPAWN_NUM_MAX)
			return -1;
		r = r * 10 + (v[n] - '0');
		n++;
	}
	if (n == 0 || (v[n] != '\n' && v[n] != 0))
		return -1;
	return r;
}

/* Offset past `key=` when the line at buf[i] starts with it, else 0. */
static unsigned long respawn_key(const char *buf, unsigned long i, const char *key)
{
	unsigned long k = 0;

	while (key[k] && buf[i + k] == key[k])
		k++;
	return (!key[k] && buf[i + k] == '=') ? i + k + 1 : 0;
}

/* 1 when the value at v is exactly word, up to the end of its line. */
static int respawn_is(const char *v, const char *word)
{
	unsigned long k = 0;

	while (word[k] && v[k] == word[k])
		k++;
	return !word[k] && (v[k] == '\n' || v[k] == 0);
}

/* Fill s from the text of one state file. The first well-formed occurrence of
 * a key counts. Returns 1 when the name is safe, else 0. */
static int respawn_parse(const char *name, const char *buf, struct respawn_svc *s)
{
	unsigned long i = 0, k;

	if (!respawn_name_ok(name))
		return 0;
	for (k = 0; name[k]; k++)
		s->name[k] = name[k];
	s->name[k] = 0;
	s->starts = s->delay_s = s->stable_s = s->last_start = -1;
	s->state = RESPAWN_STATE_NONE;
	while (buf[i]) {
		unsigned long e;

		if ((e = respawn_key(buf, i, "starts"))) {
			if (s->starts < 0)
				s->starts = respawn_num(buf + e);
		} else if ((e = respawn_key(buf, i, "delay_s"))) {
			if (s->delay_s < 0)
				s->delay_s = respawn_num(buf + e);
		} else if ((e = respawn_key(buf, i, "stable_s"))) {
			if (s->stable_s < 0)
				s->stable_s = respawn_num(buf + e);
		} else if ((e = respawn_key(buf, i, "last_start_s"))) {
			if (s->last_start < 0)
				s->last_start = respawn_num(buf + e);
		} else if ((e = respawn_key(buf, i, "state")) && s->state == RESPAWN_STATE_NONE) {
			if (respawn_is(buf + e, "ok"))
				s->state = RESPAWN_OK;
			else if (respawn_is(buf + e, "backoff"))
				s->state = RESPAWN_BACKOFF;
			else if (respawn_is(buf + e, "degraded"))
				s->state = RESPAWN_DEGRADED;
		}
		while (buf[i] && buf[i] != '\n')
			i++;
		if (buf[i])
			i++;
	}
	return 1;
}

/* Whole seconds of the first token of /proc/uptime, or -1. */
static long respawn_uptime(const char *buf)
{
	unsigned long n = 0;
	long r = 0;

	while (buf[n] >= '0' && buf[n] <= '9') {
		if (n >= RESPAWN_NUM_MAX)
			return -1;
		r = r * 10 + (buf[n] - '0');
		n++;
	}
	if (n == 0 || (buf[n] != '.' && buf[n] != ' ' && buf[n] != '\n' && buf[n] != 0))
		return -1;
	return r;
}

/* 1 when the daemon still counts as inside its backoff (the state is not ok
 * and its latest start is younger than stable_s), 0 when it ran long enough
 * or the state is ok, -1 when that cannot be told. */
static int respawn_backoff_now(const struct respawn_svc *s, long uptime)
{
	if (uptime < 0 || s->state == RESPAWN_STATE_NONE || s->stable_s < 0 ||
	    s->last_start < 0 || s->last_start > uptime)
		return -1;
	if (s->state == RESPAWN_OK)
		return 0;
	return uptime - s->last_start < s->stable_s;
}

struct respawn_out {
	char *p;
	unsigned long n, cap;
};

static void respawn_put(struct respawn_out *o, const char *s)
{
	while (*s && o->n + 1 < o->cap)
		o->p[o->n++] = *s++;
	o->p[o->n] = 0;
}

static void respawn_put_num(struct respawn_out *o, long v)
{
	char tmp[12];
	int i = sizeof(tmp) - 1;

	tmp[i] = 0;
	do {
		tmp[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v > 0 && i > 0);
	respawn_put(o, tmp + i);
}

/* kind: 0 restarts, 1 backoff, 2 degraded, 3 backoff seconds. */
static void respawn_family(struct respawn_out *o, const struct respawn_svc *sv,
			   unsigned long n, long uptime, int kind)
{
	static const char *const name[4] = { "gpon_service_restarts_total", "gpon_service_backoff",
					     "gpon_service_degraded", "gpon_service_backoff_seconds" };
	static const char *const help[4] = {
		"Restarts of the daemon since boot (its starts minus the first), from /var/run/respawn/<service>.",
		"1 while the daemon is inside its restart backoff: its latest start followed a short run and "
		"has not yet lived stable_s seconds. Absent when that cannot be told.",
		"1 while the daemon is in the degraded state of its restart budget (five or more short runs in "
		"a row) and its latest start has not yet lived stable_s seconds. Absent when that cannot be told.",
		"The wait before the latest start of the daemon, in seconds, while it is inside its backoff; "
		"0 once it has run stable_s seconds. Absent when that cannot be told.",
	};
	unsigned long i, rows = 0;

	for (i = 0; i < n; i++) {
		const struct respawn_svc *s = &sv[i];
		int b = respawn_backoff_now(s, uptime);
		long v;

		if (kind == 0) {
			if (s->starts < 1)
				continue;
			v = s->starts - 1;
		} else {
			if (b < 0)
				continue;
			if (kind == 1)
				v = b;
			else if (kind == 2)
				v = b && s->state == RESPAWN_DEGRADED;
			else if (s->delay_s < 0)
				continue;
			else
				v = b ? s->delay_s : 0;
		}
		if (!rows++) {
			respawn_put(o, "# HELP ");
			respawn_put(o, name[kind]);
			respawn_put(o, " ");
			respawn_put(o, help[kind]);
			respawn_put(o, "\n# TYPE ");
			respawn_put(o, name[kind]);
			respawn_put(o, kind == 0 ? " counter\n" : " gauge\n");
		}
		respawn_put(o, name[kind]);
		respawn_put(o, "{service=\"");
		respawn_put(o, s->name);
		respawn_put(o, "\"} ");
		respawn_put_num(o, v);
		respawn_put(o, "\n");
	}
}

/* A field-wise copy: a struct assignment becomes a memcpy call, and this
 * binary links no libc. */
static void respawn_copy(struct respawn_svc *d, const struct respawn_svc *s)
{
	unsigned long k;

	for (k = 0; k < RESPAWN_NAME_MAX; k++)
		d->name[k] = s->name[k];
	d->starts = s->starts;
	d->state = s->state;
	d->delay_s = s->delay_s;
	d->stable_s = s->stable_s;
	d->last_start = s->last_start;
}

/* Insert s into sv[0..*n) in name order, so a scrape is stable whatever order
 * the directory lists. Dropped when the table is full. */
static void respawn_add(struct respawn_svc *sv, unsigned long *n, const struct respawn_svc *s)
{
	unsigned long i, k;

	if (*n >= RESPAWN_SVC_MAX)
		return;
	for (i = 0; i < *n; i++) {
		k = 0;
		while (sv[i].name[k] && sv[i].name[k] == s->name[k])
			k++;
		if ((unsigned char)s->name[k] < (unsigned char)sv[i].name[k])
			break;
	}
	for (k = *n; k > i; k--)
		respawn_copy(&sv[k], &sv[k - 1]);
	respawn_copy(&sv[i], s);
	(*n)++;
}

/* Render the families for sv[0..n) at the given uptime (whole seconds, -1 if
 * unknown) into out (NUL-terminated, truncated at cap). Returns the length. */
static unsigned long respawn_render(const struct respawn_svc *sv, unsigned long n, long uptime,
				    char *out, unsigned long cap)
{
	struct respawn_out o = { out, 0, cap };
	int kind;

	if (cap == 0)
		return 0;
	out[0] = 0;
	for (kind = 0; kind < 4; kind++)
		respawn_family(&o, sv, n, uptime, kind);
	return o.n;
}

#endif /* ODI_RESPAWN_STATE_H */
