#!/bin/sh
# Fixture check for the mib_* tables in src/metrics_body.h: the oversize
# reclassification and the new frame-size family. These tables drive an
# embedded target (no host binary to run against a fixture scrape), so this
# checks the source directly rather than exec'ing anything -- cheap, and it
# fails loudly the moment either table drifts back.
#
# Run by `make test`; must always exit 0 on pass, non-zero on any failure.
set -eu

cd "$(dirname "$0")/.."
src=src/metrics_body.h
fail=0

note() { echo "FAIL: $1" >&2; fail=1; }

# oversize must no longer be an error kind.
if grep -q 'kind=\\"oversize\\"' "$src"; then
	note "gpon_port_receive_errors_total still has kind=\"oversize\""
fi

# The frame-size histogram must exist, with the two 1519+ buckets that
# absorbed what oversize used to mean.
grep -q 'gpon_port_frames_total' "$src" ||
	note "gpon_port_frames_total metric name is missing"
grep -q 'etherStatsRxPkts1519toMaxOctets.*size=\\"1519_max\\"' "$src" ||
	note "rx 1519_max bucket missing from mib_frames"
grep -q 'etherStatsTxPkts1519toMaxOctets.*size=\\"1519_max\\"' "$src" ||
	note "tx 1519_max bucket missing from mib_frames"

# Real errors must still be exported (this must NOT have been dropped along
# with oversize).
for kind in crc_align fragment jabber undersize; do
	grep -q "kind=\\\\\"$kind\\\\\"" "$src" ||
		note "gpon_port_receive_errors_total lost kind=\"$kind\""
done

# The two octet families must extend through wrap.h, everything else must not
# (only ifInOctets/ifOutOctets can plausibly wrap inside one scrape interval;
# see docs/METRICS.md).
awk '
	/mib_rx_octets, rx_octet_state/ { rx = 1 }
	/mib_tx_octets, tx_octet_state/ { tx = 1 }
	END {
		if (!rx) { print "gpon_port_receive_octets_total is not wired to rx_octet_state"; e = 1 }
		if (!tx) { print "gpon_port_transmit_octets_total is not wired to tx_octet_state"; e = 1 }
		exit e
	}
' "$src" || fail=1

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "test_metrics: all checks passed"
