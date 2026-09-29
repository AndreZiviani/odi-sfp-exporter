# Metrics reference

Full metric list, sourcing, and the caveats that matter when alerting on any
of these. See [DESIGN.md](DESIGN.md) for why the exporter is built the way it
is.

## Metrics

From `/bin/diag`, **all in a single fork per scrape**:

| metric | source |
|---|---|
| `gpon_rx_power_dbm` | `diag pon get transceiver rx-power` |
| `gpon_tx_power_dbm` | `diag pon get transceiver tx-power` |
| `gpon_bias_current_ma` | `diag pon get transceiver bias-current` |
| `gpon_temperature_celsius` | `diag pon get transceiver temperature` |
| `gpon_voltage_volts` | `diag pon get transceiver voltage` |
| `gpon_onu_state` | `diag gpon get onu-state` — the N in O(N); 5 is operational |
| `gpon_alarm{alarm="..."}` | `diag gpon get alarm-status` — `los`, `lof`, `lom`, `sf`, `sd`, `tx_too_long`, `tx_mismatch`; 1 means asserted |
| `gpon_port_{receive,transmit}_octets_total{port="..."}` | `diag mib dump counter port all` — **the throughput metric**, see below |
| `gpon_port_{receive,transmit}_packets_total{port,kind="unicast\|multicast\|broadcast"}` | same |
| `gpon_port_{receive,transmit}_drops_total{port}` | same |
| `gpon_port_receive_errors_total{port,kind="crc_align\|fragment\|jabber\|undersize"}` | same |
| `gpon_port_pause_frames_total{port,direction="receive\|transmit"}` | same |
| `gpon_port_frames_total{port,direction="rx\|tx",size="64\|65_127\|128_255\|256_511\|512_1023\|1024_1518\|1519_max"}` | same — frame-size histogram |

**`port="2"` is the PON side and `port="0"` the host SerDes side.** These are
the only counters that show whether the stick is actually *forwarding*, so they
are the ones to alert on. Established by correlating deltas over one window on
each of two lines independently, rather than assumed from the port numbers: on
one line, `p2` received 13220591 octets while `p0` transmitted 13203165, and
`p0` received 12626164 while `p2` transmitted 12653844; on the other, 9204264 /
9184671 and 880819 / 898523. The mirror is the switch forwarding between the
two.

These are the same counters the vendor web UI shows (boa's `ponGetStatus`,
which prints them with `%llu`). Reading is **non-destructive** —
`diag mib get count-mode` reports `normal free run`, resetting is a separate
explicit `diag mib reset counter ...` — so a scrape takes nothing away from the
web UI or from a manual `diag`. Both properties are why these can be exported
as real `counter`s.

### Throughput and the octet counters' width

`gpon_port_receive_octets_total` / `gpon_port_transmit_octets_total` are **the
throughput metric** — `port="2"` is the fibre/PON side, `port="0"` the host
side. Mbit/s over a 5-minute window:

```promql
rate(gpon_port_receive_octets_total{port="2"}[5m]) * 8 / 1e6
```

`ifInOctets`/`ifOutOctets` are two consecutive 32-bit registers in odi-oss's
own kernel driver (`odi_switch_mib.c`, `ODI_SW_MIB_WIDE`), so a correctly
matched image reports a real 64-bit reading — one read of `ifInOctets` came
back 5057428519, past 2^32. But a scrape cannot assume the stick it talks to
is running that driver: rc35 prefers an `/etc/config/metricsd` override over
the image's own `/bin/metricsd`, and nothing ties that override to a matching
kernel, so an exporter can end up paired with an older driver that reports
these as plain 32-bit registers, wrapping at 4.29 GB — which is what a live
read on stick `vero` showed. So the exporter no longer trusts the field width:
it tracks the last raw reading per port and extends it into a monotonic
64-bit total itself (`src/wrap.h`; see docs/DESIGN.md). On a stick with the
fixed driver this is a no-op, since the reading then never goes backwards.

What this does **not** cover: two wraps between two scrapes, i.e. more than
4.29 GB of traffic inside one scrape interval — at the default 60 s interval,
about 572 Mbit/s sustained for the whole interval. A shorter `scrape_interval`
narrows this window directly. Every other MIB counter here (packets, drops,
errors, pause frames, the frame-size histogram) is a single 32-bit register
with no equivalent extension, because wrapping one of those needs a sustained
rate many orders of magnitude higher than this hardware forwards.

**Port 0's counters are reset every 15 minutes; port 2's are not.** Nothing in
this exporter does it — `omci_app` does, at each OMCI performance-monitoring
interval boundary. Caught by sampling `ifOutOctets` against ME24's `IntEndTime`
once a minute; both flip in the same 60 s window:

```
T=11727.56  p0out=487034420  IntEndTime=12
T=11787.66  p0out= 27505987  IntEndTime=13
```

ME24 `EthPmHistoryData` has an instance and monitors the **UNI**, which is port
0. ME321/322 — the PON-side Ethernet frame PM MEs — have no instances on
either line tested, which is exactly why port 2 is spared: over 30 consecutive
reads across 5 minutes, and every read taken since, it only ever grew.

This does not change the metric type. `counter` is right precisely because
Prometheus detects a counter reset and handles it; the cost is one interval's
`rate()` every 15 minutes, on the host-side port. **Port 2 — the PON side, the
one that answers "is it forwarding" — is unaffected.** Reading remains
non-destructive either way; the resets come from PM collection, not from
scraping.

The device prints 46 counters per port. Only those with an unambiguous unit are
exported; most of the rest are half-duplex collision counters that mean
nothing on a SerDes or a PON. Run the command by hand to see them all.

From `/proc`, which costs no fork at all:

| metric | source |
|---|---|
| `gpon_uptime_seconds` | `/proc/uptime` |
| `gpon_boot_count` | `/proc/odi_ramlog_prev` — boots of this image since the last power cycle, this one included |
| `gpon_last_reset_reason{reason="...",client="..."}` | `/proc/odi_ramlog_prev` — always 1; why the previous boot ended, see below |
| `gpon_load{1,5,15}` | `/proc/loadavg` |
| `gpon_memory_bytes{kind="total\|free\|buffers\|cached"}` | `/proc/meminfo` |
| `gpon_network_{receive,transmit}_{bytes,packets,errs,drop}_total{device="..."}` | `/proc/net/dev` |

From the config partition, `/var/config` (jffs2, shared by both firmware
slots, never written by a reflash), one `stat` per file per scrape and a fork
of `/bin/md5sum` only when a file changed:

| metric | source |
|---|---|
| `gpon_config_info{file,hash}` | always 1; `hash` is the first 12 hex digits of `md5sum /var/config/<file>` |
| `gpon_config_mtime_seconds{file}` | `stat` of the same file: its mtime, by the stick clock when it was written |

`file` is one of `lastgood.xml` (the service settings: VLAN, LOID, PLOAM
password), `lastgood_hs.xml` (the hardware identity: GPON serial, MAC) and
`odi.conf` (the odi-oss only keys, `SYSLOG_SERVER` and `NTP_SERVER`; absent
until one is set). These are the provisioning identity: losing or silently
changing one is an outage, and they survive everything except an explicit
write or an erase of the partition. **Only a hash is exported**, never a
value: the files hold the PLOAM and LOID passwords. Check a label by hand on
the stick with `md5sum /var/config/lastgood.xml | cut -c1-12`.

Alert on the **hash**, not the mtime. A config change shows up as a new
`gpon_config_info` series for the same `file`, so two series for one file
inside a window is a change:

```promql
count by (instance, file) (last_over_time(gpon_config_info[30m])) > 1
```

and a missing file as its mtime series going away while the exporter still
answers (`gpon_config_mtime_seconds` comes from `stat` alone, so a failed
`md5sum` does not look like a missing file):

```promql
gpon_exporter_up unless on (instance) gpon_config_mtime_seconds{file="lastgood_hs.xml"}
```

The mtime is by the stick clock, which starts at 1970 on every boot until NTP
sets it (`NTP_SERVER`, odi-oss) -- so a write before NTP synced carries a
1970 date. It says when, the hash says whether.

The hash is cached against each file's inode, size, mtime and ctime, so
`md5sum` runs once per boot and again only after a write. jffs2 keeps whole
seconds; a write that kept all four the same would be missed until the next
one that does not. `md5sum` is bounded by `MD5SUM_TIMEOUT_MS` (2 s); on a
timeout or failure the hash is left out, the mtime is still exported, and the
next scrape tries again.

Plus three health gauges:

| metric | meaning |
|---|---|
| `gpon_exporter_up` | always 1 — distinguishes "scraped and found nothing" from "did not scrape" |
| `gpon_image_info` | which firmware image this stick was built from, and the component builds inside it |
| `gpon_diag_up` | 1 when `/bin/diag` ran and at least one section parsed |
| `gpon_diag_sections_parsed` / `_expected` | how much of the diag scrape was understood |

`gpon_exporter_up` covers only the `/proc` half, so it stays 1 while every
diag-derived metric is missing. The other two close that: `gpon_diag_up 0` is a
diag that did not run, and `parsed < expected` is a scrape that ran and was
**truncated** — which is the quiet one, because the tail sections are lost and
what remains looks like a healthy scrape with no forwarding data.

    gpon_diag_up == 0                                  # diag is broken
    gpon_diag_sections_parsed < gpon_diag_sections_expected   # partial scrape

**A metric that cannot be read is omitted entirely, never emitted as zero.** An
absent series is honest; `0` reads as a genuine measurement of zero dBm.

### Why the stick last reset

odi-oss's kernel records why each boot ends, at the moment it knows, in DRAM
that survives a reset, and prints it on the next boot as `reason=` in the
second line of `/proc/odi_ramlog_prev`. The exporter turns that line into one
series:

| `reason` | meaning |
|---|---|
| `wdt_client` | the kernel watchdog reset the board because a registered client missed its ping deadline; `client` names it (e.g. `client="omcid"`) |
| `wdt_mem` | the watchdog reset it because `MemAvailable` stayed below the floor |
| `wdt_userland` | the watchdog reset it because userland never confirmed the boot |
| `reboot`, `halt`, `poweroff` | the `reboot` syscall path (a halt or power-off ends in a watchdog reset about 42 s later) |
| `panic`, `oops` | a kernel panic, or an oops that did not panic |
| `power` | DRAM lost its contents: a power cycle or a cold boot |
| `unknown` | the DRAM survived but nothing recorded a reason: a hang the hardware watchdog caught, an emergency restart, or a previous boot of an image too old to record one |

`client` is present only for `wdt_client`. Both series are absent on a stock
kernel (no `/proc/odi_ramlog_prev`), and `gpon_last_reset_reason` alone on an
odi-oss kernel from before the reason existed (the line has no `reason=`).
Values are fixed for the life of a boot, so the file is read once, on the
first scrape that finds it, and not again: the kernel renders the whole saved
ramlog, about 8 KB, on every read. The read is bounded to its first 255
bytes.

A watchdog reset since the last scrape, for an alert:

```promql
gpon_last_reset_reason{reason=~"wdt_.*"} and on(instance) changes(gpon_boot_count[15m]) > 0
```

`gpon_boot_count` restarts at 1 after a power cycle, so
`resets(gpon_boot_count[1d])` counts those, and `changes()` any boot at all.

## One fork per scrape, not one per metric

`/bin/diag` costs ~32 ms per invocation on this CPU, and almost all of it is its
own startup — it links `librtk`, `libmib` and `libomci_api`, and relocating
those dwarfs the work. Measured over 20 iterations each:

| | per call |
|---|---|
| `diag pon get transceiver rx-power` | 32.5 ms |
| `diag gpon get alarm-status` | 32.0 ms |
| `diag mib dump counter port all` (5.9 KB, 92 counters) | 35.5 ms |
| `/bin/true` — bare fork+exec baseline | 4.0 ms |

So cost scales with the number of *processes*, not the number of metrics. diag
also reads commands from stdin and echoes each after its `RTK.0> ` prompt, so
the whole scrape goes into one invocation and the output splits back into
per-command sections. Measured end to end over HTTP on the device:

| | per scrape |
|---|---|
| one fork, 8 commands, 29 families | **86.5 ms** |
| eight forks, 21 families | 291.5 ms |

An additional metric now costs its own work — 1-3 ms — instead of another
process startup.

Two consequences worth knowing. **This is one failure domain**: a diag that
hangs or crashes now costs every diag-derived metric rather than one. That is
what `gpon_diag_up` and the section counts are for — the `/proc` metrics are
unaffected and `gpon_exporter_up` is hardcoded to 1, so without them the only
signal is ~90 series going absent, which looks identical to a stick that has not
been scraped yet or to a relabelling mistake. And **it depends on the `RTK.0> ` prompt
string** to split sections; if that ever changes, metrics go absent rather than
wrong. The command list and the strings matched against it are built from one
table in `src/metrics_body.h` so they cannot drift apart.

## Known caveats

- **`kind="oversize"` was removed from `gpon_port_receive_errors_total` in
  v1.1.0; it never belonged there.** `etherStatsRxOversizePkts` /
  `etherStatsTxOversizePkts` turned out to be exactly the `1519_max`
  size bucket — two independent live counters matched exactly
  (38605 == 38605 rx, 52482 == 52482 tx) — which is ordinary VLAN-tagged
  full-size (1522 B) traffic being forwarded normally, not an error. It is
  now under `gpon_port_frames_total{size="1519_max"}`. **A dashboard or
  alert built on `gpon_port_receive_errors_total{kind="oversize"}` needs
  updating** to use the new metric/label.
- **`gpon_load*` is pinned and carries no signal.** Linux counts uninterruptible
  tasks in the load average, and this firmware keeps two kernel threads
  (`watchdog`, `led_swBlink`) permanently in D state. Load therefore sits at
  exactly 2.00 forever. Do not alert on it.
- **`/proc/net/dev` counters are 32-bit** and wrap at 4.29 GB, because
  `struct net_device_stats` uses `unsigned long` on this platform. Prometheus
  cannot distinguish a wrap from a counter reset, so `rate()` undercounts on a
  busy link. At 100 Mbps sustained a wrap happens roughly every six minutes.
- **`gpon_memory_bytes` would overflow above 4 GB.** These devices have tens of
  megabytes, so this is theoretical.
- **`pon0` reports zeros** on every field, and `eth0`/`br0` only ever show the
  stick's **own management traffic** — forwarding happens in switch hardware and
  never reaches the CPU. Over one 25 s window `eth0` moved 2390/11134 bytes
  while the switch ports moved ~13 MB each way. So `/proc/net/dev` cannot tell
  you whether the link is carrying service; use `gpon_port_*` for that.

## Deliberately not exported

`diag gpon show counter global ds-eth` returns Ethernet frame counters, and they
are **read-and-clear**: four consecutive reads gave 55827, 13537, 53607, 8873.
Two problems, the second worse — they are not monotonic, so `counter` is the
wrong type; and reading is destructive, so a scrape silently consumes the delta
from anything else reading those registers, including the vendor web UI.

Making them usable would mean accumulating deltas into a running total held in
the exporter process, which is only sound if it is the sole reader. The parser
is still in `src/metrics_body.h`, unused — and now unnecessary: the
`diag mib dump counter` block above carries the same traffic volume in both
directions, free-running, 64-bit and non-destructive. That is what the web UI
was reading all along.

OMCI (`omcicli`) is not used either. Its ANI-G optical values duplicate `diag`
at 0.002 dB granularity instead of six decimals, and the FEC performance
monitoring entity that would have justified it (ME 312) has no instances unless
the OLT creates it.
