#!/bin/sh
set -eu

# Builds every cross-compiled userspace component this project ships to the
# device, from scratch, in dependency order. One entry point instead of
# remembering which of tools/build-*.sh to run and in what order.
#
# Each step delegates to the existing per-component script and every one of
# those is idempotent -- they skip work that is already current -- so calling
# this unconditionally (e.g. from tools/build-and-deploy.sh) is cheap once
# things are built. Pass --force to rebuild everything from scratch.
#
# WHAT IT BUILDS (in this order -- the order matters):
#
#   1. userspace/src/md5sum         a tiny static ARM md5sum. Deployed FIRST
#                                   by chunked-deploy.sh so every later
#                                   transfer can be content-verified rather
#                                   than only byte-counted. Built here (not
#                                   just inline in build-and-deploy.sh) so a
#                                   plain `tools/build-userspace.sh` produces
#                                   a complete, deployable set.
#  1b. userspace/src/brightd        the backlight policy daemon (Fn+3/Fn+4,
#                                   idle dim, lid blank). Static, libc only:
#                                   it reads evdev and sysfs directly and
#                                   deliberately links nothing X. Ordered
#                                   next to md5sum because it has no
#                                   dependencies on anything below.
#   2. tools/build-alsa.sh          alsa-lib + alsa-utils. MUST run before
#                                   MPlayer: MPlayer links libasound.a out of
#                                   userspace/stage-alsa, so building it
#                                   first is a hard ordering dependency, not
#                                   a preference.
#   3. tools/build-mplayer.sh       MPlayer (video/audio playback).
#   4. tools/build-sdl.sh           SDL 1.2 (libSDL-1.2.so.0, shared -- see
#                                   its header for why this one component is
#                                   dynamically linked against this project's
#                                   otherwise-static convention) plus the
#                                   sdltest dummy smoke-test app.
#   5. tools/build-st.sh            st (suckless terminal). Unlike the other
#                                   four, it is NOT self-contained: it links
#                                   dynamically against libX11/libXft/
#                                   fontconfig/freetype from
#                                   userspace/stage-target, i.e. it needs the
#                                   X11/matchbox stack (see below) already
#                                   staged. Skipped, not fatal, when that
#                                   stage doesn't exist, so a clean checkout
#                                   that hasn't done the X11 bring-up yet
#                                   still gets a complete ALSA/MPlayer/SDL
#                                   build out of this script.
#   6. tools/build-fltk.sh          FLTK 1.3 (libfltk.so.1.3 + _images +
#                                   _forms, shared) and the fltktest smoke
#                                   test. Like st, it needs the X11 stack
#                                   staged first and is skipped -- not
#                                   fatal -- when it isn't. Unlike SDL it
#                                   installs INTO userspace/stage-target
#                                   rather than a stage of its own, because
#                                   it is part of that X11 sysroot: the
#                                   X11/Matchbox payload ships it, and
#                                   anything cross-linking against FLTK
#                                   later needs it on the same include/lib
#                                   path as libX11.
#
# NOT BUILT HERE: the X11/matchbox stack itself (userspace/src/libX11,
# xserver, matchbox-window-manager, pixman, ...). Those are git submodules
# that were cross-built and staged into userspace/stage-target by hand;
# there is no scripted build for them yet, and inventing one blindly here
# would be worse than saying so. tools/deploy-x11.sh deploys whatever is
# already staged. If you add a build-x11.sh, wire it in here.
#
# Everything produced is a build artifact and is gitignored: the staging
# trees (userspace/stage-alsa, stage-alsa-runtime, stage-mplayer,
# stage-sdl, stage-sdl-runtime), the vendored upstream source trees under
# userspace/src/, userspace/src/md5sum, userspace/src/st/st, and everything
# tools/build-fltk.sh installs into userspace/stage-target.
#
# Usage:
#   tools/build-userspace.sh [--force] [--skip-alsa] [--skip-mplayer] [--skip-sdl] [--skip-st] [--skip-fltk]
#
# --force        rebuild every component from scratch (re-extract sources,
#                reconfigure). Slow: MPlayer alone is a ~15 MiB static binary
#                with a bundled FFmpeg and takes a while on any machine.
# --skip-alsa    don't build alsa-lib/alsa-utils (implies MPlayer must
#                already have a usable userspace/stage-alsa to link against).
# --skip-mplayer don't build MPlayer -- much the slowest step, so this is
#                the useful one when you only touched the audio stack.
# --skip-sdl     don't build SDL 1.2 / sdltest.
# --skip-st      don't build st.
# --skip-fltk    don't build FLTK / fltktest.
#
# Env overrides are passed straight through to the per-component scripts;
# see those for the full list. The common ones:
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE       default arm-unknown-linux-uclibcgnueabi-
#   JOBS                default: nproc
#
# Exit codes:
#   0   every requested component built
#   1   a hard failure in one of them (the failing script's own output says
#       which; this wrapper does not swallow it)

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

FORCE=0
SKIP_ALSA=0
SKIP_MPLAYER=0
SKIP_SDL=0
SKIP_ST=0
SKIP_FLTK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --force)        FORCE=1;        shift ;;
        --skip-alsa)    SKIP_ALSA=1;    shift ;;
        --skip-mplayer) SKIP_MPLAYER=1; shift ;;
        --skip-sdl)     SKIP_SDL=1;     shift ;;
        --skip-st)      SKIP_ST=1;      shift ;;
        --skip-fltk)    SKIP_FLTK=1;    shift ;;
        -h|--help)
            sed -n '3,93p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "tools/build-userspace.sh: unknown argument: $1" >&2
            echo "Usage: tools/build-userspace.sh [--force] [--skip-alsa] [--skip-mplayer] [--skip-sdl] [--skip-st] [--skip-fltk]" >&2
            exit 1
            ;;
    esac
done

FORCE_ARG=""
[ "$FORCE" -eq 1 ] && FORCE_ARG="--force"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"
if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
    export PATH
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/build-userspace.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE." >&2
    exit 1
fi
export TOOLCHAIN_BIN_DIR CROSS_COMPILE

echo "==> userspace build using $CROSS_COMPILE (from $TOOLCHAIN_BIN_DIR)"

# --- 1. md5sum (deploy-time content verification) ---------------------------
# -static because this rootfs ships NO dynamic linker at all: there is no
# /lib/ld-uClibc.so.0 and no /usr/lib, so a dynamically linked binary dies
# with a bare "not found" that reads like a missing file rather than a
# missing loader. Same reason alsa-utils and MPlayer are static.
MD5SUM_SRC="$REPO/userspace/src/md5sum.c"
MD5SUM_BIN="$REPO/userspace/src/md5sum"
if [ -f "$MD5SUM_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$MD5SUM_BIN" ] || [ "$MD5SUM_SRC" -nt "$MD5SUM_BIN" ]; then
        echo "==> building userspace/src/md5sum"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$MD5SUM_BIN" "$MD5SUM_SRC"
        "${CROSS_COMPILE}strip" "$MD5SUM_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/md5sum already up to date"
    fi
else
    echo "==> skipping md5sum (no $MD5SUM_SRC)"
fi

# --- 1b. brightd (backlight policy daemon) ----------------------------------
# -static for the same reason as md5sum above: no dynamic linker on the
# rootfs. Links nothing but libc -- it reads evdev and sysfs directly and
# deliberately avoids X (see the header comment in brightd.c).
BRIGHTD_SRC="$REPO/userspace/src/brightd.c"
BRIGHTD_BIN="$REPO/userspace/src/brightd"
if [ -f "$BRIGHTD_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$BRIGHTD_BIN" ] || [ "$BRIGHTD_SRC" -nt "$BRIGHTD_BIN" ]; then
        echo "==> building userspace/src/brightd"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$BRIGHTD_BIN" "$BRIGHTD_SRC"
        "${CROSS_COMPILE}strip" "$BRIGHTD_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/brightd already up to date"
    fi
else
    echo "==> skipping brightd (no $BRIGHTD_SRC)"
fi

# --- 2. ALSA (must precede MPlayer -- MPlayer links libasound.a from it) ----
if [ "$SKIP_ALSA" -eq 0 ]; then
    echo "==> building ALSA userspace (alsa-lib + alsa-utils)"
    sh "$REPO/tools/build-alsa.sh" $FORCE_ARG
else
    echo "==> --skip-alsa: not building alsa-lib/alsa-utils"
fi

# --- 3. MPlayer -------------------------------------------------------------
if [ "$SKIP_MPLAYER" -eq 0 ]; then
    echo "==> building MPlayer"
    sh "$REPO/tools/build-mplayer.sh" $FORCE_ARG
else
    echo "==> --skip-mplayer: not building MPlayer"
fi

# --- 4. SDL 1.2 (independent of ALSA/MPlayer -- video only, audio disabled) -
if [ "$SKIP_SDL" -eq 0 ]; then
    echo "==> building SDL 1.2"
    sh "$REPO/tools/build-sdl.sh" $FORCE_ARG
else
    echo "==> --skip-sdl: not building SDL"
fi

# --- 5. st (needs the X11 stack already staged -- see header) --------------
if [ "$SKIP_ST" -eq 0 ]; then
    if [ -f "$REPO/userspace/stage-target/usr/include/X11/Xlib.h" ]; then
        echo "==> building st"
        sh "$REPO/tools/build-st.sh" $FORCE_ARG
    else
        echo "==> skipping st (userspace/stage-target has no X11 stack staged yet)"
    fi
else
    echo "==> --skip-st: not building st"
fi

# --- 6. FLTK (needs the X11 stack already staged -- see header) ------------
if [ "$SKIP_FLTK" -eq 0 ]; then
    if [ -f "$REPO/userspace/stage-target/usr/lib/pkgconfig/xft.pc" ]; then
        echo "==> building FLTK"
        sh "$REPO/tools/build-fltk.sh" $FORCE_ARG
    else
        echo "==> skipping FLTK (userspace/stage-target has no X11/Xft stack staged yet)"
    fi
else
    echo "==> --skip-fltk: not building FLTK"
fi

echo ""
echo "==> userspace build complete"
# Explicit ifs rather than `[ ... ] && echo`: a false test on the last such
# line would make the script exit non-zero under `set -e`, turning a
# perfectly successful build into a reported failure.
if [ -f "$MD5SUM_BIN" ]; then
    echo "    md5sum:  $MD5SUM_BIN"
fi
if [ -f "$BRIGHTD_BIN" ]; then
    echo "    brightd: $BRIGHTD_BIN"
fi
if [ -d "$REPO/userspace/stage-alsa-runtime" ]; then
    echo "    alsa:    $REPO/userspace/stage-alsa-runtime ($(du -sh "$REPO/userspace/stage-alsa-runtime" 2>/dev/null | cut -f1))"
fi
if [ -f "$REPO/userspace/stage-mplayer/usr/bin/mplayer" ]; then
    echo "    mplayer: $REPO/userspace/stage-mplayer/usr/bin/mplayer ($(du -h "$REPO/userspace/stage-mplayer/usr/bin/mplayer" 2>/dev/null | cut -f1))"
fi
if [ -d "$REPO/userspace/stage-sdl-runtime" ]; then
    echo "    sdl:     $REPO/userspace/stage-sdl-runtime ($(du -sh "$REPO/userspace/stage-sdl-runtime" 2>/dev/null | cut -f1))"
fi
if [ -f "$REPO/userspace/src/st/st" ]; then
    echo "    st:      $REPO/userspace/src/st/st ($(du -h "$REPO/userspace/src/st/st" 2>/dev/null | cut -f1))"
fi
if [ -f "$REPO/userspace/stage-target/usr/lib/libfltk.so.1.3" ]; then
    echo "    fltk:    $REPO/userspace/stage-target/usr/lib/libfltk.so.1.3 ($(du -h "$REPO/userspace/stage-target/usr/lib/libfltk.so.1.3" 2>/dev/null | cut -f1))"
fi
echo ""
echo "    Deploy with tools/build-and-deploy.sh (or tools/chunked-deploy.sh)."
echo "    NOTE: the X11/matchbox stack is NOT built here -- see the header."
