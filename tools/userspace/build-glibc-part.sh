#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
STAGE_DIR="${STAGE_DIR:-$REPO/userspace/stage-glibc}"
CACHE="${CACHE:-$REPO/userspace/.thirdparty-cache}"
MIRROR="${MIRROR:-http://archive.debian.org/debian}"

LIBC6_DEB="${LIBC6_DEB:-libc6_2.31-13+deb11u11_armel.deb}"
LIBC6_URL="${LIBC6_URL:-$MIRROR/pool/main/g/glibc/$LIBC6_DEB}"
LIBGCC_DEB="${LIBGCC_DEB:-libgcc-s1_10.2.1-6_armel.deb}"
LIBGCC_URL="${LIBGCC_URL:-$MIRROR/pool/main/g/gcc-10/$LIBGCC_DEB}"
LIBSTDCXX_DEB="${LIBSTDCXX_DEB:-libstdc++6_10.2.1-6_armel.deb}"
LIBSTDCXX_URL="${LIBSTDCXX_URL:-$MIRROR/pool/main/g/gcc-10/$LIBSTDCXX_DEB}"
ZLIB_DEB="${ZLIB_DEB:-zlib1g_1.2.8.dfsg-5_armel.deb}"
ZLIB_URL="${ZLIB_URL:-$MIRROR/pool/main/z/zlib/$ZLIB_DEB}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

for tool in ar tar curl; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "tools/userspace/build-glibc-part.sh: $tool is required" >&2
        exit 1
    fi
done

mkdir -p "$CACHE"

fetch() {
    _deb="$1"; _url="$2"
    if [ -f "$CACHE/$_deb" ]; then
        echo "==> reusing cached $_deb"
        return 0
    fi
    echo "==> downloading $_url"
    if ! curl -fL --max-time 300 -o "$CACHE/$_deb.partial" "$_url"; then
        rm -f "$CACHE/$_deb.partial"
        echo "tools/userspace/build-glibc-part.sh: cannot download $_deb" >&2
        echo "  Debian armel is ARMv5TE soft-float, which is what this board needs." >&2
        echo "  Override LIBC6_DEB/LIBC6_URL if the archive moved it." >&2
        exit 1
    fi
    mv "$CACHE/$_deb.partial" "$CACHE/$_deb"
}

unpack() {
    _deb="$1"; _into="$2"
    _tmp="$(mktemp -d /tmp/piko-glibc-part.XXXXXX)"
    ( cd "$_tmp" && ar x "$CACHE/$_deb" )
    _data="$(ls "$_tmp"/data.tar.* 2>/dev/null | head -1)"
    if [ -z "$_data" ]; then
        echo "tools/userspace/build-glibc-part.sh: no data member in $_deb" >&2
        rm -rf "$_tmp"
        exit 1
    fi
    mkdir -p "$_into"
    tar -xf "$_data" -C "$_into"
    rm -rf "$_tmp"
}

if [ "$FORCE" -eq 1 ] || [ ! -d "$STAGE_DIR" ]; then
    rm -rf "$STAGE_DIR"
fi
mkdir -p "$STAGE_DIR"

fetch "$LIBC6_DEB" "$LIBC6_URL"
fetch "$LIBGCC_DEB" "$LIBGCC_URL"
fetch "$LIBSTDCXX_DEB" "$LIBSTDCXX_URL"
fetch "$ZLIB_DEB" "$ZLIB_URL"

RAW="$(mktemp -d /tmp/piko-glibc-raw.XXXXXX)"
trap 'rm -rf "$RAW"' EXIT
unpack "$LIBC6_DEB" "$RAW"
unpack "$LIBGCC_DEB" "$RAW"
unpack "$LIBSTDCXX_DEB" "$RAW"
unpack "$ZLIB_DEB" "$RAW"

mkdir -p "$STAGE_DIR/lib"

copied=0
for src in "$RAW/lib/arm-linux-gnueabi"/* "$RAW/usr/lib/arm-linux-gnueabi"/*; do
    [ -e "$src" ] || continue
    case "$(basename "$src")" in
        *.a|*.o|audit|gconv) continue ;;
    esac
    cp -a "$src" "$STAGE_DIR/lib/"
    copied=$((copied + 1))
done

if [ "$copied" -eq 0 ]; then
    echo "tools/userspace/build-glibc-part.sh: the .deb layout was not what we expected" >&2
    echo "  looked under lib/arm-linux-gnueabi and usr/lib/arm-linux-gnueabi" >&2
    exit 1
fi

echo "==> flattening symlinks (SD is VFAT and cannot store them)"
flattened=0
for link in "$STAGE_DIR/lib"/*; do
    [ -L "$link" ] || continue
    target="$(readlink "$link")"
    case "$target" in
        */*) continue ;;
    esac
    [ -f "$STAGE_DIR/lib/$target" ] || continue
    refs=0
    for other in "$STAGE_DIR/lib"/*; do
        [ -L "$other" ] || continue
        [ "$(readlink "$other")" = "$target" ] && refs=$((refs + 1))
    done
    rm -f "$link"
    if [ "$refs" -le 1 ]; then
        mv "$STAGE_DIR/lib/$target" "$link"
    else
        cp -a "$STAGE_DIR/lib/$target" "$link"
    fi
    flattened=$((flattened + 1))
done
echo "    $flattened symlink(s) replaced by the real file"

for orphan in "$STAGE_DIR/lib"/*; do
    [ -L "$orphan" ] && rm -f "$orphan"
done

for want in libstdc++.so.6 libz.so.1; do
    if [ ! -e "$STAGE_DIR/lib/$want" ]; then
        echo "tools/userspace/build-glibc-part.sh: $want is missing -- the Java runtime needs it" >&2
        exit 1
    fi
done

LOADER="$(cd "$STAGE_DIR/lib" && ls ld-linux.so.3 ld-*.so 2>/dev/null | head -1)"
if [ -z "$LOADER" ]; then
    echo "tools/userspace/build-glibc-part.sh: no dynamic loader in the staged tree" >&2
    exit 1
fi
if [ ! -e "$STAGE_DIR/lib/ld-linux.so.3" ]; then
    ln -sf "$LOADER" "$STAGE_DIR/lib/ld-linux.so.3"
fi

echo "==> verifying the loader is ARM soft-float"
if command -v readelf >/dev/null 2>&1; then
    real_loader="$(readlink -f "$STAGE_DIR/lib/ld-linux.so.3")"
    flags="$(LC_ALL=C readelf -h "$real_loader" | sed -n 's/^ *Flags: *//p')"
    case "$flags" in
        *soft-float*|0x5000200*|0x5000000*) echo "    Flags: $flags" ;;
        *)
            echo "tools/userspace/build-glibc-part.sh: loader is not soft-float ABI: $flags" >&2
            echo "  armel is required -- armhf will not run on this board" >&2
            exit 1
            ;;
    esac
fi

size_kb="$(du -sk "$STAGE_DIR" | while read -r n _; do echo "$n"; break; done)"

echo ""
echo "==> done: $STAGE_DIR ($size_kb KiB, $copied files)"
echo "    loader: lib/ld-linux.so.3"
echo ""
echo "    This runs ALONGSIDE uClibc, it does not replace it. Nothing on the"
echo "    system links against it by default -- glibc binaries are launched"
echo "    through the loader explicitly:"
echo ""
echo "      <part>/lib/ld-linux.so.3 --library-path <part>/lib <program>"
echo ""
echo "    build-rootfs.sh ships it in the root image at /usr/glibc/lib."
