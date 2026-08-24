#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SRC_DIR="$REPO/flash/src"
OUT_DIR="${OUT_DIR:-$REPO/build/flash}"
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
    echo "tools/userspace/build-piko-install.sh: no ARM cross compiler found in PATH." >&2
    echo "Expected one of: arm-unknown-linux-uclibcgnueabi-gcc, arm-buildroot-linux-uclibcgnueabi-gcc, arm-linux-gnueabi-gcc, arm-unknown-linux-gnueabi-gcc, arm-none-linux-gnueabi-gcc" >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, export CROSS_COMPILE explicitly," >&2
    echo "or run tools/toolchain/build-oabi-toolchain.sh first to build one." >&2
    exit 1
fi

CC="${CROSS_COMPILE}gcc"
READELF="${CROSS_COMPILE}readelf"
command -v "$READELF" >/dev/null 2>&1 || READELF="readelf"

echo "==> using compiler: $("$CC" --version | head -1)"
echo "==> CROSS_COMPILE=$CROSS_COMPILE"

CFLAGS="-mabi=apcs-gnu -march=armv5te -nostdlib -static -ffreestanding -fno-builtin -fno-stack-protector -Os -Wall -Wextra"

need_build() {
    out="$1"; shift
    [ "$FORCE" -eq 1 ] && return 0
    [ -f "$out" ] || return 0
    for src in "$@"; do
        [ "$src" -nt "$out" ] && return 0
    done
    [ "$0" -nt "$out" ] && return 0
    return 1
}

verify_oabi() {
    bin="$1"
    flags_line="$("$READELF" -h "$bin" | grep -i '^ *Flags:' || true)"
    echo "    $flags_line"
    case "$flags_line" in
        *0x600*) ;;
        *)
            echo "tools/userspace/build-piko-install.sh: $bin is NOT genuine OABI (expected ELF Flags: 0x600, got: $flags_line)" >&2
            echo "This binary will not run correctly on the target recovery kernel -- refusing to ship it." >&2
            exit 1
            ;;
    esac
}

build_one() {
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

echo "==> done. piko-install verified genuine OABI (ELF Flags: 0x600)."
