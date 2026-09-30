# AGENTS.md

Guidance for any coding agent (or human) working in this repository. Read
the top-level `README.md` first for what this project is, then
`docs/DESIGN.md` for why it is built the way it is; this file is about how to
work in it safely and correctly.

## What this is, and how it fits with the firmware image

`odi-sfp-exporter` builds `metricsd`, a Prometheus exporter that runs **on** an
RTL9601-based GPON SFP ONU stick and serves optics and forwarding metrics on
its own HTTP port. It is a standalone, freestanding binary with no
dependencies of its own.

It does not build a flashable firmware image. It is part of the
[odi-oss](https://github.com/AndreZiviani/odi-oss) project, which consumes
this repo's **releases** (the `metricsd` binary plus `SHA256SUMS`, verified
before use) rather than its source, and bakes them into a custom image
alongside the stock vendor firmware and a config UI. If you are looking for
image-building, flashing, or device-provisioning logic, it lives in that
other repo, not here.

## Layout

    src/metricsd.c        the exporter: socket, accept loop, HTTP response
    src/metrics_body.h    the metrics themselves; every metric name lives here,
                          except the gpon_provision_* families
    src/slot_state.h      the boot-slot metrics from odi-oss /var/run/odi-slot; pure, host-tested
    src/provision.h       gpon_provision_*: what the OLT provisioned, pure text in and out
    src/wrap.h            octet-counter 64-bit wraparound extension; no MIPS-specific code
    src/confighash.h      config-store files, stat comparison, md5sum parse; no MIPS-specific code
    src/syscall.h         o32 syscall layer, file reads, fork/exec, decimals
    src/start.S           _start and a 6-argument syscall stub for setsockopt
    test/test_wrap.c      host-native unit test for src/wrap.h (`make test`)
    test/test_confighash.c  host-native unit test for src/confighash.h (`make test`)
    test/test_slot_state.c host-native unit test for src/slot_state.h (`make test`)
    test/test_metrics.sh  fixture check on the mib_* tables in metrics_body.h
    test/test_provision.c host-native test for src/provision.h against test/fixtures, with goldens
    scripts/verify.sh     asserts ELF32 / big endian / MIPS / static / no INTERP
    scripts/toolchain-image.sh  prints (and pulls) the pinned toolchain image
    scripts/deploy.sh     push a file to the stick over netcat
    stick/exporter-up.sh  on-device start/stop
    toolchain.env         the toolchain image (odi-toolchain freestanding), pinned by digest
    docs/BUILDING.md      the toolchain image: pulling, building it locally, release targets
    docs/DESIGN.md        why it is built this way, implementation notes, install, porting
    docs/METRICS.md       full metric reference and caveats
    docs/ALERTS.md        the alerting rules: what each alert means, thresholds, dependencies
    prometheus/alerts.yml       Prometheus alerting rules on these metrics
    prometheus/alerts_test.yml  promtool unit tests for them (`make rules`)
    grafana/gpon-stats.json     Grafana dashboard (v2 resource) for these metrics
    Makefile               every target below; re-enters itself with IN_CONTAINER=1
    .github/workflows/release.yml   build + gate on every push, publish on v* tags

## Build and test commands

    make image      # pull the pinned toolchain image (once; docs/BUILDING.md)
    make httpd      # build build/metricsd (what CI runs)
    make verify      # ELF shape: ELF32, big-endian MIPS, static, no INTERP
    make isa         # instruction census against the RLX5281's confirmed ISA
    make run ARGS=... # run a built binary under qemu-user (proves logic/syscalls,
                       # NOT instruction legality -- qemu emulates full MIPS32)
    make test         # host-native: the pure-header unit tests + mib_* table fixture check
    make rules        # promtool check + unit tests of prometheus/alerts.yml (Docker)
    make all          # httpd + verify + isa + test -- what you almost always want
    make release      # httpd + verify + isa + sums -- exactly what CI runs on a tag
    make sums         # SHA256SUMS over build/metricsd
    make clean

`make test` is host-native (plain `cc`, no Docker, no qemu-user) — it covers
`src/wrap.h`, `src/resetinfo.h`, `src/confighash.h`, `src/slot_state.h`,
`src/provision.h` and the `mib_*` tables in `src/metrics_body.h`, which have
no MIPS-specific code. Everything else is still gated by `make all` (or
`make release`), run on every push and PR via `.github/workflows/release.yml`,
not only on tags.

## Changelog discipline

Every change that affects users, the build, or the docs adds an entry under
`## Unreleased` in `CHANGELOG.md`, in the same commit as the change itself. A
release moves `## Unreleased` into a version section named after the tag.

## Release process

Every push and PR builds and gates the binary (`make httpd`, `make verify`,
`make isa`, `make sums`) so a tag can never fail on something an ordinary
commit would have caught. Only a `v*` tag publishes a GitHub release, with
`metricsd`, `SHA256SUMS`, and `stick/exporter-up.sh` as assets. `BUILD_ID` is
`git describe --tags --always --dirty`, computed on the host (the toolchain
container has no git history) and compiled in as `gpon_exporter_build_info` --
this is how a stick reports drift between what an image manifest claims and
what is actually running. A shallow checkout breaks this silently by making
every tag describe as `unknown`; CI always fetches full history.

## Coding rules

- **Freestanding C, no libc, no dependencies.** `-nostdlib -nostartfiles
  -static`, one hand-written `_start` in `src/start.S`, raw syscalls via
  `src/syscall.h`. There is no libc worth linking against on this device (a
  2009 uClibc build); going freestanding removes the ISA-compatibility
  question entirely rather than trying to satisfy it.
- **Big-endian MIPS-I, targeting a specific trapping core.** The RLX5281
  core implements the MIPS instruction set in pieces: `movz`/`movn`/`ll`/
  `sc`/`sync`/`bltzl`/`madd` are confirmed to work; `mul`, `clz` (and the
  rest of the SPECIAL2 class), `teq`/`tne`/`tge`/`tlt`, and `beql`/`bnel`
  are confirmed illegal and raise SIGILL. Every build uses `-march=mips1
  -mabi=32 -EB -msoft-float -G0 -fno-pic -mno-abicalls -ffreestanding
  -fno-builtin -fno-stack-protector`. Do not raise `-march`, add FPU code,
  or assume a generic MIPS32/MIPS-II toolchain default is safe here --
  `mul` and `clz` are the two most common ways a normal C compile silently
  becomes unrunnable on this hardware.
  - **Go and TinyGo cannot target this core**, and no compiler flag fixes
    it: the runtime library sets the ISA floor, not the compiler's code
    generation flag. Do not propose porting this to Go.
  - `isa-audit` and `isa-allowlist`, shared with the other RLX5281 projects
    and installed in the toolchain image (odi-toolchain), are the actual
    gate, run by `make isa` and by CI: the binary fails if any
    confirmed-illegal mnemonic or floating point appears, and anything
    unverified is printed as UNVERIFIED (a CI warning, not a build failure).
    Run it, do not just trust `-march`. The allowlist grows in odi-toolchain,
    and only by executing an instruction on a device.
  - `scripts/verify.sh` checks the ELF shape a dynamic or non-static binary
    would fail on the device: ELF32, big-endian, MIPS, no `PT_INTERP`, no
    `NEEDED` shared libraries.
- **MIPS syscall/ABI constants differ from the generic Linux ones you may
  remember from x86 or ARM**: `SOCK_STREAM`/`SOCK_DGRAM` are swapped,
  `SOL_SOCKET` is `65535`, `O_CREAT` is `0x100`, and o32 syscall numbers are
  offset from a base of 4000. A wrong constant here is a syscall that
  "succeeds" at doing the wrong thing, not one that fails loudly. Look up
  the actual target header rather than porting a constant from memory.
- **No apostrophes in shell-script comments.** A single quote inside a
  single-quoted inline block (e.g. `bash -c '...'`) silently terminates the
  shell word and runs the rest of the line in the outer shell. Rephrase;
  do not escape.
- Values read from `diag` are emitted as the literal text it printed, never
  parsed to a number and back -- see "Implementation notes" in
  `docs/DESIGN.md` before changing anything in the metric-formatting path.
- **Every wait on another process is bounded.** This is single-threaded and
  serves the socket, so anything that blocks on a child blocks the whole
  exporter (hardware trial rc3, claro, 2026-09-28: a stuck omcid parked
  `run_to_buf()`'s `read()` forever and metricsd stopped accepting
  connections entirely, though it was otherwise fine). `run_to_buf()` and
  `run_script_to_buf()` (`src/syscall.h`) take a `timeout_ms` and poll the
  child's pipe rather than block on it, SIGKILL + reap on expiry
  (`drain_bounded()`/`kill_and_reap()`); every new caller must pass a real
  bound (`OMCICLI_TIMEOUT_MS`, `DIAG_TIMEOUT_MS` in `src/metrics_body.h`) and
  report the failure as a metric (`gpon_omci_up`, `gpon_diag_up`) rather than
  going silent. There is no libc `alarm()`/`select()` here -- this is
  `poll(2)` over the raw o32 syscall layer, which has no MIPS-specific
  divergence (unlike the socket/IPC calls elsewhere in `src/syscall.h`).

## Testing on a stick safely

- **Copy to `/tmp`, never overwrite a running binary in place.**
  `scripts/deploy.sh` pushes over a netcat pipe (the device cannot open
  connections outward, so it listens and this host connects) and lands the
  file in `/tmp`, which is tmpfs: a bad binary cannot brick the device and
  does not survive a reboot. `stick/exporter-up.sh` starts/stops it from
  there.
- The rootfs is read-only squashfs and the daemon has no installer of its
  own; there is no in-place replacement to accidentally perform on this
  binary, but keep that same discipline (temp location, verify, then run)
  for anything else you push alongside it.
- This repo does not flash firmware and has no `sw_tryactive`/`sw_commit`
  logic; if your task involves writing to a flash partition or a boot slot,
  that is out of scope here.

## Dangerous commands on the stick

These are properties of the device's other userland tools, not of this
exporter, but anyone testing on real hardware needs to know them because
they are easy to trigger by accident while poking around:

- **`omcicli get tables` wedges the OMCI daemon.** Do not run it against a
  device you need to stay provisioned.
- **`diag`, read from a stdin that never closes, spins at 100% CPU.** Any
  script or shell that pipes to `diag` and leaves stdin open (no EOF, no
  command) will peg the CPU. Always give it a way to see EOF, or wrap the
  invocation in a timeout.
- **Reading an undecoded SoC register address with `devmem` can stall the
  bus until the hardware watchdog resets the device.** Do not probe
  addresses you cannot already account for.

## Release checklist

A release is cut only when the maintainer asks for one. Before tagging:

1. **Every change since the last tag is in the changelog.** Walk
   `git log --oneline <last-tag>..origin/main` and check that each merged
   change has its entry under `## Unreleased`. Add any that are missing in
   the release commit.
2. **No merge debris.** `grep -nE '^(<<<<<<<|=======|>>>>>>>)' CHANGELOG.md`
   finds nothing.
3. **Move `## Unreleased` into the version section** named after the tag,
   with the date, and leave an empty `## Unreleased` above it. Commit it as
   `CHANGELOG: <tag>`, then tag that commit (`git tag -s`).
4. **Check the published release** (`gh release view <tag>`): the assets are
   there and the notes are the new section, not an empty one.
