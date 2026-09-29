# odi-sfp-exporter — Prometheus exporter for RTL9601-based GPON SFP ONU sticks.
#
# Host targets shell out to the toolchain container; the C build re-enters
# this Makefile there with IN_CONTAINER=1.
#
# The container is the freestanding toolchain image from odi-toolchain,
# pinned by digest in toolchain.env and pulled on first use;
# TOOLCHAIN_IMAGE overrides it (scripts/toolchain-image.sh, docs/BUILDING.md).
include toolchain.env
TOOLCHAIN_IMAGE ?= $(TOOLCHAIN_IMAGE_PINNED)
IMAGE   := $(TOOLCHAIN_IMAGE)
BUILD   := build

# Stick connection, for `make deploy` / `make pull`. IP is deliberately unset so
# a bare `make deploy` only prints the recipe instead of talking to the network.
# The stick cannot open connections to us, so it listens and this host
# connects.
IP        ?=
PORT      ?= 12345

# Which binary the one-off targets act on.
BIN  ?= $(BUILD)/metricsd

# Baked into the binary and exported as gpon_exporter_build_info. It exists
# because the exporter can be updated WITHOUT reflashing — rc35 prefers
# /etc/config/metricsd, on the jffs2 config partition, over the /bin/metricsd
# in the image — so an override can outlive the image it was built against with
# nothing in the metrics to say so. Computed on the host: the toolchain
# container has no git and no repo history.
BUILD_ID ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo unknown)

ifeq ($(IN_CONTAINER),1)

# ---------------------------------------------------------------- in container

CROSS   := mips-linux-gnu-
CC      := $(CROSS)gcc
STRIP   := $(CROSS)strip

# -march=mips1     RLX5281 is MIPS-I class
# -EB              big-endian, per CONFIG_CPU_BIG_ENDIAN in Realtek's 9601b config
# -msoft-float     RLX cores have no FPU
# -G0              no gp-relative small data — _start never sets up $gp
# -fno-pic
# -mno-abicalls    plain static ELF, no GOT
# -ffreestanding   no libc, and no turning our loops back into libc calls
CFLAGS  := -std=c99 -Os -Wall -Wextra \
           -march=mips1 -mabi=32 -EB -msoft-float -G0 \
           -fno-pic -mno-abicalls -ffreestanding -fno-builtin -fno-stack-protector

# Passed in from the host target; the header defaults it if absent so a bare
# in-container build still compiles.
BUILD_ID ?=
ifneq ($(BUILD_ID),)
CFLAGS  += -DBUILD_ID='"$(BUILD_ID)"'
endif

LDFLAGS := -nostdlib -nostartfiles -static -Wl,-e,_start -Wl,--build-id=none

HDRS := src/syscall.h src/metrics_body.h src/resetinfo.h src/confighash.h

# BUILD_ID is compiled in, but it is a make VARIABLE -- make cannot see it
# change, so with the sources untouched it will not rebuild and the binary keeps
# whatever stamp it was last compiled with.
#
# That shipped: an image was built whose manifest said exporter
# v1.0.1-7-gb4d4e9f while /bin/metricsd inside it reported
# v1.0.1-6-g60083e9-dirty. The code was current; only the stamp was stale, and
# the manifest -- whose entire job is saying what is on the stick -- was wrong.
# gpon_exporter_build_info caught it on the first boot after flashing, which is
# exactly why that metric exists.
#
# Park the value in a file and depend on the file. FORCE makes the recipe run
# every time; `cmp` means the file is only rewritten, and the mtime only moves,
# when the value actually differs.
.PHONY: FORCE
$(BUILD)/.build-id: FORCE | $(BUILD)
	@printf '%s' '$(BUILD_ID)' | cmp -s - $@ 2>/dev/null || printf '%s' '$(BUILD_ID)' > $@

$(BUILD)/metricsd: src/start.S src/metricsd.c $(HDRS) $(BUILD)/.build-id | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(filter %.S %.c,$^)
	$(STRIP) $@

$(BUILD):
	mkdir -p $@

else

# --------------------------------------------------------------------- on host

RUN   := docker run --rm -v "$(CURDIR)":/src -w /src $(IMAGE)

.PHONY: all httpd image verify isa test run deploy shell clean release sums

# What you almost always want: the exporter, checked.
all: httpd verify isa test

image:
	@TOOLCHAIN_IMAGE='$(IMAGE)' scripts/toolchain-image.sh >/dev/null

## --- targets -----------------------------------------------------------------

# The Prometheus exporter, standalone HTTP server. This is the one that works
# on the ODI stick — Realtek's boa has no external-CGI path.
httpd: image
	$(RUN) make IN_CONTAINER=1 BUILD_ID='$(BUILD_ID)' $(BUILD)/metricsd

## --- inspection --------------------------------------------------------------

# src/wrap.h (the octet-counter wraparound logic), src/resetinfo.h (the
# /proc/odi_ramlog_prev parse), src/confighash.h (the md5sum parse and stat
# comparison behind gpon_config_info) and the mib_* tables in
# src/metrics_body.h have no MIPS-specific code, so they are tested with the
# HOST compiler -- no Docker, no qemu-user. test_wrap.c, test_resetinfo.c and
# test_confighash.c are real unit tests; test_metrics.sh is a fixture check on
# the mib_* tables themselves, since
# there is no host binary to point a fake diag output at.
# A temp file, not $(BUILD)/test_wrap: on CI $(BUILD) is created by `make
# httpd`'s container as root (see the `sums` comment below), so a host-side
# write into it fails with EACCES on Linux runners -- the same trap that bit
# `sums` once already.
test:
	@for u in test_wrap test_resetinfo test_confighash; do \
		t=$$(mktemp); \
		$(CC) -std=c99 -Wall -Wextra -o "$$t" test/$$u.c; rc=$$?; \
		if [ $$rc -eq 0 ]; then "$$t"; rc=$$?; fi; \
		rm -f "$$t"; \
		[ $$rc -eq 0 ] || exit $$rc; \
	done
	sh test/test_metrics.sh

verify: image
	$(RUN) scripts/verify.sh $(BIN)

# The ISA gate, shared with the other RLX5281 projects and installed in the
# toolchain image: isa-audit refuses an instruction known to trap (or any
# floating point); isa-allowlist reports any mnemonic never executed on the
# hardware. A trap fails the target. An unverified mnemonic (isa-allowlist
# exit 2) is printed as UNVERIFIED and does not, since the fix is to execute
# it on a device and extend the list in odi-toolchain -- CI turns that line
# into a warning.
isa: image
	$(RUN) isa-audit $(BIN)
	@$(RUN) isa-allowlist $(BIN); rc=$$?; [ $$rc = 0 ] || [ $$rc = 2 ]

# Run a built binary under qemu-user. Proves logic and syscalls; does NOT prove
# instruction legality — qemu emulates full MIPS32 and will happily execute the
# `mul` and `clz` that trap on real hardware. Use `make isa` for that.
run: image
	@$(RUN) qemu-mips-static $(BIN) $(ARGS)

## --- moving files ------------------------------------------------------------

# make deploy BIN=build/metricsd IP=192.168.1.1
deploy:
	@scripts/deploy.sh $(BIN) $(if $(IP),$(IP) $(PORT))

# Exactly what the release workflow runs, so a tag cannot fail on something you
# could have caught locally.
#
# It runs the SAME targets rather than repeating their recipes, which is what
# made the claim above false: this target rebuilt metricsd with its own command
# line and left BUILD_ID out, so every binary `make release` produced reported
# `version="unknown"` while `make httpd` -- what CI actually runs -- stamped it
# correctly. A copy of a recipe is a copy that drifts.
release: httpd verify isa sums

# Written inside the container, not on the host. build/ is created by the
# container as root, so on Linux — every CI runner — the host user cannot write
# into it. On macOS Docker maps ownership and hides the problem, which is
# exactly how this reached CI.
sums: image
	$(RUN) sh -c 'cd $(BUILD) && sha256sum metricsd > SHA256SUMS && cat SHA256SUMS'

shell: image
	docker run --rm -it -v "$(CURDIR)":/src -w /src $(IMAGE) bash

clean:
	rm -rf $(BUILD)

endif
