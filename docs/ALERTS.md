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
| `OdiOnuNoUpstreamGrants` | critical | in O5 while `gpon_ds_bwmap_allocations_total` has not moved for 5 min (plus 2 min): the OLT stopped granting the ONU upstream time |
| `OdiFecUncorrectable` | warning | `gpon_ds_fec_codewords_total{result="uncorrectable"}` increasing for 10 min: data is lost on the fibre |
| `OdiOmciServicesDropped` | warning | fewer services than the 6-hour maximum, but not zero, for 15 min |
| `OdiPortReceiveErrors` | warning | more than 10 malformed frames (CRC, fragment, jabber, undersize) in 15 min on one port, for 15 min; port 3 (CPU) undersize excluded: it is the OMCI replies (METRICS.md, "Known caveats") |

`sf` and `sd` are signal fail and signal degrade, raised by the ONU when the
downstream bit error rate crosses the thresholds the OLT set (G.984.3
11.1.3). Only the stock firmware reports them; odi-oss reports `los`, `lof`
and `lom`, so `OdiGponAlarm` cannot fire there. The line counters from odi-oss (METRICS.md, "The GPON
link") add what the ONU sees itself: `OdiOnuNoUpstreamGrants` is the stuck
ONU, and `OdiFecUncorrectable` is data lost on the fibre. Corrected FEC
codewords and BIP errors are on the dashboard rather than alerted on, since a
weak line corrects thousands a second without losing data. These two need an
odi-oss kernel with the counters; on any other the series are absent and the
rules stay silent. `OdiPortReceiveErrors` covers the Ethernet side of both
ports.

### Optics

The absolute thresholds below are the ITU-T G.984.2 class B+ ONU limits
(sensitivity is quoted as -27 dBm here and -28 dBm elsewhere; not checked
against the standard), which is what a GPON stick like this one is sold as,
not the limits of the module in your stick: the module stores its own (A2h
bytes 0-39) and raises its own flags against them, which the last three
alerts below follow. A line a stick works on below -27 dBm keeps the absolute
alerts firing; silence those and keep `OdiTransceiverAlarm`. Change the
numbers if your link budget is a different class.

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
| `OdiTransceiverAlarm` | warning | the module itself raises an alarm flag (`gpon_transceiver_alarm{kind="alarm"}`), for 10 min |
| `OdiTransceiverWarning` | info | the module raises a warning flag, its inner limit, for 30 min |
| `OdiTransceiverUnreadable` | warning | `gpon_transceiver_up` is 0 for 5 min while diag answers: the optical readings are missing and the optics alerts cannot fire |

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
| `OdiSafeModeBoot` | critical | `gpon_safe_mode` is 1 for 1 min: a safe-mode boot, the PON off and the default password in force for that boot |
| `OdiSafeModeUnacknowledged` | warning | a safe-mode record waits for 30 min after a normal boot: it may have cleared `HOST_AUTONEG` |
| `OdiHostLinkFallback` | warning | `gpon_host_autoneg_fallback` is 1 for 5 min: `HOST_AUTONEG` is off but the host port runs auto-negotiation this boot |

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

`OdiSafeModeBoot` is critical because the stick carries no service on that
boot (its PON is off) and accepts the default password, whether someone
power-cycled it on purpose or its power is failing; a reboot returns it to
the saved configuration. `OdiSafeModeUnacknowledged` is the reminder after
it: the record stays until acknowledged on the web UI banner, and the safe
mode may have cleared `HOST_AUTONEG`. `OdiHostLinkFallback` means the stick
is reachable but not in the host port mode it was set to. All three
**depend on `gpon_safe_mode`, `gpon_safe_mode_unacknowledged` and
`gpon_host_autoneg_fallback`**, which need an exporter release that exports
them and an odi-oss image with `HOST_AUTONEG` and the safe mode; without
both they never fire.

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

## Grafana Cloud and Slack

Grafana can import the file as Grafana-managed rules (Alerting > Alert rules >
Import to Grafana-managed rules, source "Prometheus YAML file", data source
the one the sticks are scraped into). Three things to set after the import:

- **Execution error state.** The import sets both the no-data and the error
  state to OK, so a query that fails (a data source hiccup) resolves a
  firing alert. Set the error state to **Keep last state** on every rule
  except the five in `odi-exporter`, and to **Error** on those five: a
  broken data path then sends one warning instead of resolving everything.
  Leave no data at OK; a stick that stops answering loses its series, and
  `OdiExporterDown` covers that.
- **Images.** Grafana attaches a panel screenshot to a notification only for
  a rule linked to a panel (annotations `__dashboardUid__` and
  `__panelId__`). Link each rule to the panel in
  [`grafana/gpon-stats.json`](../grafana/gpon-stats.json) that shows its
  problem:

  | rules | panel |
  |---|---|
  | OdiOnuNotOperational | 206 ONU state over time |
  | OdiGponAlarmCritical, OdiGponAlarm | 102 GPON Alarms by type |
  | OdiOmciNoServices, OdiOmciServicesDropped | 201 OMCI Services |
  | OdiOmciDown | 643 OMCI frames/s |
  | OdiOnuNoUpstreamGrants | 664 Upstream grants/s |
  | OdiFecUncorrectable | 663 Line errors/s |
  | OdiPortReceiveErrors | 106 Port Hardware Receive Errors |
  | OdiRxPower*, OdiTxPowerOutOfRange | 9 PON Optical Power Trend |
  | OdiLaserBiasRising | 5 Bias Current |
  | OdiModuleTemperature* | 1 Transceiver Temperature |
  | OdiSupplyVoltageOutOfRange | 2 SFP voltage |
  | OdiWatchdogReset, OdiRebooted | 202 Uptime |
  | OdiResetLoop | 203 Reboots (range) |
  | OdiMemoryLow | 500 Memory |
  | OdiUncommittedImage | 204 Image / Firmware / Exporter |
  | OdiSafeModeBoot | 721 Safe-mode boot |
  | OdiSafeModeUnacknowledged | 722 Safe-mode record unacknowledged |
  | OdiHostLinkFallback | 713 Host port: fallback, trial and boot watch |
  | odi-exporter group | 205 Exporter Health |

  The Slack contact point needs a bot token (not a webhook URL) for Grafana
  to upload the image.
- **Message format.** [`grafana/slack.tmpl`](../grafana/slack.tmpl) is a
  notification template (Alerting > Contact points > Notification
  templates); set the Slack contact point Title to
  `{{ template "slack.title" . }}` and Text to `{{ template "slack.text" . }}`.
  Add `instance` to the notification policy group_by so each stick gets its
  own message. The template is not ODI-specific: any alert gets a marker by
  severity, its target (instance, host or job), the summary, the
  description, and Rule / Silence / Dashboard links; an alert whose rule
  could not be evaluated shows as a warning, never as resolved.

## Changing a threshold

Edit `prometheus/alerts.yml`, adjust the matching case in
`prometheus/alerts_test.yml` (the test asserts the rendered annotations, so a
changed text or threshold shows there), and run `make rules`.
