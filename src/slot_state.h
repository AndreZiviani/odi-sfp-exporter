/*
 * The boot-slot metrics, from odi-oss's slot state file.
 *
 * odi-oss writes /var/run/odi-slot at every boot (its slot-state.sh; the
 * format is in odi-oss docs/TOOLS.md, "Slot state"): one KEY=value per line,
 * read out of the U-Boot environment and /proc/cmdline once, so this
 * exporter never parses the environment itself. The keys used here:
 *
 *     running=1               the slot the kernel was booted from
 *     primary_sw_commit=0     sw_commit in the copy U-Boot boots from
 *     fallback_sw_commit=0    sw_commit in the other copy
 *     uncommitted=1           1 when either copy names another slot
 *
 * rendered as
 *
 *     gpon_boot_slot{slot="1"} 1
 *     gpon_committed_slot{copy="primary",slot="0"} 1
 *     gpon_committed_slot{copy="fallback",slot="0"} 1
 *     gpon_uncommitted 1
 *
 * A value that is not exactly 0 or 1 -- empty because it could not be told,
 * or anything else -- emits nothing for that series, and a family with no
 * series emits no header either: an absent series is honest, a guessed 0 is
 * not. No file at all (an older image, the stock firmware) is the same.
 *
 * Pure: no syscalls, so it is tested on the host (test/test_slot_state.c).
 */
#ifndef ODI_SLOT_STATE_H
#define ODI_SLOT_STATE_H

#define SLOT_STATE_PATH "/var/run/odi-slot"

/* The 0 or 1 of `key=` at the start of a line of buf, or -1. */
static int slot_state_bit(const char *buf, const char *key)
{
	unsigned long i = 0;

	while (buf[i]) {
		unsigned long k = 0;

		while (key[k] && buf[i + k] == key[k])
			k++;
		if (!key[k] && buf[i + k] == '=') {
			char c = buf[i + k + 1];
			char e = c ? buf[i + k + 2] : 0;

			if ((c == '0' || c == '1') && (e == '\n' || e == 0))
				return c - '0';
			return -1;
		}
		while (buf[i] && buf[i] != '\n')
			i++;
		if (buf[i])
			i++;
	}
	return -1;
}

struct slot_out {
	char *p;
	unsigned long n, cap;
};

static void slot_put(struct slot_out *o, const char *s)
{
	while (*s && o->n + 1 < o->cap)
		o->p[o->n++] = *s++;
	o->p[o->n] = 0;
}

/* Render the metrics for the file contents in buf into out (NUL-terminated,
 * truncated at cap). Returns the length. */
static unsigned long slot_state_render(const char *buf, char *out, unsigned long cap)
{
	struct slot_out o = { out, 0, cap };
	int running = slot_state_bit(buf, "running");
	int cp = slot_state_bit(buf, "primary_sw_commit");
	int cf = slot_state_bit(buf, "fallback_sw_commit");
	int un = slot_state_bit(buf, "uncommitted");

	if (cap == 0)
		return 0;
	out[0] = 0;
	if (running >= 0) {
		slot_put(&o, "# HELP gpon_boot_slot Always 1. slot is the firmware slot "
			     "the running kernel was booted from (0 or 1), from "
			     SLOT_STATE_PATH ".\n# TYPE gpon_boot_slot gauge\n"
			     "gpon_boot_slot{slot=\"");
		slot_put(&o, running ? "1" : "0");
		slot_put(&o, "\"} 1\n");
	}
	if (cp >= 0 || cf >= 0) {
		slot_put(&o, "# HELP gpon_committed_slot Always 1. slot is the slot "
			     "sw_commit names in that copy of the U-Boot environment: "
			     "primary is the copy U-Boot boots from, fallback the one it "
			     "falls back to.\n# TYPE gpon_committed_slot gauge\n");
		if (cp >= 0) {
			slot_put(&o, "gpon_committed_slot{copy=\"primary\",slot=\"");
			slot_put(&o, cp ? "1" : "0");
			slot_put(&o, "\"} 1\n");
		}
		if (cf >= 0) {
			slot_put(&o, "gpon_committed_slot{copy=\"fallback\",slot=\"");
			slot_put(&o, cf ? "1" : "0");
			slot_put(&o, "\"} 1\n");
		}
	}
	if (un >= 0) {
		slot_put(&o, "# HELP gpon_uncommitted 1 when sw_commit in either copy "
			     "of the U-Boot environment is not the running slot (a trial "
			     "boot: the next reset boots another slot), 0 when both name "
			     "it.\n# TYPE gpon_uncommitted gauge\ngpon_uncommitted ");
		slot_put(&o, un ? "1" : "0");
		slot_put(&o, "\n");
	}
	return o.n;
}

#endif /* ODI_SLOT_STATE_H */
