#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
STAGE="$REPO/userspace/stage-target"
HOST_TRIPLET="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST_TRIPLET/bin}"
SYSROOT="$REPO/toolchain/x-tools/$HOST_TRIPLET/$HOST_TRIPLET/sysroot"

PAYLOAD="${PAYLOAD_DIR:-/tmp/mb-payload}"
TARBALL="${PAYLOAD_TAR:-/tmp/matchbox-payload.tar}"

D_WM="${D_WM:-/tmp/mbwm-stage}"
D_DESKTOP="${D_DESKTOP:-/tmp/mb-stage-desktop}"
D_PANEL="${D_PANEL:-/tmp/mb-stage-panel}"
D_COMMON="${D_COMMON:-/tmp/mb-stage-common}"
D_CARD="${D_CARD:-/tmp/mb-stage-card}"
D_VOLUME="${D_VOLUME:-/tmp/mb-stage-volume}"
D_BRIGHT="${D_BRIGHT:-/tmp/mb-stage-brightness}"
D_PIKAFFEINE="${D_PIKAFFEINE:-/tmp/mb-stage-pikaffeine}"

DEPLOY=0
TARGET=""
ADAPTER=""
SKIP_ST=0
while [ $# -gt 0 ]; do
    case "$1" in
        --deploy)  DEPLOY=1; shift
                   case "${1:-}" in -*|"") ;; *) TARGET="$1"; shift ;; esac ;;
        --adapter) ADAPTER="${2:?--adapter needs an interface}"; shift 2 ;;
        --skip-st) SKIP_ST=1; shift ;;
        -h|--help) sed -n '3,40p' "$0"; exit 0 ;;
        *) echo "FAILED: unknown option: $1" >&2; exit 1 ;;
    esac
done
TARGET="${TARGET:-root@10.208.47.72}"

STRIP="$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-strip"
[ -x "$STRIP" ] || { echo "FAILED: no strip at $STRIP" >&2; exit 1; }

LIBS="libX11 libXext libXpm libxcb libXau libXdmcp libz libexpat libpng16 \
libfreetype libfontconfig libXrender libXft libmb libpixman-1 libXfont \
libfontenc libxkbfile libmd libfltk libfltk_images libfltk_forms"

XSERVER_BIN="${XSERVER_BIN:-$REPO/userspace/src/xserver/hw/kdrive/fbdev/Xfbdev}"
XKBCOMP_BIN="${XKBCOMP_BIN:-$REPO/userspace/src/xkbcomp/xkbcomp}"
XEV_BIN="${XEV_BIN:-$REPO/userspace/src/xev/xev}"
ST_BIN="${ST_BIN:-$REPO/userspace/src/st/st}"
TOASTERS_BIN="${TOASTERS_BIN:-$REPO/userspace/src/toasters}"
FLTKTEST_BIN="${FLTKTEST_BIN:-$STAGE/usr/bin/fltktest}"
FBRUN_BIN="${FBRUN_BIN:-$STAGE/usr/bin/matchbox-apprun}"
PIKOSTORE_BIN="${PIKOSTORE_BIN:-$STAGE/usr/bin/pikostore}"
FOUND_BIN="${FOUND_BIN:-$STAGE/usr/bin/found-file-browser}"
WALLPAPER_PICKER_BIN="${WALLPAPER_PICKER_BIN:-$STAGE/usr/bin/mb-wallpaper-picker}"
PIKO_SETTINGS_BIN="${PIKO_SETTINGS_BIN:-$STAGE/usr/bin/piko-settings}"
PIKO_PLAYER_BIN="${PIKO_PLAYER_BIN:-$STAGE/usr/bin/piko-player}"

echo "==> assembling into $PAYLOAD"
rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD/lib" "$PAYLOAD/usr" "$PAYLOAD/etc"

for base in $LIBS; do
    found=0
    for real in $(ls "$STAGE/usr/lib/" 2>/dev/null | grep -E "^${base}\.so\.[0-9]+(\.[0-9]+)*$"); do
        cp "$STAGE/usr/lib/$real" "$PAYLOAD/lib/"
        soname="$(echo "$real" | sed -E 's/^(.*\.so\.[0-9]+)\..*$/\1/')"
        [ "$soname" = "$real" ] || ln -sf "$real" "$PAYLOAD/lib/$soname"
        echo "    lib: $real"
        found=1
    done
    if [ "$found" -eq 0 ]; then
        echo "FAILED: $base not in $STAGE/usr/lib -- build it first" >&2
        exit 1
    fi
done

cp "$SYSROOT/lib/libgcc_s.so.1" "$PAYLOAD/lib/"
cp -L "$SYSROOT/lib/libstdc++.so.6" "$PAYLOAD/lib/libstdc++.so.6"

UCLIBC_LD_REAL="$(basename "$(readlink -f "$SYSROOT/lib/ld-uClibc.so.0")")"
UCLIBC_C_REAL="$(basename "$(readlink -f "$SYSROOT/lib/libc.so.0")")"
for real in "$UCLIBC_LD_REAL" "$UCLIBC_C_REAL"; do
    if [ ! -f "$SYSROOT/lib/$real" ]; then
        echo "FAILED: uClibc runtime $real not in $SYSROOT/lib" >&2
        exit 1
    fi
    cp "$SYSROOT/lib/$real" "$PAYLOAD/lib/$real"
    chmod 0755 "$PAYLOAD/lib/$real"
    echo "    lib: $real"
done
ln -sf "$UCLIBC_LD_REAL" "$PAYLOAD/lib/ld-uClibc.so.1"
ln -sf ld-uClibc.so.1   "$PAYLOAD/lib/ld-uClibc.so.0"
ln -sf "$UCLIBC_C_REAL"  "$PAYLOAD/lib/libc.so.0"
ln -sf "$UCLIBC_C_REAL"  "$PAYLOAD/lib/libc.so.1"

for d in "$D_WM" "$D_DESKTOP" "$D_PANEL" "$D_COMMON" "$D_CARD" "$D_VOLUME" \
         "$D_BRIGHT" "$D_PIKAFFEINE"; do
    if [ ! -d "$d" ]; then
        echo "FAILED: missing component DESTDIR: $d" >&2
        echo "Build that component first." >&2
        exit 1
    fi
    [ -d "$d/usr" ] && cp -a "$d/usr/." "$PAYLOAD/usr/"
    [ -d "$d/etc" ] && cp -a "$d/etc/." "$PAYLOAD/etc/"
    echo "    merged: $d"
done

mkdir -p "$PAYLOAD/usr/local/bin" "$PAYLOAD/usr/bin" "$PAYLOAD/usr/sbin"
BINS="$XSERVER_BIN:usr/local/bin/Xfbdev \
$XKBCOMP_BIN:usr/bin/xkbcomp \
$XEV_BIN:usr/local/bin/xev \
$TOASTERS_BIN:usr/local/bin/toasters \
$FLTKTEST_BIN:usr/local/bin/fltktest \
$PIKOSTORE_BIN:usr/local/bin/pikostore \
$FOUND_BIN:usr/local/bin/found-file-browser \
$WALLPAPER_PICKER_BIN:usr/local/bin/mb-wallpaper-picker \
$PIKO_SETTINGS_BIN:usr/local/bin/piko-settings \
$PIKO_PLAYER_BIN:usr/local/bin/piko-player \
$FBRUN_BIN:usr/sbin/matchbox-apprun"
if [ "$SKIP_ST" -eq 0 ]; then
    BINS="$BINS $ST_BIN:usr/local/bin/st"
else
    echo "    --skip-st: leaving st out of the payload"
fi
for spec in $BINS; do
    src="${spec%:*}"; dst="${spec##*:}"
    if [ ! -f "$src" ]; then
        echo "FAILED: missing $src -- build that component first" >&2
        echo "tools/userspace/build-x11-stack.sh builds every one of these; if it ran" >&2
        echo "and this is still missing, that is the bug, not your setup." >&2
        exit 1
    fi
    cp "$src" "$PAYLOAD/$dst"
    echo "    bin: $dst"
done

ln -sf matchbox-apprun "$PAYLOAD/usr/sbin/matchbox-fbrun"
echo "    compat: /usr/sbin/matchbox-fbrun -> matchbox-apprun"
ln -sf matchbox-apprun "$PAYLOAD/usr/sbin/matchbox-heavyrun"
echo "    compat: /usr/sbin/matchbox-heavyrun -> matchbox-apprun"

mkdir -p "$PAYLOAD/usr/share/X11" "$PAYLOAD/etc/X11"
cp -a "$STAGE/usr/share/X11/xkb" "$PAYLOAD/usr/share/X11/"
cp "$REPO/userspace/xkb/symbols/zaurus" "$PAYLOAD/usr/share/X11/xkb/symbols/zaurus"
cp "$REPO/userspace/xkb/zaurus.xkb"     "$PAYLOAD/etc/X11/zaurus.xkb"

mkdir -p "$PAYLOAD/usr/share/fonts/truetype/dejavu"
cp "$STAGE"/usr/share/fonts/truetype/dejavu/*.ttf \
   "$PAYLOAD/usr/share/fonts/truetype/dejavu/"
cp -a "$STAGE/etc/fonts" "$PAYLOAD/etc/"

mkdir -p "$PAYLOAD/usr/share/backgrounds"
cp "$REPO/userspace/backgrounds/piko-default.png" \
   "$PAYLOAD/usr/share/backgrounds/piko-default.png"

mkdir -p "$PAYLOAD/etc/matchbox"
cp "$REPO/modules/x11/matchbox-session" "$PAYLOAD/etc/matchbox/session"
chmod 755 "$PAYLOAD/etc/matchbox/session"

applets="$(sh -c "$(grep '^APPLETS=' "$REPO/modules/x11/matchbox-session")
                  echo \"\$APPLETS\"")"
applets="$(printf '%s\n' "$applets" | tr ',' '\n' | while read -r entry; do
    set -- $entry
    [ -n "${1:-}" ] && echo "$1"
done)"
[ -n "$applets" ] || { echo "FAILED: parsed no applets from modules/x11/matchbox-session" >&2; exit 1; }
for a in $applets; do
    if [ ! -f "$PAYLOAD/usr/bin/$a" ]; then
        echo "FAILED: /etc/matchbox/session runs $a but it is not in the payload." >&2
        echo "Rebuild matchbox-panel (mb-applet-battery needs --enable-proc-apm)" >&2
        echo "or drop it from modules/x11/matchbox-session." >&2
        exit 1
    fi
    echo "    applet: $a"
done

mkdir -p "$PAYLOAD/usr/share/applications" "$PAYLOAD/usr/share/pixmaps"
LAUNCHERS="piko-settings piko-player pikalibrate pikostore found-file-browser mb-wallpaper-picker suspend reboot gototty"
if [ "$SKIP_ST" -eq 0 ]; then
    LAUNCHERS="st xev $LAUNCHERS"
else
    echo "    --skip-st: leaving out the st and xev launchers (both exec st)"
fi
for app in $LAUNCHERS; do
    cp "$REPO/userspace/desktop/$app.desktop" \
       "$PAYLOAD/usr/share/applications/$app.desktop"
    cp "$REPO/userspace/desktop/$app.png" \
       "$PAYLOAD/usr/share/pixmaps/$app.png"
    echo "    launcher: $app"
done

echo "==> pruning"
find "$PAYLOAD" -name "*.la" -delete
rm -rf "$PAYLOAD/usr/include" "$PAYLOAD/usr/lib/pkgconfig"

echo "==> stripping"
find "$PAYLOAD" -type f | while read -r f; do
    case "$(file -b "$f")" in
        ELF*) chmod u+w "$f"; "$STRIP" --strip-unneeded "$f" 2>/dev/null || true ;;
    esac
done

echo "==> verifying"
if find "$PAYLOAD" -type f -exec file {} \; | grep "ELF" | grep -qv "ARM"; then
    echo "FAILED: non-ARM ELF in payload:" >&2
    find "$PAYLOAD" -type f -exec file {} \; | grep "ELF" | grep -v "ARM" >&2
    exit 1
fi
NEEDED="$(find "$PAYLOAD" -type f | while read -r f; do
    case "$(file -b "$f")" in
        ELF*) "$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-readelf" -d "$f" 2>/dev/null \
                | grep -oE '\[lib[^]]+\]' | tr -d '[]' ;;
    esac
done | sort -u)"
missing=0
for n in $NEEDED; do
    [ -e "$PAYLOAD/lib/$n" ] && continue
    echo "FAILED: nothing provides $n" >&2
    missing=1
done

INTERPS="$(find "$PAYLOAD" -type f | while read -r f; do
    case "$(file -b "$f")" in
        ELF*) LC_ALL=C "$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-readelf" -l "$f" 2>/dev/null \
                | sed -nE 's/.*interpreter: (\/[^]]+)\]/\1/p' ;;
    esac
done | sort -u)"
for i in $INTERPS; do
    [ -e "$PAYLOAD$i" ] && continue
    echo "FAILED: nothing provides the ELF interpreter $i" >&2
    echo "  a flashed image has no uClibc runtime unless this payload ships it" >&2
    missing=1
done

[ "$missing" -eq 0 ] || exit 1
echo "    all DT_NEEDED + ELF interpreters satisfied; all ELF are ARM"

tar --format=ustar -cf "$TARBALL" -C "$PAYLOAD" .
echo "==> $TARBALL ($(wc -c < "$TARBALL") bytes, $(find "$PAYLOAD" -type f | wc -l) files)"

if [ "$DEPLOY" -eq 0 ]; then
    echo "Not deploying (pass --deploy to ship it)."
    exit 0
fi

SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=15"
SSH_OPTS="$SSH_OPTS -o ServerAliveCountMax=8 -o StrictHostKeyChecking=accept-new"
[ -n "$ADAPTER" ] && SSH_OPTS="$SSH_OPTS -B $ADAPTER"
KEY="${SSH_KEY:-$HOME/.ssh/zaurus_ed25519}"

echo "==> deploying to $TARGET"
want="$(wc -c < "$TARBALL")"
want_md5="$(md5sum < "$TARBALL" | cut -d' ' -f1)"

remote_md5() {
    ssh $SSH_OPTS -i "$KEY" "$TARGET" \
        "md5sum < $1 2>/dev/null || /usr/local/bin/md5sum < $1 2>/dev/null" \
        2>/dev/null | awk '{print $1; exit}'
}
have_remote_md5=1
probe="$(remote_md5 /dev/null)"
case "$probe" in
    d41d8cd98f00b204e9800998ecf8427e) ;;
    *) have_remote_md5=0
       echo "    no usable md5sum on device -- length check only" ;;
esac

attempt=1
while : ; do
    ssh $SSH_OPTS -i "$KEY" "$TARGET" "cat > /tmp/mb.tar" < "$TARBALL"
    got="$(ssh $SSH_OPTS -i "$KEY" "$TARGET" "wc -c < /tmp/mb.tar" | tr -d ' \r\n')"
    if [ "$want" = "$got" ]; then
        if [ "$have_remote_md5" -eq 0 ]; then
            echo "    transferred $got bytes"
            break
        fi
        got_md5="$(remote_md5 /tmp/mb.tar)"
        if [ "$want_md5" = "$got_md5" ]; then
            echo "    md5 verified ($want_md5)"
            break
        fi
        echo "    attempt $attempt: md5 mismatch (want $want_md5, got $got_md5)" >&2
    else
        echo "    attempt $attempt: short transfer (sent $want, device has $got)" >&2
    fi
    attempt=$((attempt + 1))
    if [ "$attempt" -gt 5 ]; then
        echo "FAILED: could not get an intact payload to the device in 5 tries" >&2
        exit 1
    fi
done
ssh $SSH_OPTS -i "$KEY" "$TARGET" '
rm -f /usr/bin/*.replaced /usr/local/bin/*.replaced 2>/dev/null

if [ -f /root/.matchbox/kbdconfig ]; then
    rm -f /root/.matchbox/kbdconfig
    echo "    removed stale /root/.matchbox/kbdconfig (it shadowed the shipped one)"
fi

n=0
while [ "$n" -lt 20 ]; do
    out="$(/usr/local/bin/untar /tmp/mb.tar / 2>&1)"
    if [ -z "${out##*extracted*}" ]; then
        echo "    $out"
        rm -f /tmp/mb.tar
        exit 0
    fi
    busy="$(echo "$out" | sed -n "s|^untar: could not create //\.\(/.*\): Text file busy\$|\1|p")"
    if [ -z "$busy" ]; then
        echo "$out" >&2
        exit 1
    fi
    mv -f "$busy" "$busy.replaced" || exit 1
    echo "    in use, moved aside: $busy"
    n=$((n + 1))
done
echo "FAILED: still blocked after $n retries" >&2
exit 1' || { echo "FAILED: unpack on device failed" >&2; exit 1; }
echo "==> deployed."
echo "    A session started before this deploy is still running the OLD"
echo "    binaries from their original inodes. Reboot, or restart it with:"
echo "        DISPLAY=:0 matchbox-session &"
