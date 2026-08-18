#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL_VERSION="${KERNEL_VERSION:-7.1.4}"
KERNEL_SRC_DIR="${KERNEL_SRC_DIR:-$REPO/kernel-src-bootstrap}"
KERNEL_DIR="$KERNEL_SRC_DIR/linux-$KERNEL_VERSION"
BASE_CONFIG="$REPO/kernel.config-corgi-$KERNEL_VERSION-minimal"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

MTD1_SLOT=1294336
MTD1_PROVEN=1280656

FORCE=0
FLAVOR=sd
OUT=
while [ $# -gt 0 ]; do
    case "$1" in
        --force)  FORCE=1; shift ;;
        --flavor) FLAVOR="$2"; shift 2 ;;
        *)        OUT="$1"; shift ;;
    esac
done

FLAVOR_CONF="$REPO/tools/kernel/flavors/$FLAVOR.conf"
COMMON_CONF="$REPO/tools/kernel/flavors/common.conf"
FRAGMENTS="$COMMON_CONF $FLAVOR_CONF"
if [ ! -f "$FLAVOR_CONF" ]; then
    echo "tools/kernel/build-bootstrap.sh: unknown flavor '$FLAVOR' (no $FLAVOR_CONF)" >&2
    echo "  available: $(cd "$REPO/tools/kernel/flavors" && echo *.conf | sed 's/\.conf//g')" >&2
    exit 1
fi

[ -n "$OUT" ] || OUT="$REPO/flash/zImage-$FLAVOR"
INITRAMFS_CPIO="${INITRAMFS_CPIO:-$REPO/initramfs/initramfs-minimal-built-$FLAVOR.cpio.gz}"

if [ ! -f "$INITRAMFS_CPIO" ]; then
    echo "==> no built initramfs at $INITRAMFS_CPIO -- building it"
    "$REPO/tools/kernel/build-initramfs.sh" --flavor "$FLAVOR"
fi
if [ ! -f "$INITRAMFS_CPIO" ]; then
    echo "tools/kernel/build-bootstrap.sh: still no $INITRAMFS_CPIO after build-initramfs.sh -- check OUT_CPIO" >&2
    exit 1
fi

echo "==> preparing $KERNEL_DIR from $(basename "$BASE_CONFIG")"
FORCE_ARG=""
[ "$FORCE" -eq 1 ] && FORCE_ARG="--force"
KERNEL_SRC_DIR="$KERNEL_SRC_DIR" \
KERNEL_CONFIG="kernel.config-corgi-$KERNEL_VERSION-minimal" \
    "$REPO/tools/kernel/setup-kernel-src.sh" $FORCE_ARG

if [ -n "${TOOLCHAIN_BIN_DIR}" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if [ -z "${CROSS_COMPILE:-}" ]; then
    for prefix in arm-unknown-linux-uclibcgnueabi- arm-buildroot-linux-uclibcgnueabi- arm-linux-gnueabi- arm-unknown-linux-gnueabi-; do
        if command -v "${prefix}gcc" >/dev/null 2>&1; then
            CROSS_COMPILE="$prefix"
            break
        fi
    done
fi
if [ -z "${CROSS_COMPILE:-}" ]; then
    echo "tools/kernel/build-bootstrap.sh: no ARM cross compiler found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi

echo "==> restoring the base config"
cp "$BASE_CONFIG" "$KERNEL_DIR/.config"

echo "==> applying the common and $FLAVOR fragments"
for frag in $FRAGMENTS; do
    while read -r op sym val; do
        [ -n "$op" ] || continue
        case "$op" in
            --set-str|--set-val) ( cd "$KERNEL_DIR" && ./scripts/config "$op" "$sym" "$val" ) ;;
            *)                   ( cd "$KERNEL_DIR" && ./scripts/config "$op" "$sym" ) ;;
        esac
    done < "$frag"
done

echo "==> pointing CONFIG_INITRAMFS_SOURCE at $INITRAMFS_CPIO"
( cd "$KERNEL_DIR" && ./scripts/config --set-str CONFIG_INITRAMFS_SOURCE "$INITRAMFS_CPIO" )
( cd "$KERNEL_DIR" && make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" olddefconfig >/dev/null )

echo "==> verifying the fragment survived olddefconfig"
bad=0
for frag in $FRAGMENTS; do
    while read -r op sym val; do
        [ -n "$op" ] || continue
        state="$( cd "$KERNEL_DIR" && ./scripts/config --state "$sym" 2>/dev/null || echo undef )"
        case "$op" in
            --enable)
                case "$state" in
                    y|m) ;;
                    *)
                        echo "  CONFIG_$sym is '$state', expected y -- an unmet dependency dropped it" >&2
                        bad=1
                        ;;
                esac
                ;;
            --disable)
                case "$state" in
                    y|m)
                        echo "  CONFIG_$sym is '$state', expected off -- something reselected it" >&2
                        bad=1
                        ;;
                esac
                ;;
        esac
    done < "$frag"
done
if [ "$bad" -ne 0 ]; then
    echo "tools/kernel/build-bootstrap.sh: $FLAVOR fragment did not survive olddefconfig" >&2
    exit 1
fi

echo "==> building the $FLAVOR bootstrap zImage (-j$JOBS)"
( cd "$KERNEL_DIR" && make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" -j"$JOBS" zImage )

if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
    echo "tools/kernel/build-bootstrap.sh: build finished but no arch/arm/boot/zImage -- something failed silently" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")"
cp "$KERNEL_DIR/arch/arm/boot/zImage" "$OUT.partial"
mv "$OUT.partial" "$OUT"

SIZE="$(stat -c '%s' "$OUT")"
echo ""
echo "==> done: $OUT ($SIZE of $MTD1_SLOT bytes, $((SIZE * 100 / MTD1_SLOT))% of the mtd1 kernel slot)"
if [ "$SIZE" -gt "$MTD1_SLOT" ]; then
    echo "tools/kernel/build-bootstrap.sh: $FLAVOR zImage $((SIZE - MTD1_SLOT)) bytes over the $MTD1_SLOT slot, would overwrite a recovery kernel" >&2
    exit 1
fi
if [ "$SIZE" -gt "$MTD1_PROVEN" ]; then
    echo "    $((SIZE - MTD1_PROVEN)) over the largest shipped bootstrap ($MTD1_PROVEN), untested on hardware"
fi
