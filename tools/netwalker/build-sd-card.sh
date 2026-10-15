#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
KERNEL_VERSION="${KERNEL_VERSION:-7.1.4}"
BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.36.1}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
BUILD="${BUILD:-$REPO/build/netwalker}"
DL_DIR="${DL_DIR:-$REPO/build/kernel/dl}"
OUT_TARBALL="${OUT_TARBALL:-$REPO/build/netwalker.tar.gz}"
OUT_IMAGE="${OUT_IMAGE:-$REPO/build/netwalker.img}"
IMAGE_MB="${IMAGE_MB:-64}"

KERNEL_TARBALL="$DL_DIR/linux-$KERNEL_VERSION.tar.xz"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v${KERNEL_VERSION%%.*}.x/linux-$KERNEL_VERSION.tar.xz"
KERNEL_DIR="$BUILD/linux-$KERNEL_VERSION"
BUSYBOX_DIR="$BUILD/busybox-$BUSYBOX_VERSION"
INITRAMFS_DIR="$BUILD/initramfs"
INITRAMFS_LIST="$BUILD/initramfs.list"
SD_DIR="$BUILD/sd"
DTB=imx51-netwalker.dtb
CMDLINE="console=ttymxc0,115200 console=tty0 loglevel=7"

export ARCH=arm CROSS_COMPILE

die() {
    echo "tools/netwalker/build-sd-card.sh: $*" >&2
    exit 1
}

command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1 || die "${CROSS_COMPILE}gcc not found in PATH"

mkdir -p "$BUILD" "$DL_DIR"

if [ -f "$KERNEL_TARBALL" ] && ! xz -t "$KERNEL_TARBALL" 2>/dev/null; then
    rm -f "$KERNEL_TARBALL"
fi
if [ ! -f "$KERNEL_TARBALL" ]; then
    echo "==> downloading $KERNEL_URL"
    curl -fL --http1.1 --retry 6 --retry-delay 5 --retry-all-errors -C - \
         -o "$KERNEL_TARBALL.partial" "$KERNEL_URL"
    xz -t "$KERNEL_TARBALL.partial"
    mv "$KERNEL_TARBALL.partial" "$KERNEL_TARBALL"
fi

if [ ! -f "$KERNEL_DIR/Makefile" ]; then
    echo "==> extracting linux-$KERNEL_VERSION"
    rm -rf "$KERNEL_DIR"
    tar xf "$KERNEL_TARBALL" -C "$BUILD"
fi

DTS_DIR="$KERNEL_DIR/arch/arm/boot/dts/nxp/imx"
cp "$REPO/modules/netwalker/imx51-netwalker.dts" "$DTS_DIR/"
grep -q "$DTB" "$DTS_DIR/Makefile" || echo "dtb-\$(CONFIG_SOC_IMX51) += $DTB" >> "$DTS_DIR/Makefile"

if [ ! -x "$BUSYBOX_DIR/busybox" ]; then
    echo "==> building busybox $BUSYBOX_VERSION"
    rm -rf "$BUSYBOX_DIR"
    tar xjf "$REPO/vendor/busybox-$BUSYBOX_VERSION.tar.bz2" -C "$BUILD"
    make -C "$BUSYBOX_DIR" -s defconfig
    sed -i \
        -e 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' \
        -e 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' \
        "$BUSYBOX_DIR/.config"
    make -C "$BUSYBOX_DIR" -s oldconfig </dev/null >/dev/null
    make -C "$BUSYBOX_DIR" -s -j"$JOBS"
fi

echo "==> staging the initramfs"
rm -rf "$INITRAMFS_DIR"
make -C "$BUSYBOX_DIR" -s CONFIG_PREFIX="$INITRAMFS_DIR" install >/dev/null
cp -a "$REPO/modules/netwalker/rootfs/." "$INITRAMFS_DIR/"
mkdir -p "$INITRAMFS_DIR/dev" "$INITRAMFS_DIR/proc" "$INITRAMFS_DIR/sys" \
         "$INITRAMFS_DIR/tmp" "$INITRAMFS_DIR/root" "$INITRAMFS_DIR/mnt"
ln -sf bin/busybox "$INITRAMFS_DIR/init"
cat > "$INITRAMFS_LIST" <<LIST
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
LIST

echo "==> configuring linux-$KERNEL_VERSION"
cp "$REPO/kernel.config-netwalker-$KERNEL_VERSION" "$KERNEL_DIR/.config"
"$KERNEL_DIR/scripts/config" --file "$KERNEL_DIR/.config" \
    --set-str INITRAMFS_SOURCE "$INITRAMFS_DIR $INITRAMFS_LIST" \
    --set-val INITRAMFS_ROOT_UID 0 \
    --set-val INITRAMFS_ROOT_GID 0
make -C "$KERNEL_DIR" -s olddefconfig

echo "==> building zImage and $DTB"
make -C "$KERNEL_DIR" -s -j"$JOBS" zImage "nxp/imx/$DTB"

echo "==> assembling the sd card tree"
rm -rf "$SD_DIR"
mkdir -p "$SD_DIR/boot"
cat "$KERNEL_DIR/arch/arm/boot/zImage" "$KERNEL_DIR/arch/arm/boot/dts/nxp/imx/$DTB" \
    > "$SD_DIR/boot/zImage"
cp "$KERNEL_DIR/arch/arm/boot/dts/nxp/imx/$DTB" "$SD_DIR/boot/"
echo "/boot/zImage $CMDLINE" > "$SD_DIR/boot/boot.conf"

tar -C "$SD_DIR" --owner=0 --group=0 -czf "$OUT_TARBALL" boot
echo "==> $OUT_TARBALL"
tar -tzvf "$OUT_TARBALL"

echo "==> building the ${IMAGE_MB}MiB sd card image"
PART_START=1
PART_SECTORS=$((IMAGE_MB * 2048 - PART_START))
PART_BLOCKS=$((PART_SECTORS / 8))
rm -f "$OUT_IMAGE" "$OUT_IMAGE.xz"
truncate -s "${IMAGE_MB}M" "$OUT_IMAGE"
printf 'label: dos\nstart=%s, size=%s, type=83, bootable\n' "$PART_START" "$PART_SECTORS" \
    | sfdisk --quiet --no-reread --no-tell-kernel "$OUT_IMAGE"
mke2fs -q -F -t ext2 -b 4096 -I 128 -O none -L netwalker -E root_owner=0:0,offset=$((PART_START * 512)) \
    -d "$SD_DIR" "$OUT_IMAGE" "$PART_BLOCKS"
xz -T0 -9 "$OUT_IMAGE"
echo "==> $OUT_IMAGE.xz"
