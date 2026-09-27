# Changelog

## Unreleased

- CI: a `v*` tag release now uses the matching `## <tag>` section of this
  file as its release body, falling back to the previous auto-generated
  notes when a tag has no section.
- CI: a pull request that changes anything besides docs (`docs/`,
  `README.md`) or CI (`.github/`) must also update this file, unless labeled
  `no-changelog`.
- AGENTS.md: documented the rule above.

## v1.1.1

(v1.1.0 was tagged and pushed but its CI run failed on `make test` writing
into a root-owned `build/` on the Linux runner; fixed and re-tagged as
v1.1.1 rather than force-pushing over the existing v1.1.0 tag. No v1.1.0
release was published.)

**Metric rename — dashboards and alerts using `kind="oversize"` need
updating.**

- Removed `gpon_port_receive_errors_total{kind="oversize"}`. It was never an
  error: `etherStatsRxOversizePkts`/`etherStatsTxOversizePkts` are register
  aliases for the `1519_max` frame-size bucket (verified by two independent
  live counters matching exactly: 38605 == 38605 rx, 52482 == 52482 tx), i.e.
  ordinary VLAN-tagged full-size (1522 B) frames being forwarded normally.
- Added `gpon_port_frames_total{port,direction="rx|tx",size="64|65_127|128_255|256_511|512_1023|1024_1518|1519_max"}`,
  the full frame-size histogram in both directions (replaces the row above).
- `gpon_port_receive_errors_total`'s remaining kinds (`crc_align`, `fragment`,
  `jabber`, `undersize`) are unchanged.
- `gpon_port_receive_octets_total` / `gpon_port_transmit_octets_total` are now
  extended to a monotonic 64-bit total in-process (`src/wrap.h`), guarding
  against a stick whose kernel driver still reports `ifInOctets`/`ifOutOctets`
  as plain 32-bit registers that wrap at 4.29 GB. See docs/DESIGN.md and
  docs/METRICS.md for the design and its one known limitation (more than one
  wrap inside a single scrape interval).
- docs/METRICS.md: documented which metric is throughput, added a PromQL
  Mbit/s example, and called out that `gpon_network_*` only ever sees the
  stick's own management traffic (forwarding happens in switch hardware).
- Added `make test` (host-native, no Docker/qemu-user): a unit test for the
  wraparound extension and a fixture check on the mib_* tables, wired into
  `make all` and CI.

## v1.0.3 and earlier

See git tags `v1.0.0`..`v1.0.3` and their GitHub Releases; this file starts
tracking changes from v1.1.0 on.
