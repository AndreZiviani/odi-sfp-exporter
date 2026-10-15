# Changelog

## Unreleased

- Host port and safe-mode metrics, from the files an odi-oss image with
  `HOST_AUTONEG` and the safe mode keeps (`/var/run/host-autoneg`,
  `/var/run/safe-mode`, `/etc/config/safe-mode`, the `odi_switch.host_link`
  parameter): `gpon_host_autoneg{state="running|setting"}` (1 negotiating,
  0 forced 1000BASE-X), `gpon_host_link_up`, `gpon_host_autoneg_fallback`,
  `gpon_host_autoneg_trial`, `gpon_host_autoneg_watch`, `gpon_safe_mode`
  and `gpon_safe_mode_unacknowledged`. Nothing is emitted without the state
  file (an older image, the stock firmware). Alerts: `OdiSafeModeBoot`
  (critical: the PON is off and the default password in force for that
  boot), `OdiSafeModeUnacknowledged` and `OdiHostLinkFallback`, with
  promtool tests. `src/host_link.h`, tested natively
  (`test/test_host_link.c`).
  The alert texts describe the safe-mode trigger as power cycles cut before
  the stick had been up a minute (about 60 to 110 s after power-on, as
  measured on hardware), not power-ups shorter than a minute.

- `gpon_gem_frames_total{direction,gem_port}`, `gpon_gem_bytes_total` and
  `gpon_gem_eth_frames_total{direction,gem_port,kind="rx|fwd|tx"}`: what each
  GEM port has carried, downstream and upstream, from the new odi-oss
  `/proc/odi_gpon/flows` (the driver samples the clear-on-read per-flow
  registers and keeps 64-bit totals; this side only renders them), and
  `gpon_gem_flow_count_faults_total{kind}` for counts the driver could not
  read in time or read full. A silent GEM port on a multi-service line shows
  where the aggregate counters cannot. The file is optional like the
  others: the stock firmware and an older odi-oss kernel have none, and then
  nothing is emitted and the rest is unchanged. Totals are read as up to 20
  digits (a byte total passes 10 within hours). Test:
  `test/test_gpon_flow.c`, and a scrape with and without the file in
  `test/test_gpon_link.c`. Refs #92.

- BREAKING: the GPON metrics read odi-oss `/proc/odi_gpon/status`,
  `counters` and `ploam` (odi-oss split the old `/proc/odi_gpon` file into
  that directory, same line formats), so this metricsd needs an odi-oss
  kernel with the split, and an odi-oss kernel with the split needs this
  metricsd. A pair that does not match loses the `gpon_provision_tcont*`
  and GPON link metrics, nothing else. Each file is optional: on the stock
  firmware, which has no `/proc/odi_gpon`, nothing changes (no detection;
  a missing file only skips the metrics from it, and every `diag` and
  `omcicli` metric is emitted as before). Test: a scrape with none of the
  files present (`test/test_gpon_link.c`). Refs #92 (groundwork).

## v1.4.1 (2026-10-14)

- GPON link metrics: a total of 11 digits or more (10000000000 and up) was
  dropped from the scrape, so `gpon_us_gem_bytes_total` vanished after a few
  hours of traffic and the idle GEM frame total soon after boot. Values of
  up to 20 digits, the 64-bit range the driver keeps, are now emitted.

## v1.4.0 (2026-10-13)

- `gpon_port_{receive,transmit}_octets_total`: a counter reset is no longer
  read as a 32-bit wrap. The 64-bit extension treated every reading that went
  backwards as one wraparound, so port 0, which `omci_app` clears every 15
  minutes (487034420 then 27505987 was observed), gained a phantom 2^32 -
  last + raw, about 3.8 GB, at each boundary: a spike of roughly 100 Mbit/s in
  `rate()` on the host-side throughput panel of every stock-firmware stick, in
  a counter that looked monotonic and clean. A reading that falls from below
  2^31 cannot have wrapped, so it is now a reset and adds only what it holds.
  A reset from 2^31 or more still looks like a wrap (at most 2^31 + the new
  reading is added, half the old error). `test/test_wrap.c` has the observed
  pair, a reset to zero, the 2^31 edge and a wrap followed by a reset.

- `grafana/slack.tmpl`: a link appears only when its URL is a full http(s)
  URL; a test notification has no rule, and Grafana fills its rule URL with a
  bare `?orgId=1`, which Slack showed as a literal `<|Rule>`.

- `grafana/slack.tmpl`: a Slack notification template for Grafana alerting.
  One line per alert with a severity marker, the stick (or host or job) and
  the summary, the description, and short Rule / Silence / Dashboard links,
  instead of the default dump of every label and annotation; a rule that
  could not be evaluated shows as a warning, never as resolved. Validated
  with Grafana's own template engine. docs/ALERTS.md, "Grafana Cloud and
  Slack", also covers the error state to set after an import (the import
  leaves OK, which resolves a firing alert on any query error) and the
  panel each rule should link for notification images.

- `OdiGponAlarm`: the description no longer says the exporter has no BIP or
  FEC counters; it points at `gpon_ds_fec_codewords_total` and
  `gpon_ds_bip_error_blocks_total` (odi-oss v1.3.0 or later).

- `grafana/gpon-stats.json`: the dashboard now has two tabs. "Overview"
  holds the previous rows plus a compact "GPON link" row, after Optics, for
  the v1.3.0 line counters: stats for upstream grants/s, uncorrectable FEC
  blocks and LOS events over the range, one "Line errors/s" panel (FEC
  corrected and uncorrectable codewords, BIP error blocks), upstream
  grants/s over time, and a "Link events" bar panel (LOS/LOF/LOM and ONU
  state transitions). "GPON link detail" holds the rest: FEC in use,
  ONU-ID, counter sampler health, FEC codewords and the estimated pre-FEC
  bit error rate, BIP errors, BWmap allocations and errors, alarm events
  and seconds, state transitions, PLOAM, REI and timers, downstream GEM
  frames and errors, OMCI frames, upstream bursts, GEM frames and bytes.
- `prometheus/alerts.yml`: `OdiOnuNoUpstreamGrants` (O5 but the OLT grants
  no upstream time, critical) and `OdiFecUncorrectable` (uncorrectable FEC
  codewords, warning), with promtool tests; docs/ALERTS.md no longer says
  the exporter has no BIP or FEC counters.

## v1.3.0 (2026-10-12)

- The GPON link, from odi-oss `/proc/odi_gpon` (src/gpon_link.h): the line
  counters of the GPON MAC as the kernel driver accumulates them (BIP
  errors, FEC corrected bits, bytes and codewords and the uncorrectable
  ones, PLend and BWmap errors, PLOAMd, BWmap allocations for this ONU, GEM
  idle and non-idle frames and GEM errors, downstream Ethernet and OMCI
  frames, upstream bursts, PLOAMu, DBRu, GEM frames and bytes), how often
  and how long LOS, LOF and LOM were raised, time in each ONU state and the
  state transitions, FEC in use per direction, the superframe count, and
  the driver's PLOAM, REI, TO1/TO2 and interrupt counts. Counters are
  `*_total`. A weak fibre shows as FEC corrections and BIP errors climbing,
  and an ONU that says O5 while the OLT no longer grants it as
  `gpon_ds_bwmap_allocations_total` standing still. The counters clear in
  hardware when read, so the exporter never reads them: the driver is
  their one reader. Needs an odi-oss kernel with the counters; on any other
  the families are absent. docs/METRICS.md, "The GPON link".

- Dashboard: the Load averages note says odi-oss idles near 0 (2.00 was the
  stock firmware), and Port Hardware Receive Errors explains that port-3
  undersize is one count per OMCI reply.
- `grafana/gpon-stats.json`: the maintainer's Grafana dashboard (optics, ONU
  state, alarms, port throughput/drops/errors, pause frames, frame sizes,
  OMCI services, memory, uptime, image info), as a Grafana 12
  `dashboard.grafana.app/v2` resource with an `instance` variable. Linked
  from the README.
- README: the "See docs/..." sentence had the provisioning paragraph merged
  into its middle; put back in order.
- docs: repair text merged into the wrong place by union-resolved rebases.
  AGENTS.md listed `src/metrics_body.h` twice in the layout and its `make
  test` paragraph broke off mid-line and left out three of the tested
  headers; docs/METRICS.md ran the OMCI paragraph into the config paragraph
  with no blank line and had the boot-slot section between the health-gauge
  table and the text that explains it; the layout in docs/DESIGN.md read "but
  gpon_provision_*"; the Makefile comment above `test` was cut mid-sentence.

## v1.2.1

- AGENTS.md has a release checklist: every change since the last tag has an
  entry, no merge debris, `Unreleased` moved into the tag section, and the
  published notes checked.
- `OdiPortReceiveErrors` no longer counts `kind="undersize"` on port 3 (the
  CPU port). Every upstream OMCI reply crosses it as a 52-byte frame (the
  48-byte baseline message plus the FCS, unpadded so the OLT receives
  exactly 48 bytes), which the switch counts as undersize and forwards; the
  stock firmware does the same. On a line whose OLT polls, the alert fired
  permanently (about 160 per 15 min on ISP2). CRC, fragment and jabber errors
  on port 3 still count. docs/METRICS.md, "Known caveats", has the evidence;
  `prometheus/alerts_test.yml` covers both cases.

## v1.2.0

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
- New boot-slot metrics from odi-oss `/var/run/odi-slot` (written by its
  `slot-state.sh` at boot; one small tmpfs file, no fork, no parsing of the
  U-Boot environment here): `gpon_boot_slot{slot}`,
  `gpon_committed_slot{copy="primary|fallback",slot}` and
  `gpon_uncommitted` (1 on a trial boot nobody committed). A value the file
  leaves empty omits that series, and no file omits all three. Rendering is
  host-tested (`test/test_slot_state.c`, in `make test`). docs/METRICS.md.
- New alert `OdiUncommittedImage` (warning): `gpon_uncommitted == 1` for 30
  minutes, with a promtool test. docs/ALERTS.md.

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
- Added `gpon_provision_*`: what the OLT provisioned, so an ISP plan change
  (speed tier, VLAN, T-CONT) is visible in Grafana. `gpon_provision_tconts`
  and `gpon_provision_tcont_info{alloc_id}` from `/proc/odi_gpon` (the
  Alloc-IDs the OLT assigned by PLOAM; no fork); from `omcicli provision`,
  one more fork under the same 2 s bound as `dump srvflow`, and skipped when
  that one timed out: `gpon_provision_gem_ports`,
  `gpon_provision_gem_port_info{gem_port,direction}`,
  `gpon_provision_vlan_info{vlan,source}`,
  `gpon_provision_traffic_descriptors`,
  `gpon_provision_traffic_descriptor_{cir,pir}_bytes_per_second{descriptor}`,
  `gpon_provision_mib_entities` and `gpon_provision_mib_data_sync`. Needs an
  odi-oss image with `omcli provision` and the `alloc_ids` line; anything
  older, or the vendor `omci_app`, leaves the families absent. The parsing
  lives in `src/provision.h`, pure text in and out, and `make test` now runs
  it against fixtures and goldens (`test/test_provision.c`).

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
