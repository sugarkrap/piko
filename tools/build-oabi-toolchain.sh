#!/bin/sh
set -eu

# Builds (or reuses) an ARM cross-toolchain capable of producing genuine
# OABI code via -mabi=apcs-gnu -- needed by tools/build-piko-install.sh
# (piko-install/piko-backup) and, separately, by tools/build-initramfs.sh's
# static-busybox build (which wants a small uclibc target, not a bloated
# glibc-static binary, to keep the bootstrap zImage's embedded initramfs
# inside the 1,294,336-byte NAND slot budget).
#
# This project's own dev toolchain (arm-unknown-linux-uclibcgnueabi,
# crosstool-NG 1.28.0 / GCC 13.4.0) lives at toolchain/x-tools/ but is
# gitignored (a huge, machine-specific build output, not source -- see
# .gitignore's `toolchain/` line and README.md's "Toolchain notes"). It is
# NOT vendored anywhere fetchable. What IS tracked is the crosstool-NG
# .config that produced it (tools/oabi-toolchain.config, a copy of the
# confirmed-working local build's config with the two machine-specific
# absolute paths replaced by an @@PIKO_TOOLCHAIN_DIR@@ placeholder this
# script substitutes) -- so any environment (this repo's CI included) can
# reproduce the identical toolchain from source, at the cost of a real
# (cacheable) build.
#
# -mabi=apcs-gnu is a GCC ARM-backend codegen flag, not something specific
# to this exact toolchain build -- it is expected to work identically
# against a stock arm-linux-gnueabi- cross-gcc too. This script always
# checks for an already-usable compiler FIRST (see find_working_cc below)
# before paying the cost of a from-source crosstool-NG build, so if the
# CI job already installed e.g. `gcc-arm-linux-gnueabi` via apt and that
# happens to support -mabi=apcs-gnu correctly, the expensive path below is
# skipped entirely. It is never assumed to work without checking --
# find_working_cc() always does a real compile + `readelf -h` flags check.
#
# Usage:
#   tools/build-oabi-toolchain.sh [--force]
#
# --force always does the from-source crosstool-NG build, skipping the
# "is there already a working compiler" short-circuit.
#
# Env overrides:
#   TOOLCHAIN_DIR   default <repo>/toolchain (gitignored)
#   CT_TARGET       default arm-unknown-linux-uclibcgnueabi (must match
#                   tools/oabi-toolchain.config's own CT_TARGET)
#   CT_NG_VERSION   default 1.28.0
#   EXTRA_CC_CANDIDATES  space-separated list of additional CROSS_COMPILE
#                   prefixes to try before building from source (e.g.
#                   "arm-linux-gnueabi-" once a CI job has apt-installed
#                   gcc-arm-linux-gnueabi)
#
# Exit codes:
#   0   a working OABI-capable toolchain is available; its bin/ dir is
#       printed on the last line as "TOOLCHAIN_BIN_DIR=<path>" and (if
#       GITHUB_OUTPUT is set) also written there as toolchain_bin_dir=...
#   1   hard failure (crosstool-NG build failed, or the toolchain it
#       produced still doesn't pass the OABI check -- never reported as
#       success if the empirical check doesn't actually pass)

REPO="$(cd "$(dirname "$0")/.." && pwd)"
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-$REPO/toolchain}"
CT_TARGET="${CT_TARGET:-arm-unknown-linux-uclibcgnueabi}"
CT_NG_VERSION="${CT_NG_VERSION:-1.28.0}"
CT_NG_URL="https://github.com/crosstool-ng/crosstool-ng/releases/download/crosstool-ng-${CT_NG_VERSION}/crosstool-ng-${CT_NG_VERSION}.tar.bz2"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

TARGET_BIN_DIR="$TOOLCHAIN_DIR/x-tools/$CT_TARGET/bin"

# check_oabi_cc PREFIX -- does `<prefix>gcc -mabi=apcs-gnu ...` actually
# produce ELF Flags: 0x600? This is the ONLY thing that decides whether a
# candidate compiler counts as "usable" -- a compiler that merely runs and
# exits 0 is not enough (see tools/build-piko-install.sh's header comment
# for the exact failure mode this guards against: a silently
# accidentally-EABI binary that looks fine but won't run on the device).
check_oabi_cc() {
    prefix="$1"
    command -v "${prefix}gcc" >/dev/null 2>&1 || return 1
    readelf="${prefix}readelf"
    command -v "$readelf" >/dev/null 2>&1 || readelf="readelf"
    command -v "$readelf" >/dev/null 2>&1 || return 1

    tmpd="$(mktemp -d)"
    cat > "$tmpd/probe.c" <<'EOF'
void _start(void) { __builtin_unreachable(); }
EOF
    if ! "${prefix}gcc" -mabi=apcs-gnu -march=armv5te -nostdlib -static \
            -ffreestanding -o "$tmpd/probe" "$tmpd/probe.c" 2>"$tmpd/err.log"; then
        rm -rf "$tmpd"
        return 1
    fi
    flags="$("$readelf" -h "$tmpd/probe" 2>/dev/null | grep -i '^ *Flags:' || true)"
    rm -rf "$tmpd"
    case "$flags" in
        *0x600*) return 0 ;;
        *) return 1 ;;
    esac
}

# find_working_cc -- searches, in priority order: an already-built local
# toolchain at $TARGET_BIN_DIR, then any prefix in $EXTRA_CC_CANDIDATES
# (space-separated, meant for a CI job to hand in e.g. "arm-linux-gnueabi-"
# after apt-installing gcc-arm-linux-gnueabi), then a short built-in list
# of common prefixes already on PATH. Prints the winning prefix's bin dir
# (empty string if the prefix was bare, meaning "already on PATH") and the
# prefix itself, space-separated, on success; prints nothing and returns 1
# if none work.
find_working_cc() {
    if [ -d "$TARGET_BIN_DIR" ]; then
        PATH="$TARGET_BIN_DIR:$PATH"
        if check_oabi_cc "${CT_TARGET}-"; then
            echo "$TARGET_BIN_DIR ${CT_TARGET}-"
            return 0
        fi
    fi
    for prefix in ${EXTRA_CC_CANDIDATES:-} arm-linux-gnueabi- arm-unknown-linux-gnueabi- \
                  arm-buildroot-linux-uclibcgnueabi- arm-none-linux-gnueabi-; do
        [ -n "$prefix" ] || continue
        if check_oabi_cc "$prefix"; then
            bindir="$(dirname "$(command -v "${prefix}gcc")")"
            echo "$bindir $prefix"
            return 0
        fi
    done
    return 1
}

report_and_exit() {
    bindir="$1"
    echo "TOOLCHAIN_BIN_DIR=$bindir"
    if [ -n "${GITHUB_OUTPUT:-}" ]; then
        echo "toolchain_bin_dir=$bindir" >> "$GITHUB_OUTPUT"
    fi
    exit 0
}

if [ "$FORCE" -eq 0 ]; then
    if result="$(find_working_cc)"; then
        bindir="${result%% *}"
        prefix="${result#* }"
        echo "==> already have a working OABI-capable compiler: ${prefix}gcc (in $bindir)"
        report_and_exit "$bindir"
    fi
    echo "==> no working OABI-capable compiler found on PATH, building one from source (crosstool-NG $CT_NG_VERSION)"
    echo "    this reproduces this project's own dev toolchain from the tracked config at"
    echo "    tools/oabi-toolchain.config -- expect this to take a while (tens of minutes),"
    echo "    it is a full binutils+gcc+uclibc-ng build. Cache toolchain/ across CI runs."
fi

# --- build crosstool-NG itself (source release, --enable-local so it runs
# straight out of its own build dir, no install step needed) ------------
CT_NG_SRC_DIR="$TOOLCHAIN_DIR/src/crosstool-ng-$CT_NG_VERSION"
CT_NG_TARBALL="$TOOLCHAIN_DIR/src/crosstool-ng-$CT_NG_VERSION.tar.bz2"
mkdir -p "$TOOLCHAIN_DIR/src"

if [ ! -x "$CT_NG_SRC_DIR/ct-ng" ]; then
    if [ ! -f "$CT_NG_TARBALL" ]; then
        echo "==> downloading $CT_NG_URL"
        curl -fL -o "$CT_NG_TARBALL.partial" "$CT_NG_URL"
        mv "$CT_NG_TARBALL.partial" "$CT_NG_TARBALL"
    else
        echo "==> reusing cached $CT_NG_TARBALL"
    fi
    if [ ! -d "$CT_NG_SRC_DIR" ]; then
        echo "==> extracting crosstool-NG to $TOOLCHAIN_DIR/src"
        tar xjf "$CT_NG_TARBALL" -C "$TOOLCHAIN_DIR/src"
    fi
    echo "==> building crosstool-NG itself (--enable-local)"
    ( cd "$CT_NG_SRC_DIR" && ./configure --enable-local && make -j"$(nproc)" )
fi

# --- build the actual OABI-capable target toolchain ---------------------
CT_BUILD_ROOT="$TOOLCHAIN_DIR/build/$CT_TARGET"
mkdir -p "$CT_BUILD_ROOT"

CONFIG_SRC="$REPO/tools/oabi-toolchain.config"
if [ ! -f "$CONFIG_SRC" ]; then
    echo "tools/build-oabi-toolchain.sh: missing tracked input: $CONFIG_SRC" >&2
    exit 1
fi
sed "s|@@PIKO_TOOLCHAIN_DIR@@|$TOOLCHAIN_DIR|g" "$CONFIG_SRC" > "$CT_BUILD_ROOT/.config"

echo "==> ct-ng olddefconfig (adapting tracked config to this crosstool-NG build)"
( cd "$CT_BUILD_ROOT" && "$CT_NG_SRC_DIR/ct-ng" olddefconfig )

echo "==> ct-ng build (this is the slow part -- full binutils+gcc+uclibc-ng build)"
( cd "$CT_BUILD_ROOT" && "$CT_NG_SRC_DIR/ct-ng" build )

PATH="$TARGET_BIN_DIR:$PATH"
if ! check_oabi_cc "${CT_TARGET}-"; then
    echo "tools/build-oabi-toolchain.sh: freshly built toolchain at $TARGET_BIN_DIR still does NOT" >&2
    echo "produce genuine OABI (ELF Flags: 0x600) with -mabi=apcs-gnu -- refusing to report success." >&2
    exit 1
fi

echo "==> built and verified OABI-capable toolchain at $TARGET_BIN_DIR"
report_and_exit "$TARGET_BIN_DIR"
