#!/bin/sh
set -eu

# Boots the just-built kernel under QEMU (-M spitz -- the closest thing to
# real Corgi/Husky hardware QEMU can emulate, see README.md's "Launching
# QEMU" section) with the *actual* update.tar package about to ship, then
# inside that boot: insmod's every kernel module the package contains and
# runs `piko-update --dry-run` against the real package -- the same
# regression class that actually broke this device once already
# (DEADLETTER-WIFI-SSH.md: a kernel redeployed without matching modules,
# "section size must match" at insmod time, device unreachable). A package
# that fails this never gets uploaded.
#
# The initramfs PID 1 is userspace/src/piko-smoke-init.c, a small
# dependency-free static binary (same reasoning as md5sum.c's own "no
# busybox applet for this" rationale) -- it mounts proc/sysfs/devtmpfs,
# mounts a real SD card at /tmp (see below), insmod's every *.ko it finds
# by reading the shipped /update.tar directly (no separate loose-extracted
# copy under /lib/modules -- see its own header comment on why that
# duplication used to tip this test over the guest's fixed 64M as the
# package grew), execs piko-update --dry-run against that same
# /update.tar, and prints one greppable PASS/FAIL line. It never touches
# the rest of the shipped rootfs overlay (etc/*, the other usr/sbin/*
# scripts, or the package's own top-level "init" -- note that path
# collides with this script's own /init if the whole tar were extracted
# naively, which is why only usr/sbin/piko-update and usr/sbin/piko-smf-write
# are pulled out of it loose, not the full archive).
#
# Why a real SD card: piko-update's own --dry-run stages a full verify
# copy of every shipped file under /tmp before touching anything live (see
# piko-update.c's safety-model comment) -- intrinsic to how it actually
# works, not something to design around. On real hardware /tmp lives on
# the flash-backed "home" partition, with far more room than this guest's
# fixed 64M of RAM (see the qemu-system-arm invocation below for why that
# ceiling can't be raised). Piling the whole package into a RAM-backed
# initramfs and hoping /tmp's transient copy also fits kept tipping over
# as the package grew (hit this twice already, even after real fixes each
# time), so instead of shrinking the package further, this gives /tmp
# actual room: a small SD card image, formatted vfat (CONFIG_MMC/
# CONFIG_MMC_BLOCK/CONFIG_MMC_PXA/CONFIG_VFAT_FS are all built into the
# kernel, confirmed via kernel.config-corgi-7.1.4, so there's no module to
# insmod for this -- it's live from boot) -- exactly how piko-update is
# actually used on real hardware (this script's own header, and
# build-update-package.sh's: "copy the resulting update.tar to an SD card
# and run piko-update /mnt/card/update.tar").
#
# Usage:
#   flash/qemu-smoke-test.sh <kernel_dir> <update_tar>
# Example:
#   flash/qemu-smoke-test.sh kernel-src/linux-7.1.4 update.tar
#
# IMPORTANT CAVEAT: QEMU's spitz machine is PXA270, not this project's real
# PXA255 Corgi/Husky target, and has no W100 display chip -- it never sets
# machine_is_corgi()/machine_is_husky() true, so corgi.c/corgi_pm.c/
# w100fb.c themselves never execute here (see README.md's own caveat on
# this from the original QEMU bring-up). This test validates the *shared*
# PXA2xx boot path, module loading, and the shipped package's integrity --
# it is not a substitute for real hardware.
#
# Also: to boot under QEMU at all, the kernel needs a *different* .config
# than the one that ships (MACH_SPITZ enabled, ttyS0 console, no
# CONFIG_CMDLINE_FORCE -- see README.md "Launching QEMU"). This script
# builds that variant kernel-side (backing up and restoring the real
# .config around it) but reuses the modules already built from the real
# .config unchanged -- flipping the machine-select/cmdline options this
# script touches doesn't affect module ABI (kernel release string, struct
# layouts, EXPORT_SYMBOL versions), so the same .ko files should still
# insmod cleanly. That assumption is the whole point of this test to begin
# with: if it's wrong, this is exactly the kind of mismatch it should catch.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_DIR="$1"
UPDATE_TAR="$2"
QEMU_TIMEOUT="${QEMU_TIMEOUT:-90}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabi-}"

STAGE="$(mktemp -d /tmp/piko-qemu-smoke.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

echo "==> cross-compiling userspace/src/piko-smoke-init.c"
GCC="${CROSS_COMPILE}gcc"
if ! command -v "$GCC" >/dev/null 2>&1; then
    echo "flash/qemu-smoke-test.sh: no working cross-compiler found ($GCC)" >&2
    exit 1
fi
"$GCC" -march=armv5te -O2 -static -Wall -Wextra \
    -o "$STAGE/piko-smoke-init" "$REPO/userspace/src/piko-smoke-init.c"

echo "==> building a QEMU-bootable kernel variant (MACH_SPITZ, ttyS0 console)"
CONFIG="$KERNEL_DIR/.config"
cp "$CONFIG" "$STAGE/config.real-device"
(
    cd "$KERNEL_DIR"
    ./scripts/config --enable MACH_SPITZ \
        --set-str CMDLINE "console=ttyS0 earlyprintk panic=1" \
        --disable CMDLINE_FORCE
    yes "" | make ARCH=arm olddefconfig >/tmp/piko-qemu-oldconfig.log 2>&1
    make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" \
        -j"$(nproc)" zImage >/tmp/piko-qemu-zimage-build.log 2>&1
)
cp "$KERNEL_DIR/arch/arm/boot/zImage" "$STAGE/zImage-qemu-variant"
# Restore the real-device .config immediately -- this tree is cached
# (actions/cache) across CI runs, so it must not be left in the
# QEMU-flavored state for next time.
cp "$STAGE/config.real-device" "$CONFIG"

echo "==> assembling smoke-test initramfs (piko-smoke-init + the actual update.tar payload)"
mkdir -p "$STAGE/root/proc" "$STAGE/root/sys" "$STAGE/root/usr/sbin" "$STAGE/root/tmp"
cp "$STAGE/piko-smoke-init" "$STAGE/root/init"
chmod 755 "$STAGE/root/init"

# Pull only what's actually exercised out of the tar -- NOT the whole
# archive: the package's own top-level "init" (rootfs/init, meant for the
# real device) would otherwise land at this initramfs's /init too and
# silently clobber piko-smoke-init. Modules are NOT extracted here --
# piko-smoke-init reads them straight out of update.tar itself (see its
# own header comment), so only the two binaries that actually need to be
# executable files (piko-update, piko-smf-write) get pulled loose.
tar xf "$UPDATE_TAR" -C "$STAGE/root" usr/sbin/piko-update
chmod 755 "$STAGE/root/usr/sbin/piko-update"
# piko-update execs this for smf/NAND work; the smoke test checks it ships
# and that the paths using it stay inert with no NAND present.
tar xf "$UPDATE_TAR" -C "$STAGE/root" usr/sbin/piko-smf-write
chmod 755 "$STAGE/root/usr/sbin/piko-smf-write"
cp "$UPDATE_TAR" "$STAGE/root/update.tar"

echo "==> staged initramfs tree (host side, before cpio archival):"
ls -la "$STAGE/root/usr/sbin/piko-update" "$STAGE/root/usr/sbin/piko-smf-write" "$STAGE/root/update.tar" 2>&1
echo "==> total staged size (this is what has to fit in the guest's fixed 64M):"
du -sb "$STAGE/root" 2>&1

( cd "$STAGE/root" && find . -mindepth 1 | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$STAGE/initramfs.cpio.gz"

echo "==> initramfs contents after cpio archival (re-read back, host side):"
zcat "$STAGE/initramfs.cpio.gz" | cpio -tv 2>&1 | grep -E 'piko-update|piko-smf-write|update\.tar'

echo "==> creating the SD card image piko-smoke-init mounts at /tmp"
# 64M is comfortably more than the package (a few MB) needs for a full
# second staged copy plus filesystem overhead, and small enough to format
# in well under a second.
SD_IMG="$STAGE/sdcard.img"
dd if=/dev/zero of="$SD_IMG" bs=1M count=64 status=none
mkfs.vfat "$SD_IMG" >/dev/null

echo "==> booting under qemu-system-arm -M spitz (timeout ${QEMU_TIMEOUT}s)"
LOG="$STAGE/boot.log"
# -M spitz hard-codes 64M of guest RAM (matching real Corgi/Husky hardware)
# and ignores -m entirely -- confirmed in CI: "Memory: 21160K/65536K
# available" was identical whether or not -m was passed. That ceiling is
# real and not worth fighting (build-update-package.sh already strips
# module debug info, and piko-smoke-init reads modules straight out of
# update.tar rather than needing a second loose-extracted copy -- see its
# own header comment); what changed here is giving /tmp its own storage
# (the -drive/-device pair below) instead of asking everything, including
# piko-update's own --dry-run staging copy, to fit in that same 64M.
timeout "$QEMU_TIMEOUT" qemu-system-arm -M spitz \
    -kernel "$STAGE/zImage-qemu-variant" \
    -initrd "$STAGE/initramfs.cpio.gz" \
    -drive id=sd0,if=none,file="$SD_IMG",format=raw \
    -device sd-card,drive=sd0 \
    -append "console=ttyS0 earlyprintk panic=1" \
    -serial stdio -nographic -monitor none -no-reboot \
    > "$LOG" 2>&1 || true

echo "==> boot log:"
cat "$LOG"

if grep -q "PIKO-SMOKE-TEST: PASS" "$LOG"; then
    echo "==> smoke test PASSED"
    exit 0
fi

echo "==> smoke test FAILED (no PASS marker found in boot log -- timeout, panic, or a check above failed)" >&2
exit 1
