/*
 * The octet-counter logic, split out from metrics_body.h so it can be tested
 * with a host compiler (test/test_wrap.c) instead of only under qemu-user.
 * Deliberately dependency-free: no syscalls, no libc, just integer
 * arithmetic, so both the MIPS cross-build and an ordinary host gcc compile it
 * unchanged.
 *
 * Why this exists: the switch's ifInOctets/ifOutOctets are 64-bit. odi-oss's
 * kernel driver composes each from two consecutive 32-bit words
 * (odi_switch_mib.c, ODI_SW_MIB_WIDE), as it has since its first release,
 * and the stock firmware prints them wide too. They do not wrap, but they are
 * reset: port 0 at every PM boundary (omci_app), any port by
 * `diag mib reset counter` or a driver reload. extend_octets() below keeps a
 * running total per port that counts on from where the register fell, so
 * the exported counter only grows. See docs/DESIGN.md and docs/METRICS.md.
 */

#ifndef ODI_WRAP_H
#define ODI_WRAP_H

/*
 * Parse a run of `len` ASCII digits (as matched by the mib line scanner) into
 * a 64-bit value. No overflow guard: diag never prints more than 20 digits,
 * and the caller only hands this a digit run it already validated.
 */
static unsigned long long parse_u64(const char *s, unsigned long len)
{
	unsigned long long v = 0;
	unsigned long i;

	for (i = 0; i < len; i++)
		v = v * 10 + (unsigned long long)(s[i] - '0');
	return v;
}

/*
 * Division-free decimal formatting for a 64-bit value into `dst` (caller
 * supplies at least 20 bytes), returning the digit count. Same trick as
 * odi-oss's diag (src/diag/src/io.c divmod10()): a 64-bit divide would pull in
 * __udivdi3, which does not exist in a -nostdlib link.
 */
static unsigned long format_u64(char *dst, unsigned long long v)
{
	char tmp[20];
	unsigned long n = 0, i;

	if (!v) {
		dst[0] = '0';
		return 1;
	}
	while (v) {
		unsigned long long q = (v >> 1) + (v >> 2);
		unsigned r;

		q += q >> 4;
		q += q >> 8;
		q += q >> 16;
		q += q >> 32;
		q >>= 3;
		r = (unsigned)(v - ((q << 3) + (q << 1)));
		if (r > 9) {
			q += 1;
			r -= 10;
		}
		tmp[n++] = (char)('0' + r);
		v = q;
	}
	for (i = 0; i < n; i++)
		dst[i] = tmp[n - 1 - i];
	return n;
}

/* One switch, four ports (0..3); the mib dump prints one digit each. */
#define MIB_MAX_PORTS 4

/*
 * Per-port running state for one octet counter (rx or tx). `seen` is false
 * until the first scrape establishes a baseline -- there is nothing to
 * extend against on process start, same as any other Prometheus counter's
 * first sample.
 */
struct octet_state {
	unsigned long long last_raw;
	unsigned long long extended;
	int seen;
};

/*
 * `port` is the ASCII port number as printed after "Port:"; anything that is
 * not exactly one digit in range is refused rather than guessed, so a device
 * with more ports fails safe (the caller falls back to printing the raw
 * value) instead of indexing out of bounds.
 */
static int port_index(const char *port, unsigned long plen)
{
	if (plen != 1 || port[0] < '0' || port[0] >= '0' + MIB_MAX_PORTS)
		return -1;
	return port[0] - '0';
}

/*
 * Extend one raw reading against the running per-port total.
 *
 * A value that grew since the last scrape is a plain delta. A value that went
 * backwards is a reset: the register restarted, so what it holds now is the
 * traffic since (extended += raw), the same thing Prometheus does with a
 * reset. A 64-bit register does not wrap, so no fall is taken for one,
 * whatever it fell from: port 0's observed 487034420 -> 27505987 at a PM
 * boundary adds 27505987, and a fall to the same reading from 3000000000 or
 * from 6000000000 adds the same. Taking a fall from the upper half of 32 bits
 * for a 32-bit wrap added a phantom 2^32 - last + raw, up to 4.29 GB, at each
 * such reset (1000, 3000000000, 1000 exported 4294968296).
 */
static unsigned long long extend_octets(struct octet_state *st, unsigned long long raw)
{
	if (!st->seen) {
		st->seen = 1;
		st->extended = raw;
	} else if (raw >= st->last_raw) {
		st->extended += raw - st->last_raw;
	} else {
		st->extended += raw;
	}
	st->last_raw = raw;
	return st->extended;
}

#endif /* ODI_WRAP_H */
