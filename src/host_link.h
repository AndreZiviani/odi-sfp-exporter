/*
 * The host-port auto-negotiation and the safe mode, from the files odi-oss
 * keeps for them (odi-oss docs/TOOLS.md, "host-autoneg.sh" and
 * "safe-mode.sh"; docs/SETTINGS.md has what each state means):
 *
 *     /var/run/host-autoneg   this boot, KEY=value per line, replaced by a
 *                             rename: setting=on|off (HOST_AUTONEG),
 *                             running=on|off (what the port runs now),
 *                             phase=steady|watch|trial, fallback=0|1
 *     /var/run/safe-mode      present on a safe-mode boot only
 *     /etc/config/safe-mode   the safe-mode record, until acknowledged
 *     /sys/module/odi_switch/parameters/host_link   1 or 0, the live link
 *
 * rendered as
 *
 *     gpon_host_autoneg{state="running"} 1      1 negotiating, 0 forced
 *     gpon_host_autoneg{state="setting"} 0      the stored HOST_AUTONEG
 *     gpon_host_link_up 1
 *     gpon_host_autoneg_fallback 0             forced setting, AN running
 *     gpon_host_autoneg_trial 0                a keep-or-revert trial runs
 *     gpon_host_autoneg_watch 0                the boot watch is deciding
 *     gpon_safe_mode 0                         this boot is a safe-mode boot
 *     gpon_safe_mode_unacknowledged 0          a safe-mode record waits
 *
 * The state file is what says the image has these features: without it
 * (an older image, the stock firmware) nothing at all is emitted, the safe
 * mode included, rather than a 0 that would read as "checked, not in safe
 * mode". A value that is not one the file defines omits its series, and
 * gpon_host_link_up needs the kernel parameter.
 *
 * Pure: no syscalls, so it is tested on the host (test/test_host_link.c).
 */
#ifndef ODI_HOST_LINK_H
#define ODI_HOST_LINK_H

#define HOST_AUTONEG_STATE_PATH "/var/run/host-autoneg"
#define SAFE_MODE_FLAG_PATH     "/var/run/safe-mode"
#define SAFE_MODE_RECORD_PATH   "/etc/config/safe-mode"
#define HOST_LINK_PATH          "/sys/module/odi_switch/parameters/host_link"

/* The value of `key=` at the start of a line of buf, compared with want:
 * 1 when equal, 0 when another value, -1 when the key is absent. */
static int host_link_is(const char *buf, const char *key, const char *want)
{
	unsigned long i = 0;

	while (buf[i]) {
		unsigned long k = 0;

		while (key[k] && buf[i + k] == key[k])
			k++;
		if (!key[k] && buf[i + k] == '=') {
			const char *v = buf + i + k + 1;
			unsigned long w = 0;

			while (want[w] && v[w] == want[w])
				w++;
			return !want[w] && (v[w] == '\n' || v[w] == 0);
		}
		while (buf[i] && buf[i] != '\n')
			i++;
		if (buf[i])
			i++;
	}
	return -1;
}

/* 1, 0 or -1 for a key whose values are a pair, a (1) and b (0). */
static int host_link_pick(const char *buf, const char *key, const char *a, const char *b)
{
	if (host_link_is(buf, key, a) == 1)
		return 1;
	if (host_link_is(buf, key, b) == 1)
		return 0;
	return -1;
}

struct host_link_out {
	char *p;
	unsigned long n, cap;
};

static void host_link_put(struct host_link_out *o, const char *s)
{
	while (*s && o->n + 1 < o->cap)
		o->p[o->n++] = *s++;
	o->p[o->n] = 0;
}

static void host_link_gauge(struct host_link_out *o, const char *name, const char *help, int v)
{
	if (v < 0)
		return;
	host_link_put(o, "# HELP ");
	host_link_put(o, name);
	host_link_put(o, " ");
	host_link_put(o, help);
	host_link_put(o, "\n# TYPE ");
	host_link_put(o, name);
	host_link_put(o, " gauge\n");
	host_link_put(o, name);
	host_link_put(o, v ? " 1\n" : " 0\n");
}

/*
 * Render the metrics into out (NUL-terminated, truncated at cap). state is
 * the state file (NULL or empty: the image has none, nothing rendered);
 * safe_now and safe_record say whether those files exist; link is the
 * kernel parameter's contents, or NULL. Returns the length.
 */
static unsigned long host_link_render(const char *state, int safe_now, int safe_record,
				      const char *link, char *out, unsigned long cap)
{
	struct host_link_out o = { out, 0, cap };
	int running, setting, up = -1;

	if (cap == 0)
		return 0;
	out[0] = 0;
	if (!state || !state[0])
		return 0;
	running = host_link_pick(state, "running", "on", "off");
	setting = host_link_pick(state, "setting", "on", "off");
	if (running >= 0 || setting >= 0) {
		host_link_put(&o, "# HELP gpon_host_autoneg 1 when the host port (the SFP edge) "
				  "negotiates 1000BASE-X with the host, 0 when it runs forced "
				  "1000BASE-X full duplex. state=running is what runs now, "
				  "state=setting the stored HOST_AUTONEG.\n"
				  "# TYPE gpon_host_autoneg gauge\n");
		if (running >= 0) {
			host_link_put(&o, "gpon_host_autoneg{state=\"running\"} ");
			host_link_put(&o, running ? "1\n" : "0\n");
		}
		if (setting >= 0) {
			host_link_put(&o, "gpon_host_autoneg{state=\"setting\"} ");
			host_link_put(&o, setting ? "1\n" : "0\n");
		}
	}
	if (link && (link[0] == '0' || link[0] == '1') && (link[1] == '\n' || link[1] == 0))
		up = link[0] - '0';
	host_link_gauge(&o, "gpon_host_link_up",
			"1 while the host port has a link (odi_switch.host_link).", up);
	host_link_gauge(&o, "gpon_host_autoneg_fallback",
			"1 when HOST_AUTONEG is off but this boot found no host link forced and "
			"runs auto-negotiation instead (the boot fallback).",
			host_link_pick(state, "fallback", "1", "0"));
	host_link_gauge(&o, "gpon_host_autoneg_trial",
			"1 while a keep-or-revert trial of the host port mode runs.",
			host_link_is(state, "phase", "trial"));
	host_link_gauge(&o, "gpon_host_autoneg_watch",
			"1 while the boot watch is still deciding the host port mode.",
			host_link_is(state, "phase", "watch"));
	host_link_gauge(&o, "gpon_safe_mode",
			"1 on a safe-mode boot: the PON off, the host port on auto-negotiation, "
			"the default password, for this boot only.", safe_now ? 1 : 0);
	host_link_gauge(&o, "gpon_safe_mode_unacknowledged",
			"1 while the record of a safe-mode boot waits to be acknowledged.",
			safe_record ? 1 : 0);
	return o.n;
}

#endif /* ODI_HOST_LINK_H */
