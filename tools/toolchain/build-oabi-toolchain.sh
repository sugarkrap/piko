#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-$REPO/toolchain}"
CT_TARGET="${CT_TARGET:-arm-unknown-linux-uclibcgnueabi}"
CT_NG_VERSION="${CT_NG_VERSION:-1.28.0}"
CT_NG_URL="https://github.com/crosstool-ng/crosstool-ng/releases/download/crosstool-ng-${CT_NG_VERSION}/crosstool-ng-${CT_NG_VERSION}.tar.bz2"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

TARGET_BIN_DIR="$TOOLCHAIN_DIR/x-tools/$CT_TARGET/bin"

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
    echo "    tools/toolchain/oabi-toolchain.config -- expect this to take a while (tens of minutes),"
    echo "    it is a full binutils+gcc+uclibc-ng build. Cache toolchain/ across CI runs."
fi

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

CT_BUILD_ROOT="$TOOLCHAIN_DIR/build/$CT_TARGET"
mkdir -p "$CT_BUILD_ROOT"

CONFIG_SRC="$REPO/tools/toolchain/oabi-toolchain.config"
if [ ! -f "$CONFIG_SRC" ]; then
    echo "tools/toolchain/build-oabi-toolchain.sh: missing tracked input: $CONFIG_SRC" >&2
    exit 1
fi
sed "s|@@PIKO_TOOLCHAIN_DIR@@|$TOOLCHAIN_DIR|g" "$CONFIG_SRC" > "$CT_BUILD_ROOT/.config"

echo "==> ct-ng olddefconfig (adapting tracked config to this crosstool-NG build)"
( cd "$CT_BUILD_ROOT" && "$CT_NG_SRC_DIR/ct-ng" olddefconfig )

echo "==> ct-ng build (this is the slow part -- full binutils+gcc+uclibc-ng build)"
( cd "$CT_BUILD_ROOT" && "$CT_NG_SRC_DIR/ct-ng" build )

PATH="$TARGET_BIN_DIR:$PATH"
if ! check_oabi_cc "${CT_TARGET}-"; then
    echo "tools/toolchain/build-oabi-toolchain.sh: freshly built toolchain at $TARGET_BIN_DIR still does NOT" >&2
    echo "produce genuine OABI (ELF Flags: 0x600) with -mabi=apcs-gnu -- refusing to report success." >&2
    exit 1
fi

echo "==> built and verified OABI-capable toolchain at $TARGET_BIN_DIR"
report_and_exit "$TARGET_BIN_DIR"
