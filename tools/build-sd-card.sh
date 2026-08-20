#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
KERNEL_VERSION="${KERNEL_VERSION:-7.1.4}"
KERNEL_DIR="${KERNEL_DIR:-$REPO/kernel-src/linux-$KERNEL_VERSION}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"
OUT_DIR="${OUT_DIR:-$REPO/sd-card}"
FLAVORS="${FLAVORS:-sd}"

FORCE=0
FORCE_FLAG=""
[ "${1:-}" = "--force" ] && { FORCE=1; FORCE_FLAG="--force"; }

echo "==> [1/7] toolchain"
n=0
until "$REPO/tools/toolchain/build-uclibc-toolchain.sh" $FORCE_FLAG; do
    n=$((n + 1))
    if [ "$n" -ge 3 ]; then
        echo "tools/build-sd-card.sh: build-uclibc-toolchain.sh kept failing, giving up" >&2
        exit 1
    fi
    echo "build-uclibc-toolchain.sh failed (attempt $n), retrying in 30s"
    sleep 30
done
"$REPO/tools/toolchain/build-oabi-toolchain.sh"

echo "==> [2/7] bootstrap zImage per flavor (mtd1, stage 1)"
FLAVOR_FORCE="$FORCE_FLAG"
for flavor in $FLAVORS; do
    "$REPO/tools/kernel/build-initramfs.sh" --flavor "$flavor" $FLAVOR_FORCE
    "$REPO/tools/kernel/build-bootstrap.sh" --flavor "$flavor" $FLAVOR_FORCE
    FLAVOR_FORCE=""
done

echo "==> [3/7] stage-2 kernel-src"
"$REPO/tools/kernel/setup-kernel-src.sh" $FORCE_FLAG

echo "==> [4/7] stage-2 initramfs + kernel (zImage + modules)"
"$REPO/tools/kernel/build-initramfs.sh" --stage2
( cd "$KERNEL_DIR" && ./scripts/config \
    --set-str CONFIG_INITRAMFS_SOURCE "$REPO/initramfs/initramfs-stage2-built.cpio.gz" )
make -C "$KERNEL_DIR" ARCH=arm \
    CROSS_COMPILE="$TOOLCHAIN_BIN_DIR/arm-unknown-linux-uclibcgnueabi-" \
    olddefconfig
make -C "$KERNEL_DIR" ARCH=arm \
    CROSS_COMPILE="$TOOLCHAIN_BIN_DIR/arm-unknown-linux-uclibcgnueabi-" \
    -j"$JOBS" zImage modules

echo "==> [5/7] userspace payload + stage-2 root image"
"$REPO/tools/userspace/build-kexec.sh"
"$REPO/tools/userspace/build-ssh.sh"
"$REPO/tools/userspace/build-alsa.sh"
"$REPO/tools/userspace/build-thirdparty-deps.sh"
"$REPO/tools/userspace/build-x11-stack.sh"
"$REPO/tools/userspace/build-userspace.sh" \
    --skip-ssh --skip-alsa --skip-kexec --skip-mplayer \
    --skip-st --skip-fltk --skip-toasters
KERNEL_DIR="$KERNEL_DIR" ROOT_IMG_OUT="$REPO/flash/piko-root.img" \
    "$REPO/tools/build-rootfs.sh"

echo "==> [6/7] piko-install + encoded updater.sh"
"$REPO/tools/userspace/build-piko-install.sh" $FORCE_FLAG
node "$REPO/tools/scripts/encode-updater.js" "$REPO/flash/updater-uncoded.sh" "$REPO/flash/updater-encoded.sh"

echo "==> [7/7] assembling one payload per flavor under $OUT_DIR"
for flavor in $FLAVORS; do
    dest="$OUT_DIR/$flavor"
    rm -rf "$dest"
    mkdir -p "$dest"

    cp "$REPO/flash/zImage-$flavor"      "$dest/zImage"
    cp "$REPO/flash/cfg/$flavor.cfg"     "$dest/piko.cfg"
    cp "$REPO/flash/piko-install"        "$dest/piko-install"
    cp "$REPO/flash/updater-encoded.sh"  "$dest/updater.sh"
    chmod 0755 "$dest/piko-install"

    cp "$KERNEL_DIR/arch/arm/boot/zImage" "$dest/zImage-full"
    cp "$REPO/flash/piko-root.img"        "$dest/piko-root.img"
    cp "$REPO/userspace/stage-kexec/sbin/kexec" "$dest/kexec"
    cp "$REPO/flash/cfg/piko-boot.cfg"    "$dest/piko-boot.cfg"
    chmod 0755 "$dest/kexec"
done

UPDATE="$OUT_DIR/update/.zaurus"
rm -rf "$OUT_DIR/update"
mkdir -p "$UPDATE"
cp "$KERNEL_DIR/arch/arm/boot/zImage" "$UPDATE/zImage-full"
cp "$REPO/flash/piko-root.img"        "$UPDATE/piko-root.img"
cp "$REPO/userspace/stage-kexec/sbin/kexec" "$UPDATE/kexec"
cp "$REPO/flash/cfg/piko-boot.cfg"    "$UPDATE/piko-boot.cfg"
chmod 0755 "$UPDATE/kexec"

echo ""
echo "==> done. full install: copy the contents of one flavor directory to the card root"
for flavor in $FLAVORS; do
    echo ""
    echo "    $OUT_DIR/$flavor"
    ls -la "$OUT_DIR/$flavor" | sed 's/^/      /'
done
echo ""
echo "==> stage-2 update: drop this .zaurus onto a card that already runs piko"
ls -la "$UPDATE" | sed 's/^/      /'
