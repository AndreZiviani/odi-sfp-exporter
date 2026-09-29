# Changelog

## Unreleased

- Added `gpon_boot_count` (boots of the image since the last power cycle) and
  `gpon_last_reset_reason{reason,client}` (always 1: why the previous boot
  ended -- `wdt_client` with the client, `wdt_mem`, `wdt_userland`, `reboot`,
  `halt`, `poweroff`, `panic`, `oops`, `power` or `unknown`), from odi-oss's
  `/proc/odi_ramlog_prev`, so an alert can fire on a watchdog reset. One
  bounded read on the first scrape that finds the file, kept for the boot;
  absent on a kernel without it. The parse is in `src/resetinfo.h`, tested on
  the host by `test/test_resetinfo.c` (`make test`).
- Added `gpon_config_info{file,hash}` (always 1) and
  `gpon_config_mtime_seconds{file}` for the three config-store files under
  `/var/config` (`lastgood.xml`, `lastgood_hs.xml`, `odi.conf`), so an alert
  can fire when the provisioning identity (GPON serial, PLOAM password, LOID,
  VLAN) is lost or changes. `hash` is the first 12 hex digits of the file md5;
  no value is ever exported. One `stat64` per file per scrape; `/bin/md5sum`
  is forked only when a file inode, size, mtime or ctime changed, bounded by
  `MD5SUM_TIMEOUT_MS` (2 s). The parse and the staleness test are in
  `src/confighash.h`, tested on the host by `test/test_confighash.c`
  (`make test`); `struct stat64` comes from the toolchain `<asm/stat.h>`
  rather than a hand-copied o32 layout. See docs/METRICS.md for the alert
  expressions.
- Added Prometheus alerting rules, `prometheus/alerts.yml`, documented in
  docs/ALERTS.md: exporter down or absent, diag and OMCI health, ONU not in
  O5, GPON alarms, O5 with no OMCI services and services dropping, port
  receive errors, rx and tx power against the G.984.2 class B+ limits, rx
  power and laser bias current against the stick own 7-day mean (the
  ageing-laser warning), module temperature and supply voltage, watchdog
  resets and reset loops, low memory, and config-store changes and loss.
  The reset rules need `gpon_last_reset_reason`/`gpon_boot_count` and the
  config rules `gpon_config_info`/`gpon_config_mtime_seconds`; without those
  metrics they never fire. `make rules` runs `promtool check rules` and the
  unit tests in `prometheus/alerts_test.yml` in a digest-pinned
  `prom/prometheus` image; CI runs it on every push and PR.

## v1.1.2

- **Every child metricsd forks is now bounded.** Hardware trial (rc3, claro,
  2026-09-28): a stuck omcid (see odi-oss CHANGELOG, the respawn/`vq_ensure()`
  bug) left `run_to_buf()`'s blind `read()` on the `omcicli` child parked
  forever, so the whole exporter -- single-threaded -- stopped accepting new
  connections even though it was otherwise healthy. `run_to_buf()` and
  `run_script_to_buf()` (`src/syscall.h`) now poll the child's pipe with a
  timeout (`OMCICLI_TIMEOUT_MS` 2 s, `DIAG_TIMEOUT_MS` 3 s,
  `src/metrics_body.h`) and SIGKILL + reap the child instead of waiting on it
  past that bound.
- Added `gpon_omci_up`: 0 when `omcicli dump srvflow` timed out against a
  stuck omcid, so the failure shows up as a metric rather than as silence
  (`gpon_omci_services` was previously just omitted). `gpon_diag_up` already
  covered the same case for `/bin/diag` and needed no new metric, only the
  bound.
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
