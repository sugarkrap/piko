#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_DIR="${KERNEL_DIR:-$REPO/build/kernel/src/linux-7.1.4}"

if [ -n "${ROOT_IMG_OUT:-}" ] && ! command -v fakeroot >/dev/null 2>&1; then
    echo "build-rootfs: fakeroot not found (apt install fakeroot)" >&2
    exit 1
fi

if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
    echo "build-rootfs: $KERNEL_DIR has no built zImage" >&2
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
        echo "build-rootfs: these rootfs files are git-ignored:" >&2
        echo "$IGNORED" | sed 's/^/  /' >&2
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
    STAGE="$REPO/build/target" OUTDIR="$PIKO_SYNC_IPK_OUT" \
        "$REPO/tools/userspace/build-piko-sync.sh" --server-only

    PIKO_SYNC_BIN="$REPO/build/target/bin/piko-sync-server"
    if [ ! -f "$PIKO_SYNC_BIN" ]; then
        echo "build-rootfs: build-piko-sync.sh succeeded but there is no $PIKO_SYNC_BIN" >&2
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

SSH_STAGE="${SSH_STAGE:-$REPO/build/stage-ssh}"
if [ -d "$SSH_STAGE" ]; then
    ssh_list="usr/bin/scp:usr/bin/scp:755
usr/libexec/sftp-server:usr/libexec/sftp-server:755
usr/bin/dbclient:usr/bin/dbclient:755
usr/bin/dropbearkey:usr/bin/dropbearkey:755
usr/sbin/dropbear:usr/sbin/dropbear:755"
    for entry in $ssh_list; do
        src="$SSH_STAGE/${entry%%:*}"
        rest="${entry#*:}"
        rel="${rest%%:*}"
        if [ ! -f "$src" ]; then
            echo "build-rootfs: $SSH_STAGE exists but $src is missing" >&2
            exit 1
        fi
        dst="$OVERLAY/$rel"
        mkdir -p "$(dirname "$dst")"
        cp "$src" "$dst"
        chmod "0${rest#*:}" "$dst"
        echo "    ssh payload: /$rel"
    done
else
    echo "build-rootfs: WARNING -- no $SSH_STAGE, no scp/sftp-server in this image" >&2
fi

KEXEC_STAGE="${KEXEC_STAGE:-$REPO/build/stage-kexec}"
if [ ! -f "$KEXEC_STAGE/sbin/kexec" ]; then
    echo "build-rootfs: no $KEXEC_STAGE/sbin/kexec -- run tools/userspace/build-kexec.sh first" >&2
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
fbtext:usr/sbin/fbtext
zramswap:usr/sbin/zramswap
kill:usr/bin/kill
md5sum:usr/bin/md5sum
untar:usr/local/bin/untar
pikoemu:usr/local/bin/pikoemu"
for entry in $SRC_TOOLS; do
    src="$REPO/build/target/bin/${entry%%:*}"
    rel="${entry#*:}"
    if [ ! -f "$src" ]; then
        echo "build-rootfs: missing $src -- run tools/userspace/build-userspace.sh first" >&2
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

PIKOVIDEO_LIB="$REPO/build/target/usr/lib/libpikovideo.so.1"
if [ ! -f "$PIKOVIDEO_LIB" ]; then
    echo "build-rootfs: missing $PIKOVIDEO_LIB -- run tools/userspace/build-pikoemu.sh first" >&2
    exit 1
fi
mkdir -p "$OVERLAY/lib"
cp "$PIKOVIDEO_LIB" "$OVERLAY/lib/libpikovideo.so.1"
chmod 0755 "$OVERLAY/lib/libpikovideo.so.1"
echo "    pikoemu: /usr/local/bin/pikoemu + /lib/libpikovideo.so.1"

SDL_STAGE="${SDL_STAGE:-$REPO/build/stage-sdl-runtime}"
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


MPLAYER_STAGE="${MPLAYER_STAGE:-$REPO/build/stage-mplayer}"
if [ -f "$MPLAYER_STAGE/usr/bin/mplayer" ]; then
    mkdir -p "$OVERLAY/usr/bin"
    cp "$MPLAYER_STAGE/usr/bin/mplayer" "$OVERLAY/usr/bin/mplayer"
    chmod 0755 "$OVERLAY/usr/bin/mplayer"
    echo "    mplayer: /usr/bin/mplayer"
else
    echo "build-rootfs: WARNING -- no $MPLAYER_STAGE, piko-player ships with no backend" >&2
fi

PHONEME_STAGE="${PHONEME_STAGE:-$REPO/build/stage-phoneme}"
PHONEME_HOME="$PHONEME_STAGE/usr/local/lib/phoneme"
if [ ! -f "$PHONEME_HOME/bin/runMidlet" ]; then
    echo "==> building phoneME J2ME (tools/userspace/build-phoneme.sh)"
    "$REPO/tools/userspace/build-phoneme.sh"
fi
if [ ! -f "$PHONEME_HOME/bin/runMidlet" ]; then
    echo "build-rootfs: build-phoneme.sh succeeded but there is no $PHONEME_HOME/bin/runMidlet" >&2
    exit 1
fi
mkdir -p "$OVERLAY/usr/local/lib/phoneme"
cp -a "$PHONEME_HOME/." "$OVERLAY/usr/local/lib/phoneme/"
chmod 0755 "$OVERLAY/usr/local/lib/phoneme/bin/runMidlet"
echo "    phoneme: /usr/local/lib/phoneme (runMidlet + skins + appdb)"

mkdir -p "$OVERLAY/usr/local/bin"
cp "$REPO/userspace/src/phoneme-run" "$OVERLAY/usr/local/bin/phoneme-run"
chmod 0755 "$OVERLAY/usr/local/bin/phoneme-run"
echo "    phoneme: /usr/local/bin/phoneme-run (what the .desktop launchers exec)"

BEZEL_STAGE="${BEZEL_STAGE:-$REPO/build/stage-bezels}"
if [ ! -d "$BEZEL_STAGE/usr/local/.zaurus/bezels" ]; then
    echo "==> baking the shipped bezels (tools/userspace/build-bezels.sh)"
    "$REPO/tools/userspace/build-bezels.sh"
fi
if [ ! -d "$BEZEL_STAGE/usr/local/.zaurus/bezels" ]; then
    echo "build-rootfs: build-bezels.sh succeeded but there is no $BEZEL_STAGE/usr/local/.zaurus/bezels" >&2
    exit 1
fi
cp -a "$BEZEL_STAGE/." "$OVERLAY/"
echo "    bezels: /usr/local/.zaurus/bezels ($(find "$OVERLAY/usr/local/.zaurus/bezels" -name '*.pkbz' | wc -l) files)"

TIMIDITY_STAGE="${TIMIDITY_STAGE:-$REPO/build/stage-timidity}"
if [ ! -f "$TIMIDITY_STAGE/timidity.cfg" ]; then
    echo "==> building the MIDI instruments (tools/userspace/build-timidity-patches.sh)"
    "$REPO/tools/userspace/build-timidity-patches.sh"
fi
if [ ! -f "$TIMIDITY_STAGE/timidity.cfg" ]; then
    echo "build-rootfs: build-timidity-patches.sh succeeded but there is no $TIMIDITY_STAGE/timidity.cfg" >&2
    exit 1
fi
mkdir -p "$OVERLAY/usr/share/timidity"
cp -a "$TIMIDITY_STAGE/." "$OVERLAY/usr/share/timidity/"
echo "    timidity: /usr/share/timidity ($(find "$OVERLAY/usr/share/timidity" -name '*.pat' | wc -l) patches)"

GLIBC_STAGE="${GLIBC_STAGE:-$REPO/build/stage-glibc}"
if [ ! -f "$GLIBC_STAGE/lib/ld-linux.so.3" ]; then
    echo "==> building the GNU C library (tools/userspace/build-glibc-part.sh)"
    "$REPO/tools/userspace/build-glibc-part.sh"
fi
if [ ! -f "$GLIBC_STAGE/lib/ld-linux.so.3" ]; then
    echo "build-rootfs: build-glibc-part.sh succeeded but there is no $GLIBC_STAGE/lib/ld-linux.so.3" >&2
    exit 1
fi
mkdir -p "$OVERLAY/usr/glibc"
cp -a "$GLIBC_STAGE/lib" "$OVERLAY/usr/glibc/"
echo "    glibc: /usr/glibc/lib (alongside uClibc)"

BUSYBOX_STAGE="${BUSYBOX_ROOT_STAGE:-$REPO/build/stage-busybox}"
if [ ! -f "$BUSYBOX_STAGE/bin/busybox" ]; then
    echo "==> building busybox for the root (tools/userspace/build-busybox-root.sh)"
    "$REPO/tools/userspace/build-busybox-root.sh"
fi
if [ ! -f "$BUSYBOX_STAGE/bin/busybox" ]; then
    echo "build-rootfs: build-busybox-root.sh succeeded but there is no $BUSYBOX_STAGE/bin/busybox" >&2
    exit 1
fi
cp -a "$BUSYBOX_STAGE/." "$OVERLAY/"
echo "    busybox: /bin/busybox built from source ($(find "$BUSYBOX_STAGE" -type l | wc -l) applets)"

WIRELESS_STAGE="${WIRELESS_STAGE:-$REPO/build/target}"
if [ ! -x "$WIRELESS_STAGE/usr/sbin/iwconfig" ]; then
    echo "==> building the wireless tools (tools/userspace/build-libiw.sh)"
    "$REPO/tools/userspace/build-libiw.sh"
fi
if [ ! -x "$WIRELESS_STAGE/usr/sbin/iwconfig" ]; then
    echo "build-rootfs: build-libiw.sh succeeded but there is no $WIRELESS_STAGE/usr/sbin/iwconfig" >&2
    exit 1
fi
mkdir -p "$OVERLAY/usr/sbin"
for tool in iwconfig iwlist iwgetid iwpriv iwspy iwevent; do
    cp "$WIRELESS_STAGE/usr/sbin/$tool" "$OVERLAY/usr/sbin/$tool"
    chmod 0755 "$OVERLAY/usr/sbin/$tool"
done
echo "    wireless: /usr/sbin/iw* built from source"

ALSA_RUNTIME="${ALSA_RUNTIME:-$REPO/build/stage-alsa-runtime}"
if [ -d "$ALSA_RUNTIME/usr/share/alsa" ]; then
    mkdir -p "$OVERLAY/usr/share" "$OVERLAY/var/lib/alsa"
    cp -a "$ALSA_RUNTIME/usr/share/alsa" "$OVERLAY/usr/share/"
    echo "    alsa: /usr/share/alsa (config data)"
    for b in bin/aplay bin/amixer sbin/alsactl; do
        if [ ! -f "$ALSA_RUNTIME/usr/$b" ]; then
            echo "build-rootfs: WARNING -- no $ALSA_RUNTIME/usr/$b, not shipping it" >&2
            continue
        fi
        mkdir -p "$OVERLAY/usr/$(dirname "$b")"
        cp "$ALSA_RUNTIME/usr/$b" "$OVERLAY/usr/$b"
        chmod 0755 "$OVERLAY/usr/$b"
        echo "    alsa: /usr/$b"
    done
else
    echo "build-rootfs: WARNING -- no $ALSA_RUNTIME/usr/share/alsa, mb-volume gets no mixer" >&2
fi

OPKG_BIN="${OPKG_BIN:-$REPO/build/target/usr/bin/opkg}"
if [ -f "$OPKG_BIN" ]; then
    mkdir -p "$OVERLAY/usr/bin"
    cp "$OPKG_BIN" "$OVERLAY/usr/bin/opkg"
    chmod 0755 "$OVERLAY/usr/bin/opkg"
    echo "    opkg: /usr/bin/opkg"
else
    echo "build-rootfs: WARNING -- no $OPKG_BIN, the pkg* wrappers will be non-functional" >&2
fi

echo "==> assembling the root tree"
MERGED="$STAGE/merged"
rm -rf "$MERGED"
mkdir -p "$MERGED"
for d in bin boot dev etc home lib media mnt proc root sbin sys tmp usr var \
         usr/bin usr/sbin usr/lib usr/share usr/local var/log var/run; do
    mkdir -p "$MERGED/$d"
done
chmod 1777 "$MERGED/tmp"
cp -a "$OVERLAY/." "$MERGED/"

echo "==> verifying the merged tree provides what its own boot scripts call"
REFS="$(cat "$REPO/rootfs/etc/init.d/rcS" "$REPO/rootfs/etc/init.d/xsession" \
             "$REPO/rootfs/etc/mdev.conf" "$REPO/rootfs/etc/inittab" \
             "$REPO/rootfs"/usr/sbin/* \
             "$REPO/userspace/src/piko-sync/emulation_db.h" \
             "$REPO/build/target/bin/piko-sync-server.cxx" 2>/dev/null \
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
    exit 1
fi
echo "    no dangling library symlinks"

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
