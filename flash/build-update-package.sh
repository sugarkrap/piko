#!/bin/sh
set -eu

# Builds an offline update package for userspace/src/piko-update.c: a plain
# ustar tar (no compression -- piko-update reads it with its own from-
# scratch reader, no tar/gzip/unzip dependency on the device) containing
# a MANIFEST (md5 per shipped file) plus:
#
#   - boot/zImage-full + lib/modules/$KVER/...   (only if kernel-src is
#     available locally -- see "Kernel/modules" below)
#   - everything under rootfs/, mapped straight onto the same paths
#     under "/" (etc/*, usr/sbin/*, init -- whatever's actually committed
#     there is what gets shipped, so this can't drift from a hand-picked
#     file list the way two independent lists would)
#   - a freshly cross-compiled usr/sbin/piko-update itself (self-update)
#
# This is the offline counterpart to tools/chunked-deploy.sh (which pushes
# the same kind of update live over SSH). Use this one when the device
# isn't reachable over WiFi at all -- copy the resulting update.tar to an
# SD card and run `piko-update /mnt/card/update.tar` on the device.
#
# Usage:
#   flash/build-update-package.sh [output.tar]
#
# Env overrides (defaults match tools/build-and-deploy.sh):
#   KERNEL_DIR     kernel-src/linux-7.1.4 checkout (gitignored, local only)
#   TOOLCHAIN      directory holding CROSS_COMPILE-prefixed binaries
#   CROSS_COMPILE  cross toolchain prefix
#
# If KERNEL_DIR doesn't exist (e.g. in CI, which has no local buildroot/
# kernel-src checkout -- see docs/HOWTO-BUILD-DEPLOY-KERNEL.md), this
# script still produces a valid, useful package: piko-update itself plus
# the full rootfs/config overlay, just without a kernel bump. It prints
# which mode it ran in.

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$REPO/update.tar}"

KERNEL_DIR="${KERNEL_DIR:-$REPO/kernel-src/linux-7.1.4}"
TOOLCHAIN="${TOOLCHAIN:-/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-buildroot-linux-uclibcgnueabi-}"

STAGE="$(mktemp -d /tmp/piko-update-package.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

# manifest_add SRC_FILE DEST_REL_PATH [MODE]
# Copies SRC_FILE into the staging tree at DEST_REL_PATH (relative to "/"),
# records its md5, and appends a MANIFEST line. MODE defaults to SRC_FILE's
# own permission bits so scripts/binaries keep their exec bit automatically.
MANIFEST="$STAGE/MANIFEST"
FILES_LIST="$STAGE/.files"
: > "$FILES_LIST"

manifest_add() {
    src="$1"
    dest="$2"
    mode="${3:-}"

    if [ ! -f "$src" ]; then
        echo "build-update-package: missing input file: $src" >&2
        exit 1
    fi
    if [ -z "$mode" ]; then
        mode="$(stat -c '%a' "$src" 2>/dev/null || stat -f '%Lp' "$src")"
    fi

    stage_dest="$STAGE/payload/$dest"
    mkdir -p "$(dirname "$stage_dest")"
    cp "$src" "$stage_dest"
    chmod "$mode" "$stage_dest"

    md5="$(md5sum "$src" | cut -d' ' -f1)"
    echo "$md5 $dest" >> "$MANIFEST"
    echo "$dest" >> "$FILES_LIST"
}

mkdir -p "$STAGE/payload"

{
    echo "PIKO-UPDATE-PACKAGE 1"
    echo "# built $(date -u +%Y-%m-%dT%H:%M:%SZ) from $(cd "$REPO" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
} > "$MANIFEST"

echo "==> cross-compiling userspace/src/piko-update.c"
GCC="${TOOLCHAIN}/${CROSS_COMPILE}gcc"
if [ ! -x "$GCC" ]; then
    # Fall back to whatever CROSS_COMPILE resolves to on PATH (e.g. CI's
    # apt-installed gcc-arm-linux-gnueabi -- a static glibc binary runs
    # fine on this uclibc rootfs since there's no dynamic libc dependency
    # at all once it's linked -static).
    GCC="${CROSS_COMPILE}gcc"
fi
if ! command -v "$GCC" >/dev/null 2>&1; then
    echo "build-update-package: no working cross-compiler found ($GCC)" >&2
    echo "  set TOOLCHAIN/CROSS_COMPILE, or install one (e.g. gcc-arm-linux-gnueabi)" >&2
    exit 1
fi
"$GCC" -march=armv5te -O2 -static -Wall -Wextra \
    -o "$STAGE/piko-update" "$REPO/userspace/src/piko-update.c"
STRIP="${GCC%gcc}strip"
command -v "$STRIP" >/dev/null 2>&1 && "$STRIP" "$STAGE/piko-update" || true
manifest_add "$STAGE/piko-update" "usr/sbin/piko-update" 755

echo "==> packaging rootfs/ overlay (etc/, usr/sbin/, init -- whatever's there)"
( cd "$REPO/rootfs" && find . -type f ) | sed 's#^\./##' | while read -r rel; do
    manifest_add "$REPO/rootfs/$rel" "$rel"
done

# SSH file transfer (scp + sftp-server + dbclient/dropbearkey) from
# tools/build-ssh.sh. Separate from the rootfs/ loop above because these
# are cross-compiled binaries: this repo tracks source and rebuilds
# binaries, so they live in a staging tree rather than under rootfs/.
#
# Included in the offline update on purpose -- an update package is the
# path for a device that is NOT reachable over the network, which is
# exactly the device that most needs working file transfer next time.
#
# dropbear itself is opt-in (PIKO_SSH_REPLACE_DROPBEAR=1), same as
# tools/chunked-deploy.sh --replace-dropbear and the mtd3 image builder:
# piko-update rewrites files in place, so a bad server binary here would
# take SSH down on a board with no serial console (AGENTS.md).
SSH_STAGE="${SSH_STAGE:-$REPO/userspace/stage-ssh}"
if [ -d "$SSH_STAGE" ]; then
    echo "==> packaging SSH file transfer payload from $SSH_STAGE"
    . "$REPO/tools/ssh-payload.sh"
    SSH_LIST="$SSH_PAYLOAD_FILES"
    if [ "${PIKO_SSH_REPLACE_DROPBEAR:-0}" = "1" ]; then
        echo "==> PIKO_SSH_REPLACE_DROPBEAR=1: also packaging the rebuilt dropbear"
        SSH_LIST="$SSH_LIST
$SSH_PAYLOAD_SERVER"
    fi
    for entry in $SSH_LIST; do
        rest="${entry#*:}"
        manifest_add "$SSH_STAGE/${entry%%:*}" "${rest%%:*}" "${rest#*:}"
    done
else
    echo "==> no $SSH_STAGE -- package will have no scp/sftp-server"
    echo "    (run tools/build-ssh.sh first if that is not intended)"
fi

if [ -d "$KERNEL_DIR" ]; then
    echo "==> KERNEL_DIR present ($KERNEL_DIR) -- including kernel + modules"

    if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
        echo "build-update-package: $KERNEL_DIR exists but has no built zImage" >&2
        echo "  build it first: cd $KERNEL_DIR && ARCH=arm CROSS_COMPILE=$CROSS_COMPILE make zImage modules" >&2
        exit 1
    fi

    manifest_add "$KERNEL_DIR/arch/arm/boot/zImage" "boot/zImage-full"

    KVER="$(cat "$KERNEL_DIR/include/config/kernel.release" 2>/dev/null || true)"
    if [ -z "$KVER" ]; then
        echo "build-update-package: cannot determine kernel release (no include/config/kernel.release)" >&2
        exit 1
    fi
    echo "# kernel: $KVER" >> "$MANIFEST"

    # Same module set tools/chunked-deploy.sh deploys live over SSH --
    # shared via tools/kernel-modules.sh so there's only one place this
    # list can go stale relative to the kernel .config.
    . "$REPO/tools/kernel-modules.sh"
    for relpath in $AUDIO_MODULES; do
        manifest_add "$KERNEL_DIR/$relpath" "lib/modules/$KVER/zaurus-audio/$(basename "$relpath")"
    done

    # These keep the "kernel/" depmod-tree prefix exactly as
    # tools/kernel-modules.sh's WIFI_MODULES/SPI_MODULES/SD_MODULES lists do
    # (stripped to find the source file under KERNEL_DIR, kept as-is for
    # the /lib/modules/$KVER/... destination) -- some of these live
    # directly under drivers/, others (net/wireless, lib/crypto, fs/nls,
    # fs/fat) don't, so the prefix has to travel with each entry rather
    # than being reconstructed from a shorter name.
    WIFI_PCMCIA_SPI_SD_MODULES="$WIFI_MODULES
$SPI_MODULES
$SD_MODULES"
    for relpath in $WIFI_PCMCIA_SPI_SD_MODULES; do
        src_rel="$(echo "$relpath" | sed 's#^kernel/##')"
        manifest_add "$KERNEL_DIR/$src_rel" "lib/modules/$KVER/$relpath"
    done
else
    echo "==> KERNEL_DIR not found ($KERNEL_DIR) -- rootfs-only package (no kernel bump)"
    echo "# kernel: not included (rootfs-only package, no local kernel-src build found)" >> "$MANIFEST"
fi

echo "==> writing $OUT"
(
    cd "$STAGE"
    files="MANIFEST"
    while read -r rel; do
        files="$files payload/$rel"
    done < "$FILES_LIST"
    # shellcheck disable=SC2086
    tar --format=ustar --transform 's#^payload/##' -cf "$OUT" $files
)

md5sum "$OUT"
n="$(wc -l < "$FILES_LIST")"
echo "==> done: $OUT ($n file(s) + MANIFEST)"
echo "    copy to the SD card and run: piko-update /mnt/card/$(basename "$OUT")"
