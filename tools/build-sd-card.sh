#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
KERNEL_VERSION="${KERNEL_VERSION:-7.1.4}"
KERNEL_DIR="${KERNEL_DIR:-$REPO/kernel-src/linux-$KERNEL_VERSION}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"
OUT_DIR="${OUT_DIR:-$REPO/sd-card}"

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

echo "==> [2/7] bootstrap zImage (mtd1, stage 1)"
"$REPO/tools/kernel/build-bootstrap.sh" $FORCE_FLAG

echo "==> [3/7] stage-2 kernel-src"
"$REPO/tools/kernel/setup-kernel-src.sh" $FORCE_FLAG

echo "==> [4/7] stage-2 kernel (zImage + modules)"
make -C "$KERNEL_DIR" ARCH=arm \
    CROSS_COMPILE="$TOOLCHAIN_BIN_DIR/arm-unknown-linux-uclibcgnueabi-" \
    -j"$JOBS" zImage modules

echo "==> [5/7] userspace payload + mtd3.jffs2 (mtd3, stage 2)"
"$REPO/tools/userspace/build-kexec.sh"
"$REPO/tools/userspace/build-ssh.sh"
"$REPO/tools/userspace/build-alsa.sh"
"$REPO/tools/userspace/build-thirdparty-deps.sh"
"$REPO/tools/userspace/build-userspace.sh" \
    --skip-ssh --skip-alsa --skip-kexec --skip-mplayer \
    --skip-st --skip-fltk --skip-toasters
KERNEL_DIR="$KERNEL_DIR" "$REPO/tools/build-mtd3-jffs2.sh"

echo "==> [6/7] piko-install + encoded updater.sh"
"$REPO/tools/userspace/build-piko-install.sh" $FORCE_FLAG
node "$REPO/tools/scripts/encode-updater.js" "$REPO/flash/updater-uncoded.sh" "$REPO/flash/updater-encoded.sh"

echo "==> [7/7] assembling SD card payload in $OUT_DIR"
mkdir -p "$OUT_DIR"
cp "$REPO/flash/zImage" "$OUT_DIR/zImage"
cp "$REPO/flash/mtd3.jffs2" "$OUT_DIR/mtd3.jffs2"
cp "$REPO/flash/piko.cfg" "$OUT_DIR/piko.cfg"
cp "$REPO/flash/piko-install" "$OUT_DIR/piko-install"
cp "$REPO/flash/updater-encoded.sh" "$OUT_DIR/updater.sh"
chmod 0755 "$OUT_DIR/piko-install"

echo ""
echo "==> done. copy the contents of $OUT_DIR to the root of the SD card:"
ls -la "$OUT_DIR"
