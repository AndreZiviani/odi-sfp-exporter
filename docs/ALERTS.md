# Alerting rules

[`prometheus/alerts.yml`](../prometheus/alerts.yml) is a Prometheus rule file
built on the metrics in [METRICS.md](METRICS.md): early warning for a stick
that is degrading, and loud alerts for one that has stopped working. Load it
next to your scrape config:

```yaml
rule_files:
  - /etc/prometheus/odi-alerts.yml   # a copy of prometheus/alerts.yml
```

and route the `severity` label (`critical`, `warning`, `info`) in
Alertmanager. For Grafana-managed alerting, the same expressions work as
alert queries against the Prometheus data source; the file can also be
loaded into a Mimir or Cortex ruler as it is.

No rule names a scrape job. Every expression is either a `gpon_*` series or
joins `up` on `gpon_exporter_up` by `job` and `instance`, so the rules apply
to whatever job the sticks are scraped in and to nothing else.

`make rules` runs `promtool check rules` and the unit tests in
[`prometheus/alerts_test.yml`](../prometheus/alerts_test.yml) in the pinned
`prom/prometheus` image; CI runs it on every push and pull request. Each test
feeds synthetic series and asserts which alerts fire and which do not,
including a healthy stick on which nothing may fire.

## The alerts

### Exporter and data path

| alert | severity | fires when |
|---|---|---|
| `OdiExporterDown` | critical | the scrape of a target that exported `gpon_exporter_up` in the last day has failed for 5 min |
| `OdiExporterAbsent` | warning | such a target has had no `gpon_exporter_up` for 10 min and no failing scrape explains it: removed from service discovery, or relabelled |
| `OdiDiagDown` | warning | `gpon_diag_up == 0` for 5 min: every optics, state, alarm and port metric is missing |
| `OdiDiagTruncated` | warning | fewer diag sections parsed than expected for 15 min: the tail, port counters included, is silently missing |
| `OdiOmciDown` | warning | `gpon_omci_up == 0` for 5 min: omcid does not answer its command queue |

### GPON

| alert | severity | fires when |
|---|---|---|
| `OdiOnuNotOperational` | critical | `gpon_onu_state != 5` for 2 min |
| `OdiGponAlarmCritical` | critical | `los`, `lof` or `lom` asserted for 1 min |
| `OdiGponAlarm` | warning | any other `gpon_alarm` (`sf`, `sd`, `tx_too_long`, `tx_mismatch`) asserted for 5 min |
| `OdiOmciNoServices` | critical | in O5 with `gpon_omci_services == 0` for 5 min: ranged but not provisioned, the wrong-identity case |
| `OdiOmciServicesDropped` | warning | fewer services than the 6-hour maximum, but not zero, for 15 min |
| `OdiPortReceiveErrors` | warning | more than 10 malformed frames (CRC, fragment, jabber, undersize) in 15 min on one port, for 15 min |

The exporter has **no BIP, FEC or HEC counters**: the ONU MAC counters that
carry them are read-and-clear, and FEC performance monitoring (OMCI ME 312)
has no instance unless the OLT creates one (METRICS.md, "Deliberately not
exported"). `sf` and `sd` stand in: they are the downstream bit-error-rate
thresholds, signal fail and signal degrade. `OdiPortReceiveErrors` covers the
Ethernet side of both ports.

### Optics

Absolute thresholds are the ITU-T G.984.2 class B+ ONU limits, which is what
a GPON stick like this one is sold as; change them if your OLT link budget is
a different class.

| alert | severity | fires when |
|---|---|---|
| `OdiRxPowerLow` | warning | rx power below -25 dBm for 10 min (2 dB above the -27 dBm sensitivity) |
| `OdiRxPowerCritical` | critical | rx power below -27 dBm or above -8 dBm (overload) for 5 min |
| `OdiRxPowerDropped` | warning | the 1-hour mean of rx power is more than 2 dB below the 7-day mean, for 1 h |
| `OdiTxPowerOutOfRange` | warning | tx power outside +0.5 to +5 dBm for 15 min |
| `OdiLaserBiasRising` | warning | the 1-hour mean of bias current is more than 25% above the 7-day mean, for 6 h |
| `OdiModuleTemperatureHigh` | warning | above 70 C (the commercial SFP range) for 10 min |
| `OdiModuleTemperatureCritical` | critical | above 80 C for 5 min |
| `OdiSupplyVoltageOutOfRange` | warning | outside 3.3 V +-5% for 10 min |

**The ageing laser.** The transmitter runs under automatic power control, so
as the laser ages the driver needs more bias current for the same light:
bias drifts up while tx power stays put, and tx power only falls once the
driver runs out of headroom. `OdiLaserBiasRising` compares the stick with its
own last week rather than with a fixed number, because the bias of a healthy
laser differs from one unit to the next. Bias also follows temperature, which
is why both sides are averaged (1 h against 7 d) and the rise has to hold for
6 h, longer than a daily heat cycle. A slow drift over months moves the 7-day
baseline with it and does not fire; plot `gpon_bias_current_ma` over months
for that, next to `gpon_temperature_celsius`. With less than 7 days of data,
the baseline is whatever there is.

`OdiRxPowerDropped` is the same idea for the receive side: a connector, a
splice or a bend getting worse shows as a step against the stick own week
long before it crosses an absolute limit.

### System

| alert | severity | fires when |
|---|---|---|
| `OdiWatchdogReset` | warning | the previous boot ended in `wdt_*`, `panic` or `oops` (`gpon_last_reset_reason`), for the first hour after the boot |
| `OdiResetLoop` | critical | `gpon_boot_count` rose by 3 or more within an hour |
| `OdiRebooted` | info | uptime under 10 min: any reboot, asked for or not |
| `OdiMemoryLow` | warning | free + buffers + cached under 4 MiB for 10 min |
| `OdiUncommittedImage` | warning | `gpon_uncommitted` is 1 for 30 min: a trial boot nobody committed, so the next reset boots the other slot |

**Depends on `gpon_last_reset_reason` and `gpon_boot_count`**, which come
from odi-oss `/proc/odi_ramlog_prev` and need both an exporter release that
exports them and an odi-oss image whose kernel has the ramlog. Without them
`OdiWatchdogReset` and `OdiResetLoop` never fire; the other rules are
unaffected. A power cycle clears the ramlog, so `gpon_boot_count` restarts at
1 and a power cycle is never reported as a reset.

`OdiMemoryLow` is a margin above the odi-oss kernel watchdog memory floor,
which resets the stick when `MemAvailable` stays under 2048 KB.
`MemAvailable` is not exported; free + buffers + cached is the closest upper
bound.

`OdiUncommittedImage` is a reminder, not a fault: odi-oss never commits a
trial by itself, so a trial that has been checked and is meant to stay is
committed by hand on the stick (`nv commit <slot>`, then `slot-state.sh` to
refresh the metric). It **depends on `gpon_uncommitted`**, which needs an
exporter release that exports it and an odi-oss image that writes
`/var/run/odi-slot`; without both it never fires.

### Config

| alert | severity | fires when |
|---|---|---|
| `OdiConfigChanged` | warning | a config-store file got a new hash (two `gpon_config_info` series for one `file` within 30 min); clears 30 min later |
| `OdiConfigFileMissing` | critical | `lastgood.xml` or `lastgood_hs.xml` was there within the last day and is gone for 5 min while the exporter answers |

**Depends on `gpon_config_info` and `gpon_config_mtime_seconds`** (an
exporter release that exports them). `OdiConfigChanged` fires for a
deliberate save or restore as well: the question it asks is whether anyone
meant it. odi-oss `tools/config-backup.sh` keeps a copy of the config per
change to compare against.

## Changing a threshold

Edit `prometheus/alerts.yml`, adjust the matching case in
`prometheus/alerts_test.yml` (the test asserts the rendered annotations, so a
changed text or threshold shows there), and run `make rules`.
