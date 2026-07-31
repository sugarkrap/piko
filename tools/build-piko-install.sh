#!/bin/sh
set -eu

# Builds the freestanding ARM OABI recovery binaries: piko-install (the SD
# card flasher piko.cfg/updater.sh actually invoke) and piko-backup (a
# read-only diagnostic tool, not part of the flash path). Both are
# no-libc/no-TLS binaries targeting Cacko's ancient pre-EABI recovery
# kernel -- see flash/src/piko-install.c's own header comment for the full
# ABI rationale (syscall number baked into the swi immediate, not r7).
#
# -mabi=apcs-gnu is what actually forces genuine OABI code generation; it's
# a GCC ARM-backend codegen flag, not something specific to any particular
# cross-toolchain build, so this works against ANY arm-*-gcc capable of
# targeting armv5te (confirmed empirically 2026-07-30 against a
# crosstool-ng arm-unknown-linux-uclibcgnueabi toolchain -- see
# tools/build-oabi-toolchain.sh). Since these binaries are -nostdlib, the
# target libc flavor of whichever toolchain is used (uclibc, glibc,
# whatever) is irrelevant -- nothing here ever links against it.
#
# ACCEPTANCE CRITERION: "gcc exited 0" is NOT sufficient. A wrong
# toolchain/flag combination silently produces an accidentally-EABI binary
# (ELF e_flags something like 0x5000200) that looks like a normal
# successful build but will not run correctly against the target recovery
# kernel. The only real signal is the ELF header itself: Flags: 0x600
# exactly. This script greps `readelf -h` for that on every binary it
# produces and fails the build if it's not there -- see verify_oabi()
# below.
#
# Usage:
#   tools/build-piko-install.sh [--force]
#
# --force rebuilds even if outputs already look up to date (default:
# skip a binary if it's newer than its source + this script).
#
# Env overrides:
#   TOOLCHAIN_BIN_DIR  optional compiler bin dir prepended to PATH
#                      (default: <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin,
#                      matching tools/build-oabi-toolchain.sh's output location)
#   CROSS_COMPILE      optional explicit compiler prefix (e.g. arm-linux-gnueabi-).
#                      If unset, auto-detected from a priority list of prefixes.
#   OUT_DIR            default <repo>/flash (where piko-install/piko-backup land --
#                      this is also where piko.cfg's "target" lines and updater.sh
#                      expect them to already be, flat, no subdirectory)
#
# Exit codes:
#   0   both binaries built (or already up to date) and verified genuine OABI
#   1   a hard failure: no usable compiler found, a build failed, or a
#       built binary does NOT carry ELF Flags: 0x600 (never shipped as if
#       it were fine -- see docs/FLASH-MTD1-MTD3-SAFE.md, this project
#       treats a wrong-ABI flash tool as a bricking risk, not a soft bug)

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$REPO/flash/src"
BACKUP_SRC="$REPO/flash/piko-backup.c"
OUT_DIR="${OUT_DIR:-$REPO/flash}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi

if [ -z "${CROSS_COMPILE:-}" ]; then
    for prefix in arm-unknown-linux-uclibcgnueabi- arm-buildroot-linux-uclibcgnueabi- \
                  arm-linux-gnueabi- arm-unknown-linux-gnueabi- arm-none-linux-gnueabi-; do
        if command -v "${prefix}gcc" >/dev/null 2>&1; then
            CROSS_COMPILE="$prefix"
            break
        fi
    done
fi

if [ -z "${CROSS_COMPILE:-}" ]; then
    echo "tools/build-piko-install.sh: no ARM cross compiler found in PATH." >&2
    echo "Expected one of: arm-unknown-linux-uclibcgnueabi-gcc, arm-buildroot-linux-uclibcgnueabi-gcc, arm-linux-gnueabi-gcc, arm-unknown-linux-gnueabi-gcc, arm-none-linux-gnueabi-gcc" >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, export CROSS_COMPILE explicitly," >&2
    echo "or run tools/build-oabi-toolchain.sh first to build one." >&2
    exit 1
fi

CC="${CROSS_COMPILE}gcc"
READELF="${CROSS_COMPILE}readelf"
command -v "$READELF" >/dev/null 2>&1 || READELF="readelf"

echo "==> using compiler: $("$CC" --version | head -1)"
echo "==> CROSS_COMPILE=$CROSS_COMPILE"

# Deliberately freestanding: no libc, no libgcc (this toolchain's libgcc.a
# is EABI-only and can't be linked into an OABI binary -- the source
# already works around every case that would otherwise need it, e.g.
# hand-rolled udiv10() instead of __udivsi3/__umodsi3, see the comments in
# flash/src/piko-install.c). -mabi=apcs-gnu is the ABI-forcing flag; see
# the file header comment above for why nothing else here matters as much.
CFLAGS="-mabi=apcs-gnu -march=armv5te -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector -Os -Wall -Wextra"

need_build() {
    # need_build OUT SRC...
    out="$1"; shift
    [ "$FORCE" -eq 1 ] && return 0
    [ -f "$out" ] || return 0
    for src in "$@"; do
        [ "$src" -nt "$out" ] && return 0
    done
    [ "$0" -nt "$out" ] && return 0
    return 1
}

# verify_oabi BINARY -- the actual acceptance gate. 0x600 is the genuine
# pre-EABI OABI flags value; anything else (most commonly something like
# 0x5000200, "accidentally-EABI") means the ABI-forcing flag didn't take
# and the binary will not work on the device even though it built cleanly.
verify_oabi() {
    bin="$1"
    flags_line="$("$READELF" -h "$bin" | grep -i '^ *Flags:' || true)"
    echo "    $flags_line"
    case "$flags_line" in
        *0x600*) ;;
        *)
            echo "tools/build-piko-install.sh: $bin is NOT genuine OABI (expected ELF Flags: 0x600, got: $flags_line)" >&2
            echo "This binary will not run correctly on the target recovery kernel -- refusing to ship it." >&2
            exit 1
            ;;
    esac
}

build_one() {
    # build_one NAME SRC...
    name="$1"; shift
    out="$OUT_DIR/$name"
    if ! need_build "$out" "$@"; then
        echo "==> $out already up to date, skipping (pass --force to rebuild)"
        verify_oabi "$out"
        return 0
    fi
    echo "==> building $name from: $*"
    "$CC" $CFLAGS -o "$out.partial" "$@"
    mv "$out.partial" "$out"
    verify_oabi "$out"
    echo "==> built $out ($(wc -c < "$out") bytes)"
}

mkdir -p "$OUT_DIR"

build_one piko-install "$SRC_DIR/piko-install.c"

# piko-backup is a read-only diagnostic/backup tool, not part of the actual
# SD-card flash path (piko.cfg/updater.sh never invoke it) -- build it for
# completeness and its own OABI verification, but callers assembling
# piko.zip should NOT ship it: only piko-install is what piko.cfg's
# "target" lines and the uncoded updater.sh actually run.
build_one piko-backup "$BACKUP_SRC"

echo "==> done. piko-install and piko-backup both verified genuine OABI (ELF Flags: 0x600)."
echo "    Only $OUT_DIR/piko-install belongs in piko.zip -- piko-backup is diagnostic-only."
