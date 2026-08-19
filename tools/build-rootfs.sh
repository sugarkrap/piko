#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BASE_JFFS2="${1:-$REPO/flash/base.jffs2}"
OUT="${2:-$REPO/flash/mtd3.jffs2}"

KERNEL_DIR="${KERNEL_DIR:-$REPO/kernel-src/linux-7.1.4}"
ERASEBLOCK="${ERASEBLOCK:-0x4000}"

if [ ! -f "$BASE_JFFS2" ]; then
    echo "build-rootfs: base image not found: $BASE_JFFS2" >&2
    exit 1
fi
case "$(file -b "$BASE_JFFS2" 2>/dev/null)" in
    *jffs2*little*endian*) ;;
    *) echo "build-rootfs: $BASE_JFFS2 doesn't look like a little-endian JFFS2 image, refusing to guess" >&2; exit 1 ;;
esac

if [ "${SKIP_JFFS2:-0}" -ne 1 ] && ! command -v mkfs.jffs2 >/dev/null 2>&1; then
    echo "build-rootfs: mkfs.jffs2 not found (apt install mtd-utils)" >&2
    exit 1
fi

if [ -n "${ROOT_IMG_OUT:-}" ] && ! command -v fakeroot >/dev/null 2>&1; then
    echo "build-rootfs: fakeroot not found (apt install fakeroot)" >&2
    echo "  mke2fs -d records the uid of whoever staged the tree, so without it every" >&2
    echo "  file in the image belongs to the build account instead of root, and dropbear" >&2
    echo "  refuses /root/.ssh/authorized_keys on the installed board" >&2
    exit 1
fi

if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
    echo "build-rootfs: $KERNEL_DIR has no built zImage -- build it first:" >&2
    echo "  cd $KERNEL_DIR && ARCH=arm CROSS_COMPILE=... make zImage modules" >&2
    exit 1
fi
KVER="$(cat "$KERNEL_DIR/include/config/kernel.release" 2>/dev/null || true)"
if [ -z "$KVER" ]; then
    echo "build-rootfs: cannot determine kernel release (no include/config/kernel.release)" >&2
    exit 1
fi

if command -v git >/dev/null 2>&1 && [ -d "$REPO/.git" ]; then
    IGNORED="$(cd "$REPO" && git ls-files --others --ignored --exclude-standard rootfs/ 2>/dev/null || true)"
    if [ -n "$IGNORED" ]; then
        echo "build-rootfs: these rootfs files are git-ignored, so a clean checkout would not have them:" >&2
        echo "$IGNORED" | sed 's/^/  /' >&2
        echo "  add a !negation to .gitignore, or delete them" >&2
        exit 1
    fi
fi

STAGE="$(mktemp -d /tmp/piko-rootfs.XXXXXX)"
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
$SD_MODULES
$NAND_MODULES
$CPUFREQ_MODULES"
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

    echo "==> building piko-sync-server (tools/userspace/build-piko-sync.sh --server-only)"
    PIKO_SYNC_IPK_OUT="$STAGE/ipk"
    mkdir -p "$PIKO_SYNC_IPK_OUT"
    STAGE="$REPO/userspace/stage-target" OUTDIR="$PIKO_SYNC_IPK_OUT" \
        "$REPO/tools/userspace/build-piko-sync.sh" --server-only

    PIKO_SYNC_BIN="$REPO/userspace/src/piko-sync/piko-sync-server"
    if [ ! -f "$PIKO_SYNC_BIN" ]; then
        echo "build-rootfs: build-piko-sync.sh reported success but there is no" >&2
        echo "  $PIKO_SYNC_BIN -- refusing to ship a launcher with no binary" >&2
        exit 1
    fi
    mkdir -p "$OVERLAY/usr/bin" "$OVERLAY/usr/share/applications" "$OVERLAY/usr/share/pixmaps"
    cp "$PIKO_SYNC_BIN" "$OVERLAY/usr/bin/piko-sync-server"
    chmod 0755 "$OVERLAY/usr/bin/piko-sync-server"
    cp "$REPO/userspace/desktop/piko-sync-server.desktop" \
        "$OVERLAY/usr/share/applications/piko-sync-server.desktop"
    cp "$REPO/userspace/desktop/piko-sync-server.png" \
        "$OVERLAY/usr/share/pixmaps/piko-sync-server.png"
    cp "$REPO/userspace/desktop/rom.png" \
        "$OVERLAY/usr/share/pixmaps/rom.png"
    echo "    piko-sync: /usr/bin/piko-sync-server (+ launcher, icon)"

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
            echo "build-rootfs: $SSH_STAGE exists but $src is missing" >&2
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
    echo "build-rootfs: WARNING -- no $SSH_STAGE, this image will have no" >&2
    echo "  scp/sftp-server. Run tools/userspace/build-ssh.sh first if that is not intended." >&2
fi

KEXEC_STAGE="${KEXEC_STAGE:-$REPO/userspace/stage-kexec}"
if [ ! -f "$KEXEC_STAGE/sbin/kexec" ]; then
    echo "build-rootfs: no $KEXEC_STAGE/sbin/kexec -- run tools/userspace/build-kexec.sh first" >&2
    echo "  the bootstrap initramfs waits forever for /sbin/kexec on this image; refusing" >&2
    echo "  to ship one without it" >&2
    exit 1
fi
mkdir -p "$OVERLAY/sbin"
cp "$KEXEC_STAGE/sbin/kexec" "$OVERLAY/sbin/kexec"
chmod 0755 "$OVERLAY/sbin/kexec"
echo "    kexec: /sbin/kexec"

SRC_TOOLS="brightd:usr/sbin/brightd
cardswap:usr/sbin/cardswap
flipd:usr/sbin/flipd
hwclock:usr/sbin/hwclock
mhz:usr/sbin/mhz
ntpsync:usr/sbin/ntpsync
piko-splash:usr/sbin/piko-splash
pkillx:usr/sbin/pkillx
vol:usr/sbin/vol
zramswap:usr/sbin/zramswap
kill:usr/bin/kill
md5sum:usr/bin/md5sum
untar:usr/local/bin/untar"
for entry in $SRC_TOOLS; do
    src="$REPO/userspace/src/${entry%%:*}"
    rel="${entry#*:}"
    if [ ! -f "$src" ]; then
        echo "build-rootfs: missing $src -- run tools/userspace/build-userspace.sh first" >&2
        echo "  rootfs/etc/init.d/rcS and rootfs/etc/mdev.conf silently skip these when absent," >&2
        echo "  which ships a board with no swap/backlight/rotation/clock; refusing" >&2
        exit 1
    fi
    dst="$OVERLAY/$rel"
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst"
    chmod 0755 "$dst"
    echo "    tool: /$rel"
done

mkdir -p "$OVERLAY/usr/local/bin"
ln -sf /usr/bin/kill "$OVERLAY/usr/local/bin/kill"
echo "    compat: /usr/local/bin/kill -> /usr/bin/kill"

SDL_STAGE="${SDL_STAGE:-$REPO/userspace/stage-sdl-runtime}"
if [ -d "$SDL_STAGE" ]; then
    SDL_SONAME="libSDL-1.2.so.0"
    if [ ! -e "$SDL_STAGE/usr/lib/$SDL_SONAME" ]; then
        echo "build-rootfs: $SDL_STAGE exists but has no $SDL_SONAME -- rerun tools/userspace/build-sdl.sh" >&2
        exit 1
    fi
    SDL_LIB="$(basename "$(readlink -f "$SDL_STAGE/usr/lib/$SDL_SONAME")")"
    mkdir -p "$OVERLAY/lib" "$OVERLAY/usr/bin"
    cp "$SDL_STAGE/usr/lib/$SDL_LIB" "$OVERLAY/lib/$SDL_LIB"
    chmod 0755 "$OVERLAY/lib/$SDL_LIB"
    [ "$SDL_LIB" = "$SDL_SONAME" ] || ln -sf "$SDL_LIB" "$OVERLAY/lib/$SDL_SONAME"
    echo "    sdl: /lib/$SDL_LIB (soname $SDL_SONAME)"

    for extra in libSDL_image-1.2.so.0 libSDL_mixer-1.2.so.0; do
        [ -e "$SDL_STAGE/usr/lib/$extra" ] || continue
        real="$(basename "$(readlink -f "$SDL_STAGE/usr/lib/$extra")")"
        cp "$SDL_STAGE/usr/lib/$real" "$OVERLAY/lib/$real"
        chmod 0755 "$OVERLAY/lib/$real"
        [ "$real" = "$extra" ] || ln -sf "$real" "$OVERLAY/lib/$extra"
        echo "    sdl: /lib/$real (soname $extra)"
    done
    for b in pikalibrate sdltest; do
        if [ ! -f "$SDL_STAGE/usr/bin/$b" ]; then
            echo "build-rootfs: $SDL_STAGE exists but $b is missing -- rerun tools/userspace/build-sdl.sh" >&2
            exit 1
        fi
        cp "$SDL_STAGE/usr/bin/$b" "$OVERLAY/usr/bin/$b"
        chmod 0755 "$OVERLAY/usr/bin/$b"
        echo "    sdl: /usr/bin/$b"
    done
else
    echo "build-rootfs: WARNING -- no $SDL_STAGE, pikalibrate will be a dead launcher" >&2
fi


PHONEME_STAGE="${PHONEME_STAGE:-$REPO/userspace/stage-phoneme}"
PHONEME_HOME="$PHONEME_STAGE/usr/local/lib/phoneme"
if [ ! -f "$PHONEME_HOME/bin/runMidlet" ]; then
    echo "==> building phoneME J2ME (tools/userspace/build-phoneme.sh)"
    "$REPO/tools/userspace/build-phoneme.sh"
fi
if [ ! -f "$PHONEME_HOME/bin/runMidlet" ]; then
    echo "build-rootfs: build-phoneme.sh reported success but there is no" >&2
    echo "  $PHONEME_HOME/bin/runMidlet -- refusing to ship a J2ME-less image" >&2
    exit 1
fi
mkdir -p "$OVERLAY/usr/local/lib/phoneme"
cp -a "$PHONEME_HOME/." "$OVERLAY/usr/local/lib/phoneme/"
chmod 0755 "$OVERLAY/usr/local/lib/phoneme/bin/runMidlet"
echo "    phoneme: /usr/local/lib/phoneme (runMidlet + skins + appdb)"

if [ ! -f "$OVERLAY/etc/zaurus/parts.cfg" ]; then
    echo "build-rootfs: rootfs/etc/zaurus/parts.cfg did not reach the overlay" >&2
    echo "  without the software-part catalog matchbox-apprun refuses to launch" >&2
    echo "  anything that declares X-Piko-Parts" >&2
    exit 1
fi
echo "    parts: /etc/zaurus/parts.cfg ($(grep -c . "$OVERLAY/etc/zaurus/parts.cfg") entries)"

ALSA_RUNTIME="${ALSA_RUNTIME:-$REPO/userspace/stage-alsa-runtime}"
if [ -d "$ALSA_RUNTIME/usr/share/alsa" ]; then
    mkdir -p "$OVERLAY/usr/share" "$OVERLAY/var/lib/alsa"
    cp -a "$ALSA_RUNTIME/usr/share/alsa" "$OVERLAY/usr/share/"
    echo "    alsa: /usr/share/alsa (config data)"
    for b in aplay amixer alsactl; do
        if [ -f "$ALSA_RUNTIME/usr/bin/$b" ]; then
            cp "$ALSA_RUNTIME/usr/bin/$b" "$OVERLAY/usr/bin/$b"
            chmod 0755 "$OVERLAY/usr/bin/$b"
            echo "    alsa: /usr/bin/$b"
        fi
    done
else
    echo "build-rootfs: WARNING -- no $ALSA_RUNTIME/usr/share/alsa, mb-volume will fail to" >&2
    echo "  open the mixer (statically linked libasound still reads /usr/share/alsa/alsa.conf)" >&2
fi

OPKG_BIN="${OPKG_BIN:-$REPO/userspace/stage-target/usr/bin/opkg}"
if [ -f "$OPKG_BIN" ]; then
    mkdir -p "$OVERLAY/usr/bin"
    cp "$OPKG_BIN" "$OVERLAY/usr/bin/opkg"
    chmod 0755 "$OVERLAY/usr/bin/opkg"
    echo "    opkg: /usr/bin/opkg"
else
    echo "build-rootfs: WARNING -- no $OPKG_BIN, the pkg* wrappers will be non-functional" >&2
fi

echo "==> unpacking base image $BASE_JFFS2 (via the real kernel jffs2 driver -- needs sudo)"
MERGED="$STAGE/merged"
sudo "$REPO/tools/scripts/jffs2-mount-extract.sh" "$BASE_JFFS2" "$MERGED"

echo "==> overlaying kernel + modules + rootfs/ on top of the unpacked base"
cp -a "$OVERLAY/." "$MERGED/"

echo "==> verifying the merged tree provides what its own boot scripts call"
REFS="$(cat "$REPO/rootfs/etc/init.d/rcS" "$REPO/rootfs/etc/init.d/xsession" \
             "$REPO/rootfs/etc/mdev.conf" "$REPO/rootfs/etc/inittab" \
             "$REPO/rootfs"/usr/sbin/* 2>/dev/null \
    | grep -aoE '(^|[^-[:alnum:]_./])/(usr/local/bin|usr/sbin|usr/bin|sbin|bin)/[A-Za-z0-9._-]*[A-Za-z0-9]' \
    | grep -aoE '/(usr/local/bin|usr/sbin|usr/bin|sbin|bin)/[A-Za-z0-9._-]*[A-Za-z0-9]' \
    | sort -u)"
DESKTOP_REFS="$(cat "$MERGED"/usr/share/applications/*.desktop 2>/dev/null \
    | sed -n 's/^Exec=//p' | tr ' ' '\n' \
    | grep -aoE '^/(usr|bin|sbin|opt)/[A-Za-z0-9._/-]*[A-Za-z0-9]' | sort -u)"

refmissing=0
for r in $REFS $DESKTOP_REFS; do
    [ -e "$MERGED$r" ] && continue
    [ -L "$MERGED$r" ] && continue
    echo "build-rootfs: boot scripts reference $r but the image does not provide it" >&2
    refmissing=1
done
if [ "$refmissing" -ne 0 ]; then
    echo "  these are all test -x / [ -x ] guarded, so the board would boot and silently" >&2
    echo "  skip the feature instead of failing -- refusing to ship that" >&2
    exit 1
fi
echo "    every binary referenced by boot scripts and .desktop launchers is present"

badlink=0
for l in $(find "$MERGED/lib" "$MERGED/usr/lib" -type l -name '*.so*' 2>/dev/null); do
    [ -e "$l" ] && continue
    echo "build-rootfs: dangling library symlink ${l#$MERGED} -> $(readlink "$l")" >&2
    badlink=1
done
if [ "$badlink" -ne 0 ]; then
    echo "  a self-referential or broken soname link makes every dependent binary fail" >&2
    echo "  with a misleading \"not found\" -- refusing to ship that" >&2
    exit 1
fi
echo "    no dangling library symlinks"

if [ "${SKIP_JFFS2:-0}" -ne 1 ]; then
    echo "==> building fresh image from merged tree (eraseblock=$ERASEBLOCK)"
    mkfs.jffs2 -r "$MERGED" -o "$OUT.partial" \
        -e "$ERASEBLOCK" -l -U -n -q -v 2>&1 | tail -20
    mv "$OUT.partial" "$OUT"

    md5sum "$BASE_JFFS2" "$OUT"
    echo "==> done: $OUT ($(stat -c '%s' "$OUT") bytes, base was $(stat -c '%s' "$BASE_JFFS2") bytes)"
fi

if [ -n "${ROOT_IMG_OUT:-}" ]; then
    if ! command -v mke2fs >/dev/null 2>&1; then
        echo "build-rootfs: mke2fs not found (apt install e2fsprogs)" >&2
        exit 1
    fi
    tree_kb="$(du -sk "$MERGED" | while read -r n _; do echo "$n"; break; done)"
    img_mb="${ROOT_IMG_SIZE_MB:-$(( tree_kb / 1024 * 14 / 10 + 32 ))}"
    echo "==> building the ext2 root image (${img_mb}M for a $(( tree_kb / 1024 ))M tree)"
    rm -f "$ROOT_IMG_OUT.partial"
    truncate -s "${img_mb}M" "$ROOT_IMG_OUT.partial"
    fakeroot sh -c "chown -R 0:0 '$MERGED' && mke2fs -F -q -t ext2 -L pikoroot -d '$MERGED' '$ROOT_IMG_OUT.partial'"
    mv "$ROOT_IMG_OUT.partial" "$ROOT_IMG_OUT"
    echo "==> done: $ROOT_IMG_OUT ($(stat -c '%s' "$ROOT_IMG_OUT") bytes)"
fi

if [ "${SKIP_JFFS2:-0}" -ne 1 ]; then
    echo ""
    IMAGE="$OUT" BASE_IMAGE="$BASE_JFFS2" "$REPO/tools/nand-budget.sh" "$OVERLAY"
fi
