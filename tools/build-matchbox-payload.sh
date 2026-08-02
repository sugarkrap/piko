#!/bin/sh
set -eu

# Assembles the Matchbox desktop into a single deployable tar and (with
# --deploy) ships it to the device.
#
# One archive rather than ~100 individual transfers: the link to this
# device is genuinely flaky, and the device's busybox has no tar, so the
# archive is unpacked with our own userspace/src/untar.c (installed at
# /usr/local/bin/untar). That is what untar exists for.
#
# Prerequisites -- this script only *collects*, it does not build:
#   tools/build-thirdparty-deps.sh      zlib expat libpng freetype
#                                       fontconfig + the DejaVu faces
#   tools/build-x11-stack.sh            everything else: the X.Org and
#                                       Matchbox packages into the DESTDIRs
#                                       listed below, and (at its end) st
#                                       and FLTK -- libfltk*.so.1.3,
#                                       fltktest, matchbox-fbrun -- into the
#                                       staging tree. It also runs
#                                       setup-x11-src.sh and build-libiw.sh,
#                                       so those are not separate steps.
# See docs/HOWTO-MATCHBOX-DESKTOP.md for the per-component configure
# lines, which are NOT all obvious (matchbox-desktop in particular needs
# --sysconfdir=/etc and a forced -DUSE_XSETTINGS).
#
# Usage:
#   tools/build-matchbox-payload.sh [--skip-st] [--deploy [user@host]] [--adapter IFACE]
#
# --skip-st leaves st, its menu entry and its icon out of the payload
# rather than failing on the missing binary. Pair it with the same flag to
# tools/build-x11-stack.sh; that script's header explains when st is
# unbuildable and why that should not cost you the rest of the desktop.
#
# Without --deploy it just writes the tar and stops, so you can inspect it.
# tools/chunked-deploy.sh (section 9) also ships this same tar, chunked and
# lock-protected -- prefer that when doing a full kernel+userspace+X11
# redeploy via tools/build-and-deploy.sh; use --deploy here directly for a
# quick X11-only iteration.

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
STAGE="$REPO/userspace/stage-target"
HOST_TRIPLET="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST_TRIPLET/bin}"
SYSROOT="$REPO/toolchain/x-tools/$HOST_TRIPLET/$HOST_TRIPLET/sysroot"

PAYLOAD="${PAYLOAD_DIR:-/tmp/mb-payload}"
TARBALL="${PAYLOAD_TAR:-/tmp/matchbox-payload.tar}"

# Per-component DESTDIRs. Each component is built separately (they were
# built in parallel by separate agents) and installed to its own prefix so
# concurrent builds cannot race on a shared tree.
D_WM="${D_WM:-/tmp/mbwm-stage}"
D_DESKTOP="${D_DESKTOP:-/tmp/mb-stage-desktop}"
D_PANEL="${D_PANEL:-/tmp/mb-stage-panel}"
D_COMMON="${D_COMMON:-/tmp/mb-stage-common}"
# mb-applet-card is its own repo/submodule rather than part of
# matchbox-panel, so it gets its own DESTDIR too.
D_CARD="${D_CARD:-/tmp/mb-stage-card}"
# Same story for mb-volume: userspace/src/mb-volume, not part of
# matchbox-panel proper.
D_VOLUME="${D_VOLUME:-/tmp/mb-stage-volume}"
# And for mb-brightness (userspace/src/mb-brightness), the applet that
# docks no icon and exists only to draw the backlight OSD.
D_BRIGHT="${D_BRIGHT:-/tmp/mb-stage-brightness}"

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

# Runtime libraries -- the WHOLE X11 stack, not just Matchbox's own
# dependencies, so this payload can populate a freshly-flashed device
# rather than assuming an earlier hand-deploy left things lying around.
# Only libc.so.0 and the dynamic loader come from the rootfs itself.
#
# Bare names here; the version suffix is discovered below, so a rebuilt
# library cannot silently keep shipping the old one.
# libXpm is here for exactly one consumer, the toasters screensaver, which
# decodes its vendored XPM sprite sheets with XpmCreateImageFromData().
LIBS="libX11 libXext libXpm libxcb libXau libXdmcp libz libexpat libpng16 \
libfreetype libfontconfig libXrender libXft libmb libpixman-1 libXfont \
libfontenc libxkbfile libmd libfltk libfltk_images libfltk_forms"

# Binaries that do not come from a component DESTDIR: the X server and the
# XKB compiler live in the xserver/xkbcomp submodule build trees. xkbcomp
# is not optional -- the server shells out to it to compile a keymap, and
# without it X dies with "Failed to activate core devices".
XSERVER_BIN="${XSERVER_BIN:-$REPO/userspace/src/xserver/hw/kdrive/fbdev/Xfbdev}"
XKBCOMP_BIN="${XKBCOMP_BIN:-$REPO/userspace/src/xkbcomp/xkbcomp}"
XEV_BIN="${XEV_BIN:-$REPO/userspace/src/xev/xev}"
ST_BIN="${ST_BIN:-$REPO/userspace/src/st/st}"
# The flying-toasters idle screensaver brightd launches -- see brightd.c's
# "SCREENSAVER CONTENT" header comment. Built by tools/build-toasters.sh.
TOASTERS_BIN="${TOASTERS_BIN:-$REPO/userspace/src/toasters}"
# fltktest is the FLTK equivalent of sdltest: proof on real hardware that
# the shared libfltk we just shipped loads and can draw. tools/build-fltk.sh
# puts it in the staging tree's own bindir rather than a component DESTDIR.
FLTKTEST_BIN="${FLTKTEST_BIN:-$STAGE/usr/bin/fltktest}"
# matchbox-fbrun hands the whole machine to a framebuffer application and
# takes it back afterwards. It ships here rather than with the rootfs
# because it is an FLTK client: it needs the libfltk and libstdc++ that this
# payload carries, and would be a dangling binary without them.
FBRUN_BIN="${FBRUN_BIN:-$STAGE/usr/bin/matchbox-fbrun}"
# pikostore ("Software Center") is the first real GUI app in the ROM, as
# opposed to a smoke test. Same staging location as fltktest -- put there
# by tools/build-pikostore.sh, which must run after tools/build-fltk.sh.
PIKOSTORE_BIN="${PIKOSTORE_BIN:-$STAGE/usr/bin/pikostore}"
# mb-wallpaper-picker: the desktop's wallpaper setter. Same staging location
# as fltktest/pikostore -- put there by tools/build-fltk.sh, which also
# builds fltktest and matchbox-fbrun.
WALLPAPER_PICKER_BIN="${WALLPAPER_PICKER_BIN:-$STAGE/usr/bin/mb-wallpaper-picker}"

echo "==> assembling into $PAYLOAD"
rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD/lib" "$PAYLOAD/usr" "$PAYLOAD/etc"

for base in $LIBS; do
    # Ship EVERY version present, not just the newest. Two libraries can
    # share a base name but export different SONAMEs -- libmd is exactly
    # that here: libmd.so.0 is libbsd's (what Xfbdev links) and
    # libmd.so.1 is this project's xsha1-compat shim. Picking "the
    # highest version" silently shipped the wrong one and left Xfbdev
    # unable to start.
    found=0
    for real in $(ls "$STAGE/usr/lib/" 2>/dev/null | grep -E "^${base}\.so\.[0-9]+(\.[0-9]+)*$"); do
        cp "$STAGE/usr/lib/$real" "$PAYLOAD/lib/"
        # DT_NEEDED names the SONAME (libfoo.so.N), so that symlink must exist.
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

# libgcc_s and libstdc++ come from the toolchain, not the staging tree.
# libexpat needs libgcc_s and nothing else drags it in; libstdc++ arrived
# with FLTK, the only C++ component in this stack -- every libfltk*.so and
# fltktest itself has it in DT_NEEDED. -L dereferences the SONAME symlink so
# one real file lands under the name the loader actually asks for.
cp "$SYSROOT/lib/libgcc_s.so.1" "$PAYLOAD/lib/"
cp -L "$SYSROOT/lib/libstdc++.so.6" "$PAYLOAD/lib/libstdc++.so.6"

for d in "$D_WM" "$D_DESKTOP" "$D_PANEL" "$D_COMMON" "$D_CARD" "$D_VOLUME" \
         "$D_BRIGHT"; do
    if [ ! -d "$d" ]; then
        echo "FAILED: missing component DESTDIR: $d" >&2
        echo "Build that component first (see docs/HOWTO-MATCHBOX-DESKTOP.md)." >&2
        exit 1
    fi
    [ -d "$d/usr" ] && cp -a "$d/usr/." "$PAYLOAD/usr/"
    [ -d "$d/etc" ] && cp -a "$d/etc/." "$PAYLOAD/etc/"
    echo "    merged: $d"
done

# X server + XKB compiler + xev (handy for diagnosing input on-device).
# Xfbdev goes to /usr/local/bin to match where it has always lived here;
# xkbcomp must be on the default PATH because the server execs it by name,
# and PATH on this device is only /usr/sbin:/usr/bin:/sbin:/bin.
mkdir -p "$PAYLOAD/usr/local/bin" "$PAYLOAD/usr/bin" "$PAYLOAD/usr/sbin"
# matchbox-fbrun goes to /usr/sbin, not /usr/local/bin: matchbox-desktop
# execs it by name for any .desktop marked X-Piko-Heavy, and /usr/local/bin
# is not on this device's PATH.
BINS="$XSERVER_BIN:usr/local/bin/Xfbdev \
$XKBCOMP_BIN:usr/bin/xkbcomp \
$XEV_BIN:usr/local/bin/xev \
$TOASTERS_BIN:usr/local/bin/toasters \
$FLTKTEST_BIN:usr/local/bin/fltktest \
$PIKOSTORE_BIN:usr/local/bin/pikostore \
$WALLPAPER_PICKER_BIN:usr/local/bin/mb-wallpaper-picker \
$FBRUN_BIN:usr/sbin/matchbox-fbrun"
if [ "$SKIP_ST" -eq 0 ]; then
    BINS="$BINS $ST_BIN:usr/local/bin/st"
else
    echo "    --skip-st: leaving st out of the payload"
fi
for spec in $BINS; do
    src="${spec%:*}"; dst="${spec##*:}"
    if [ ! -f "$src" ]; then
        echo "FAILED: missing $src -- build that component first" >&2
        echo "tools/build-x11-stack.sh builds every one of these; if it ran" >&2
        echo "and this is still missing, that is the bug, not your setup." >&2
        exit 1
    fi
    cp "$src" "$PAYLOAD/$dst"
    echo "    bin: $dst"
done

# XKB database + our Zaurus layout. Without the database the server cannot
# compile any keymap at all; without the layout the Fn symbol row (/ : [ ]
# | ...) is untypable. zaurus.xkb is the wrapper /etc/init.d/xsession
# feeds to xkbcomp at session start.
mkdir -p "$PAYLOAD/usr/share/X11" "$PAYLOAD/etc/X11"
cp -a "$STAGE/usr/share/X11/xkb" "$PAYLOAD/usr/share/X11/"
cp "$REPO/userspace/xkb/symbols/zaurus" "$PAYLOAD/usr/share/X11/xkb/symbols/zaurus"
cp "$REPO/userspace/xkb/zaurus.xkb"     "$PAYLOAD/etc/X11/zaurus.xkb"

# Fonts + fontconfig config. The device ships with NO fonts and no
# /etc/fonts at all, and Matchbox themes ask for "Sans bold 16px" -- a
# generic fontconfig family -- so without these every themed widget
# renders blank.
mkdir -p "$PAYLOAD/usr/share/fonts/truetype/dejavu"
cp "$STAGE"/usr/share/fonts/truetype/dejavu/*.ttf \
   "$PAYLOAD/usr/share/fonts/truetype/dejavu/"
cp -a "$STAGE/etc/fonts" "$PAYLOAD/etc/"

# Wallpaper. /usr/share/backgrounds is one of the two directories
# mb-wallpaper-picker scans (the other is $HOME/.matchbox/backgrounds), so
# an image here is what makes the picker show anything at all -- until now
# it opened on an empty grid, because the whole wallpaper system shipped
# without a single image to point it at.
#
# PNG, not JPEG, and that is not a preference: this build's libmb.so.1 is
# linked against libpng16 only -- no libjpeg -- so a .jpg here would be
# listed by the picker and then fail to decode. See "Wallpaper: modes,
# formats, and why it's cached raw" in docs/HOWTO-MATCHBOX-DESKTOP.md.
#
# FITTED, not cropped, and pre-rendered to exactly 640x480 -- the panel's
# size -- with black bars where the aspect ratios disagree. Fitted rather
# than filled so the whole picture is visible; with a landscape source on
# this landscape panel the bars are 7px a side and it covers 97.7% of the
# screen anyway.
#
# 640x480 LANDSCAPE. Getting this wrong is not subtle but it IS ambiguous
# from the outside: /sys/class/graphics/fb0/modes says U:640x480p and the
# stride is 1280 bytes (640px x 2), while the archived hardware notes call
# the panel "physically portrait: 480x640 fb" -- both true, describing
# different things. A 480x640 build of this file put 480x368 of picture in
# the middle of the screen, 57.5% of it, and read on the device as "really
# small". The composited cache is 480*640*2 == 640*480*2, so its size
# cannot tell the two apart either. Trust fb0/modes.
#
# Because the file is already exactly screen-sized, the spec that goes with
# it is img-centered (see tools/chunked-deploy.sh), which scales nothing at
# all: mbdesktop_view_init_bg() caches the composited result either way,
# but this way even the first boot after a flash does no scaling on a
# 400MHz part -- it is a straight blit.
#
# Regenerate with:
#   magick <src> -resize 640x480 -background black -gravity center \
#          -extent 640x480 -alpha off -strip -interlace none \
#          -define png:exclude-chunk=all -define png:compression-level=9 \
#          PNG24:piko-default.png
mkdir -p "$PAYLOAD/usr/share/backgrounds"
cp "$REPO/userspace/backgrounds/piko-default.png" \
   "$PAYLOAD/usr/share/backgrounds/piko-default.png"

# The session file decides which panel applets actually run. Without it
# matchbox-session falls through to its built-in default, which starts
# matchbox-panel with no arguments -- and the panel's compiled-in default
# is only menu-launcher + clock. See the file's own comments.
mkdir -p "$PAYLOAD/etc/matchbox"
cp "$REPO/modules/x11/matchbox-session" "$PAYLOAD/etc/matchbox/session"
chmod 755 "$PAYLOAD/etc/matchbox/session"

# Every applet the session asks for must be in the payload, or the panel
# just logs a session timeout per missing one and carries on looking
# half-broken. Cheaper to catch it here than on the device.
# Evaluate just the APPLETS= lines rather than pattern-matching them, so
# reformatting the list in that file cannot silently defeat this check.
applets="$(sh -c "$(grep '^APPLETS=' "$REPO/modules/x11/matchbox-session")
                  echo \"\$APPLETS\"")"
# One entry per line, then keep only the command word: an entry may carry
# arguments (e.g. "mb-applet-clock -s 16"), and splitting the whole list on
# whitespace would have us checking the payload for a binary called "-s".
applets="$(printf '%s\n' "$applets" | tr ',' '\n' | while read -r entry; do
    set -- $entry
    [ -n "${1:-}" ] && echo "$1"
done)"
[ -n "$applets" ] || { echo "FAILED: parsed no applets from modules/x11/matchbox-session" >&2; exit 1; }
for a in $applets; do
    if [ ! -f "$PAYLOAD/usr/bin/$a" ]; then
        echo "FAILED: /etc/matchbox/session runs $a but it is not in the payload." >&2
        echo "Rebuild matchbox-panel (mb-applet-battery needs --enable-proc-apm;" >&2
        echo "see docs/HOWTO-MATCHBOX-DESKTOP.md) or drop it from" >&2
        echo "modules/x11/matchbox-session." >&2
        exit 1
    fi
    echo "    applet: $a"
done

# Menu launchers + icons. Shipping an entry here is what puts an app on the
# desktop at all -- matchbox-desktop only reads the /usr/share/applications
# this payload deploys. That is independent of where the BINARY comes from:
# pikalibrate's ships via tools/chunked-deploy.sh's SDL section (it links
# libSDL, not this X11 stack), while st, xev, toasters, pikostore and
# mb-wallpaper-picker ship from this payload. Only the launcher and icon
# belong here either way.
#
# The Categories= line in each file picks which app-folder it lands in:
# Development matches the vfolder displayed as "Programming", System
# matches "System Tools" (see matchbox-common's data/vfolders-desktop).
# Categories=Action is the one that is not a folder at all -- both the
# menu-launcher and the desktop special-case it; see suspend.desktop.
#
# pikostore needs a launcher more than most: it is the GUI for updating the
# ROM, and expecting the user to open a terminal and type its name to reach
# it would defeat the point (this keyboard cannot even produce a slash).
#
# mb-wallpaper-picker's Categories=Settings lands it in the Settings folder
# instead of a launcher-worthy top-level folder -- same as the libmb/Xlib
# version it replaces.
#
# st and xev are the conditional pair. Under --skip-st there is no st
# binary in the payload, and xev.desktop execs "st -e xev" (xev writes to
# stdout and is useless without a terminal), so both entries would be menu
# items that do nothing when tapped -- worse than no entry. The xev BINARY
# still ships either way: it is perfectly usable from a shell over SSH.
#
# suspend, reboot and gototty are the three system ACTIONS rather than
# apps: tapping one does the thing instead of opening a window, and
# Categories=Action puts all three at the ROOT of the menu rather than
# inside a folder. They are
# unconditional -- what each one Exec=s is a plain shell script
# (/usr/sbin/suspend, /usr/sbin/softreboot, /usr/sbin/gototty), none of
# which depend on st or on anything else that --skip-st can take away.
# Those scripts ship via tools/chunked-deploy.sh, not from here; as with
# pikalibrate, only the launcher and icon belong in this payload.
mkdir -p "$PAYLOAD/usr/share/applications" "$PAYLOAD/usr/share/pixmaps"
LAUNCHERS="pikalibrate pikostore mb-wallpaper-picker toasters suspend reboot gototty"
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
# .la files are dead weight on flash AND leak absolute host build paths
# into the image; dlopen() loads the .so directly and never reads them.
find "$PAYLOAD" -name "*.la" -delete
# Headers and .pc files are for cross-building against this stack, which
# happens on the host, never on the device.
rm -rf "$PAYLOAD/usr/include" "$PAYLOAD/usr/lib/pkgconfig"

echo "==> stripping"
find "$PAYLOAD" -type f | while read -r f; do
    case "$(file -b "$f")" in
        ELF*) chmod u+w "$f"; "$STRIP" --strip-unneeded "$f" 2>/dev/null || true ;;
    esac
done

echo "==> verifying"
# Catch a host binary that wandered in via a mis-set CC.
if find "$PAYLOAD" -type f -exec file {} \; | grep "ELF" | grep -qv "ARM"; then
    echo "FAILED: non-ARM ELF in payload:" >&2
    find "$PAYLOAD" -type f -exec file {} \; | grep "ELF" | grep -v "ARM" >&2
    exit 1
fi
# Every DT_NEEDED must be satisfied by the payload or already present on
# the device. Getting this wrong means a binary that dies at exec time
# with a bare "not found", which is painful to diagnose over this link.
# Only libc comes from the rootfs now; everything else we ship.
ON_DEVICE="libc.so.0"
NEEDED="$(find "$PAYLOAD" -type f | while read -r f; do
    case "$(file -b "$f")" in
        ELF*) "$TOOLCHAIN_BIN_DIR/$HOST_TRIPLET-readelf" -d "$f" 2>/dev/null \
                | grep -oE '\[lib[^]]+\]' | tr -d '[]' ;;
    esac
done | sort -u)"
missing=0
for n in $NEEDED; do
    [ -e "$PAYLOAD/lib/$n" ] && continue
    case " $ON_DEVICE " in *" $n "*) continue ;; esac
    echo "FAILED: nothing provides $n" >&2
    missing=1
done
[ "$missing" -eq 0 ] || exit 1
echo "    all DT_NEEDED satisfied; all ELF are ARM"

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
# Verify the CONTENT, not just the length. This link corrupts payloads that
# arrive at exactly the right size -- observed 2026-07-31, where a
# byte-complete transfer made untar die on "bad header checksum". Retry the
# whole transfer on mismatch. md5sum on the device is our own
# userspace/src/md5sum; fall back to a length check if it is not installed
# yet, which is better than refusing to deploy at all.
want="$(wc -c < "$TARBALL")"
want_md5="$(md5sum < "$TARBALL" | cut -d' ' -f1)"

# Probe by USING md5sum, not by asking whether it exists: this device's ash
# has no `command` builtin (nor kill/killall/nohup), so `command -v` just
# errors. Anything that is not 32 hex digits means no usable md5sum.
remote_md5() {
    ssh $SSH_OPTS -i "$KEY" "$TARGET" \
        "md5sum < $1 2>/dev/null || /usr/local/bin/md5sum < $1 2>/dev/null" \
        2>/dev/null | awk '{print $1; exit}'
}
have_remote_md5=1
probe="$(remote_md5 /dev/null)"
case "$probe" in
    d41d8cd98f00b204e9800998ecf8427e) ;;   # md5 of empty input
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
# Unpacking over a *running* binary fails with ETXTBSY ("Text file busy"),
# and untar stops at the first one -- so a live Matchbox session used to
# abort the deploy partway through, leaving a half-updated tree.
#
# There is no way to stop the session first: this device's busybox has no
# kill, killall, pkill or nohup applet, and nothing else can signal a pid.
# So use the property that ETXTBSY blocks *writing* to a busy executable
# but not *renaming* it -- the running process keeps its inode, and untar
# is free to create a fresh file at the original path. Retry per offending
# file rather than pre-emptively moving things aside, so we only ever touch
# a path that is both in the payload and genuinely blocking.
#
# The .replaced files cannot be deleted while their process lives; they are
# swept at the start of the next deploy.
# Note for anyone editing the remote script below: it is inside a
# single-quoted argument, so it must not contain a single quote anywhere --
# not even in a comment or an apostrophe.
ssh $SSH_OPTS -i "$KEY" "$TARGET" '
rm -f /usr/bin/*.replaced /usr/local/bin/*.replaced 2>/dev/null

# Stale per-user keybindings shadow the ones we ship. The window manager
# routine keys_load_and_grab() (src/keys.c) tries $HOME/.matchbox/kbdconfig
# FIRST and only falls back to the compiled-in CONFDIR, which for the WM is
# /usr/etc/matchbox -- it is configured with --prefix=/usr and no
# --sysconfdir, unlike matchbox-desktop-classic which does pass
# --sysconfdir=/etc. The session runs as root with HOME=/root, so a
# /root/.matchbox/kbdconfig left over from an earlier image silently wins
# over whatever this payload installs, with no warning anywhere. That cost
# real debugging time on 2026-08-01: a corrected kbdconfig was deployed and
# the old binding was still in force, because the file being read was never
# the file being shipped.
#
# Removing it is the same call as --no-session for the panel mbdock.session
# (see modules/x11/matchbox-session): on an appliance the config is defined
# in tracked source and has to be identical on every boot, so per-user state
# that can outlive and override it gets deleted rather than merged. If you
# ever do want a hand-tuned local kbdconfig on a device, drop these lines --
# they will otherwise remove it on every deploy.
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
