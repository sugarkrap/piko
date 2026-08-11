#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BASE_JFFS2="${1:-$REPO/flash/base.jffs2}"
OUT="${2:-$REPO/flash/mtd3.jffs2}"

KERNEL_DIR="${KERNEL_DIR:-$REPO/kernel-src/linux-7.1.4}"
ERASEBLOCK="${ERASEBLOCK:-0x4000}"

if [ ! -f "$BASE_JFFS2" ]; then
    echo "build-mtd3-jffs2: base image not found: $BASE_JFFS2" >&2
    exit 1
fi
case "$(file -b "$BASE_JFFS2" 2>/dev/null)" in
    *jffs2*little*endian*) ;;
    *) echo "build-mtd3-jffs2: $BASE_JFFS2 doesn't look like a little-endian JFFS2 image, refusing to guess" >&2; exit 1 ;;
esac

if ! command -v mkfs.jffs2 >/dev/null 2>&1; then
    echo "build-mtd3-jffs2: mkfs.jffs2 not found (apt install mtd-utils)" >&2
    exit 1
fi

if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
    echo "build-mtd3-jffs2: $KERNEL_DIR has no built zImage -- build it first:" >&2
    echo "  cd $KERNEL_DIR && ARCH=arm CROSS_COMPILE=... make zImage modules" >&2
    exit 1
fi
KVER="$(cat "$KERNEL_DIR/include/config/kernel.release" 2>/dev/null || true)"
if [ -z "$KVER" ]; then
    echo "build-mtd3-jffs2: cannot determine kernel release (no include/config/kernel.release)" >&2
    exit 1
fi

STAGE="$(mktemp -d /tmp/piko-mtd3-jffs2.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
OVERLAY="$STAGE/overlay"
mkdir -p "$OVERLAY"

echo "==> staging overlay: kernel ($KVER) + modules + rootfs/"
mkdir -p "$OVERLAY/boot"
cp "$KERNEL_DIR/arch/arm/boot/zImage" "$OVERLAY/boot/zImage-full"

. "$REPO/tools/kernel/kernel-modules.sh"
for relpath in $AUDIO_MODULES; do
    dst="$OVERLAY/lib/modules/$KVER/zaurus-audio/$(basename "$relpath")"
    mkdir -p "$(dirname "$dst")"
    cp "$KERNEL_DIR/$relpath" "$dst"
done

WIFI_PCMCIA_SD_MODULES="$WIFI_MODULES
$SD_MODULES"
for relpath in $WIFI_PCMCIA_SD_MODULES; do
    src_rel="$(echo "$relpath" | sed 's#^kernel/##')"
    dst="$OVERLAY/lib/modules/$KVER/$relpath"
    mkdir -p "$(dirname "$dst")"
    cp "$KERNEL_DIR/$src_rel" "$dst"
done

( cd "$REPO/rootfs" && find . -type f ) | sed 's#^\./##' | while read -r rel; do
    dst="$OVERLAY/$rel"
    mkdir -p "$(dirname "$dst")"
    cp "$REPO/rootfs/$rel" "$dst"
    mode="$(stat -c '%a' "$REPO/rootfs/$rel")"
    chmod "$mode" "$dst"
done

if [ "${SKIP_X11:-0}" -ne 1 ]; then
    echo "==> building the X11/Matchbox stack (tools/userspace/build-x11-stack.sh)"
    "$REPO/tools/userspace/build-x11-stack.sh"

    echo "==> staging the X11/Matchbox payload into the image"
    PAYLOAD_DIR="${PAYLOAD_DIR:-/tmp/mb-payload}"
    "$REPO/tools/userspace/build-matchbox-payload.sh"
    cp -a "$PAYLOAD_DIR/." "$OVERLAY/"
else
    echo "==> SKIP_X11=1: not staging the X11/Matchbox payload"
fi

SSH_STAGE="${SSH_STAGE:-$REPO/userspace/stage-ssh}"
if [ -d "$SSH_STAGE" ]; then
    SSH_PAYLOAD_FILES="usr/bin/scp:usr/bin/scp:755
usr/libexec/sftp-server:usr/libexec/sftp-server:755
usr/bin/dbclient:usr/bin/dbclient:755
usr/bin/dropbearkey:usr/bin/dropbearkey:755"
    SSH_PAYLOAD_SERVER="usr/sbin/dropbear:usr/sbin/dropbear:755"
    ssh_list="$SSH_PAYLOAD_FILES"
    if [ "${PIKO_SSH_REPLACE_DROPBEAR:-0}" = "1" ]; then
        echo "==> PIKO_SSH_REPLACE_DROPBEAR=1: also overlaying the rebuilt dropbear"
        ssh_list="$ssh_list
$SSH_PAYLOAD_SERVER"
    fi
    for entry in $ssh_list; do
        src="$SSH_STAGE/${entry%%:*}"
        rest="${entry#*:}"
        rel="${rest%%:*}"
        if [ ! -f "$src" ]; then
            echo "build-mtd3-jffs2: $SSH_STAGE exists but $src is missing" >&2
            echo "  rerun tools/userspace/build-ssh.sh -- refusing to ship a half payload" >&2
            exit 1
        fi
        dst="$OVERLAY/$rel"
        mkdir -p "$(dirname "$dst")"
        cp "$src" "$dst"
        chmod "0${rest#*:}" "$dst"
        echo "    ssh payload: /$rel"
    done
else
    echo "build-mtd3-jffs2: WARNING -- no $SSH_STAGE, this image will have no" >&2
    echo "  scp/sftp-server. Run tools/userspace/build-ssh.sh first if that is not intended." >&2
fi

KEXEC_STAGE="${KEXEC_STAGE:-$REPO/userspace/stage-kexec}"
if [ ! -f "$KEXEC_STAGE/sbin/kexec" ]; then
    echo "build-mtd3-jffs2: no $KEXEC_STAGE/sbin/kexec -- run tools/userspace/build-kexec.sh first" >&2
    echo "  the bootstrap initramfs waits forever for /sbin/kexec on this image; refusing" >&2
    echo "  to ship one without it" >&2
    exit 1
fi
mkdir -p "$OVERLAY/sbin"
cp "$KEXEC_STAGE/sbin/kexec" "$OVERLAY/sbin/kexec"
chmod 0755 "$OVERLAY/sbin/kexec"
echo "    kexec: /sbin/kexec"

echo "==> unpacking base image $BASE_JFFS2 (via the real kernel jffs2 driver -- needs sudo)"
MERGED="$STAGE/merged"
sudo "$REPO/tools/scripts/jffs2-mount-extract.sh" "$BASE_JFFS2" "$MERGED"

echo "==> overlaying kernel + modules + rootfs/ on top of the unpacked base"
cp -a "$OVERLAY/." "$MERGED/"

echo "==> building fresh image from merged tree (eraseblock=$ERASEBLOCK)"
mkfs.jffs2 -r "$MERGED" -o "$OUT.partial" \
    -e "$ERASEBLOCK" -l -U -n -q -v 2>&1 | tail -20
mv "$OUT.partial" "$OUT"

md5sum "$BASE_JFFS2" "$OUT"
echo "==> done: $OUT ($(stat -c '%s' "$OUT") bytes, base was $(stat -c '%s' "$BASE_JFFS2") bytes)"
