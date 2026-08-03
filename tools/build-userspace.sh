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
# 1b2. userspace/src/flipd          the screen-rotation daemon: watches the
#                                   swivel hinge's tablet-mode switch and
#                                   turns the display 180 degrees using the
#                                   w100 CRTC's own scanout rotation. Static,
#                                   libc only, same reasoning as brightd.
#  1c. userspace/src/kill           the only way to signal a process on this
#                                   device (this busybox has no kill/killall/
#                                   pkill applet at all). Same reasoning as
#                                   brightd: no dependencies, so it goes early.
#   2. tools/build-ssh.sh           scp + OpenSSH's sftp-server + a
#                                   reproducible dropbear. The device's only
#                                   remote path is WiFi->SSH (AGENTS.md), and
#                                   until this existed nothing in the repo
#                                   could rebuild the SSH server at all.
#   3. tools/build-alsa.sh          alsa-lib + alsa-utils. MUST run before
#                                   MPlayer: MPlayer links libasound.a out of
#                                   userspace/stage-alsa, so building it
#                                   first is a hard ordering dependency, not
#                                   a preference.
#   4. tools/build-mplayer.sh       MPlayer (video/audio playback).
#   5. tools/build-sdl.sh           SDL 1.2 (libSDL-1.2.so.0, shared -- see
#                                   its header for why this one component is
#                                   dynamically linked against this project's
#                                   otherwise-static convention) plus the
#                                   sdltest dummy smoke-test app.
#   6. tools/build-st.sh            st (suckless terminal). Unlike the other
#                                   five, it is NOT self-contained: it links
#                                   dynamically against libX11/libXft/
#                                   fontconfig/freetype from
#                                   userspace/stage-target, i.e. it needs the
#                                   X11/matchbox stack (see below) already
#                                   staged. Skipped, not fatal, when that
#                                   stage doesn't exist, so a clean checkout
#                                   that hasn't done the X11 bring-up yet
#                                   still gets a complete ALSA/MPlayer/SDL
#                                   build out of this script.
#   7. tools/build-fltk.sh          FLTK 1.3 (libfltk.so.1.3 + _images +
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
#   8. tools/build-toasters.sh      toasters (the flying-toasters idle
#                                   screensaver, launched by brightd -- see
#                                   its "SCREENSAVER CONTENT" header
#                                   comment). Same "needs the X11 stack
#                                   staged" situation as st, and skipped the
#                                   same way when it isn't -- it is a single
#                                   file with no Makefile of its own, so
#                                   unlike st this is one gcc invocation
#                                   rather than a `make`.
#
# NOT BUILT HERE: the X11/matchbox stack (userspace/src/libX11, xserver,
# matchbox-window-manager, pixman, ...). tools/build-x11-stack.sh now
# cross-builds it from the tracked submodules, and
# tools/build-matchbox-payload.sh collects the result into a deployable
# tar (see tools/build-and-deploy.sh's --skip-x11 for how this hooks into
# the routine deploy path). tools/deploy-x11.sh predated both of those and
# shipped a smaller, differently-pathed subset that would now actively
# conflict with this pipeline -- retired.
#
# Everything produced is a build artifact and is gitignored: the staging
# trees (userspace/stage-alsa, stage-alsa-runtime, stage-mplayer,
# stage-sdl, stage-sdl-runtime, stage-ssh), the vendored upstream source
# trees under userspace/src/, userspace/src/md5sum, userspace/src/st/st,
# userspace/src/toasters, and everything tools/build-fltk.sh installs into
# userspace/stage-target.
#
# Usage:
#   tools/build-userspace.sh [--force] [--skip-ssh] [--skip-alsa] [--skip-mplayer] [--skip-sdl] [--skip-st] [--skip-fltk] [--skip-toasters]
#
# --force        rebuild every component from scratch (re-extract sources,
#                reconfigure). Slow: MPlayer alone is a ~15 MiB static binary
#                with a bundled FFmpeg and takes a while on any machine.
# --skip-ssh     don't build scp/sftp-server/dropbear. Only reasonable when
#                userspace/stage-ssh is already current -- see above for why
#                this is the last thing worth skipping.
# --skip-alsa    don't build alsa-lib/alsa-utils (implies MPlayer must
#                already have a usable userspace/stage-alsa to link against).
# --skip-mplayer don't build MPlayer -- much the slowest step, so this is
#                the useful one when you only touched the audio stack.
# --skip-sdl     don't build SDL 1.2 / sdltest.
# --skip-st      don't build st.
# --skip-fltk    don't build FLTK / fltktest.
# --skip-toasters don't build the toasters screensaver.
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
SKIP_SSH=0
SKIP_ALSA=0
SKIP_MPLAYER=0
SKIP_SDL=0
SKIP_ST=0
SKIP_FLTK=0
SKIP_TOASTERS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --force)          FORCE=1;          shift ;;
        --skip-ssh)       SKIP_SSH=1;       shift ;;
        --skip-alsa)      SKIP_ALSA=1;      shift ;;
        --skip-mplayer)   SKIP_MPLAYER=1;   shift ;;
        --skip-sdl)       SKIP_SDL=1;       shift ;;
        --skip-st)        SKIP_ST=1;        shift ;;
        --skip-fltk)      SKIP_FLTK=1;      shift ;;
        --skip-toasters)  SKIP_TOASTERS=1;  shift ;;
        -h|--help)
            sed -n '3,101p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "tools/build-userspace.sh: unknown argument: $1" >&2
            echo "Usage: tools/build-userspace.sh [--force] [--skip-ssh] [--skip-alsa] [--skip-mplayer] [--skip-sdl] [--skip-st] [--skip-fltk] [--skip-toasters]" >&2
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

# --- 1b1. piko-splash (stage-2 half of the boot splash) ---------------------
# Static, libc only. Exists because this rootfs cannot draw the splash any
# other way: its busybox has no fbsplash applet and no gzip, and w100fb has
# no usable write() path, so `cat splash.raw > /dev/fb0` fails with EINVAL.
# mmap is the only route -- see the header in piko-splash.c.
PIKO_SPLASH_SRC="$REPO/userspace/src/piko-splash.c"
PIKO_SPLASH_BIN="$REPO/userspace/src/piko-splash"
if [ -f "$PIKO_SPLASH_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$PIKO_SPLASH_BIN" ] || [ "$PIKO_SPLASH_SRC" -nt "$PIKO_SPLASH_BIN" ]; then
        echo "==> building userspace/src/piko-splash"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$PIKO_SPLASH_BIN" "$PIKO_SPLASH_SRC"
        "${CROSS_COMPILE}strip" "$PIKO_SPLASH_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/piko-splash already up to date"
    fi
else
    echo "==> skipping piko-splash (no $PIKO_SPLASH_SRC)"
fi

# --- 1b2. flipd (screen rotation on the swivel hinge) -----------------------
# Same shape as brightd: static, libc only, reads evdev and sysfs directly.
# It turns the display 180 degrees via the w100 CRTC's scanout rotation
# when the lid is swivelled -- no pixels move. See flipd.c's header.
FLIPD_SRC="$REPO/userspace/src/flipd.c"
FLIPD_BIN="$REPO/userspace/src/flipd"
if [ -f "$FLIPD_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$FLIPD_BIN" ] || [ "$FLIPD_SRC" -nt "$FLIPD_BIN" ]; then
        echo "==> building userspace/src/flipd"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$FLIPD_BIN" "$FLIPD_SRC"
        "${CROSS_COMPILE}strip" "$FLIPD_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/flipd already up to date"
    fi
else
    echo "==> skipping flipd (no $FLIPD_SRC)"
fi

# --- 1c. kill (the only way to signal a process on this device) -------------
# This busybox has no kill, killall or pkill applet at all, so without this
# binary there is no way to send a signal to anything. tools/chunked-deploy.sh
# already RELIES on /usr/local/bin/kill existing (it stops the running X
# session with it before unpacking the payload), and /usr/sbin/deskscan uses
# it to ask matchbox-desktop to reload -- but until now nothing actually
# built it, so a device that had never been hand-fed a copy simply did not
# have one. Same -static reasoning as md5sum above.
KILL_SRC="$REPO/userspace/src/kill.c"
KILL_BIN="$REPO/userspace/src/kill"
if [ -f "$KILL_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$KILL_BIN" ] || [ "$KILL_SRC" -nt "$KILL_BIN" ]; then
        echo "==> building userspace/src/kill"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$KILL_BIN" "$KILL_SRC"
        "${CROSS_COMPILE}strip" "$KILL_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/kill already up to date"
    fi
else
    echo "==> skipping kill (no $KILL_SRC)"
fi

# --- 1c2. mhz (CPU speed / overclocking) ------------------------------------
# Single-word name for the same reason as `bright` and `netinfo`: it is typed
# on the device keyboard, which cannot produce '/' or ':' (AGENTS.md). Drives
# the cpufreq sysfs and (re)loads pxa2xx-cpufreq at the requested ceiling --
# see userspace/src/mhz.c and docs/HOWTO-OVERCLOCK.md. Same -static reasoning
# as md5sum above; links nothing but libc.
MHZ_SRC="$REPO/userspace/src/mhz.c"
MHZ_BIN="$REPO/userspace/src/mhz"
if [ -f "$MHZ_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$MHZ_BIN" ] || [ "$MHZ_SRC" -nt "$MHZ_BIN" ]; then
        echo "==> building userspace/src/mhz"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$MHZ_BIN" "$MHZ_SRC"
        "${CROSS_COMPILE}strip" "$MHZ_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/mhz already up to date"
    fi
else
    echo "==> skipping mhz (no $MHZ_SRC)"
fi

# --- 1c-ter. pkillx (signal a process BY NAME) ------------------------------
# The companion to kill above: kill needs a PID, and with no ps-parsing
# tools on this busybox that is the hard part. pkillx walks /proc itself
# and matches on the process basename, which is what makes it usable from
# a script that cannot know a PID in advance.
#
# It was in exactly the hole kill was: userspace/src/pkillx.c has been in
# the tree since e348909 and three separate places already tell you to run
# it -- rcS's comment ("Stop it with pkillx brightd"), docs/HOWTO-
# BRIGHTNESS.md, docs/HOWTO-FLTK.md -- but nothing ever built it, so it
# only existed on boards where it had been hand-fed a copy. It became load
# bearing with /usr/sbin/gototty, whose entire body is "pkillx Xfbdev":
# without this, the Go to TTY menu entry silently does nothing.
#
# Same -static reasoning as md5sum above.
PKILLX_SRC="$REPO/userspace/src/pkillx.c"
PKILLX_BIN="$REPO/userspace/src/pkillx"
if [ -f "$PKILLX_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$PKILLX_BIN" ] || [ "$PKILLX_SRC" -nt "$PKILLX_BIN" ]; then
        echo "==> building userspace/src/pkillx"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$PKILLX_BIN" "$PKILLX_SRC"
        "${CROSS_COMPILE}strip" "$PKILLX_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/pkillx already up to date"
    fi
else
    echo "==> skipping pkillx (no $PKILLX_SRC)"
fi

# --- 1c-quater. cardswap (the SD card's swapfile) ---------------------------
# Same hole as kill and pkillx, one layer down: this busybox is built
# without mkswap, swapon AND swapoff, so there is no shell path to a swap
# area at all on this device. cardswap creates, signs and enables the
# 256 MiB file at /mnt/card/.zaurus/swap with the syscalls directly, and is
# what /usr/sbin/sdcard (the mdev hook) and mb-applet-card's Eject both
# call. Without it the card mounts exactly as before and the machine
# simply has no swap -- everything degrades quietly, which is why the
# callers all guard on it being executable.
#
# Same -static reasoning as md5sum above.
CARDSWAP_SRC="$REPO/userspace/src/cardswap.c"
CARDSWAP_BIN="$REPO/userspace/src/cardswap"
if [ -f "$CARDSWAP_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$CARDSWAP_BIN" ] || [ "$CARDSWAP_SRC" -nt "$CARDSWAP_BIN" ]; then
        echo "==> building userspace/src/cardswap"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$CARDSWAP_BIN" "$CARDSWAP_SRC"
        "${CROSS_COMPILE}strip" "$CARDSWAP_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/cardswap already up to date"
    fi
else
    echo "==> skipping cardswap (no $CARDSWAP_SRC)"
fi

# --- 1c-quater-bis. zramswap (compressed RAM swap, ahead of the card's) -----
# The same missing-applet hole as cardswap just above, for a second swap
# device: /dev/zram0, backed by CONFIG_ZRAM in kernel.config-corgi-7.1.4
# rather than the SD card. zramswap creates/resizes it, signs it, and
# swapon(2)s it at a priority that always beats cardswap's, so pages go to
# RAM-compressed storage first and only spill to the (slower, removable)
# card once zram's fixed capacity is full. Started from rcS at boot,
# unconditionally -- unlike the card, it needs no card to be present.
#
# Same -static reasoning as md5sum above.
ZRAMSWAP_SRC="$REPO/userspace/src/zramswap.c"
ZRAMSWAP_BIN="$REPO/userspace/src/zramswap"
if [ -f "$ZRAMSWAP_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$ZRAMSWAP_BIN" ] || [ "$ZRAMSWAP_SRC" -nt "$ZRAMSWAP_BIN" ]; then
        echo "==> building userspace/src/zramswap"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$ZRAMSWAP_BIN" "$ZRAMSWAP_SRC"
        "${CROSS_COMPILE}strip" "$ZRAMSWAP_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/zramswap already up to date"
    fi
else
    echo "==> skipping zramswap (no $ZRAMSWAP_SRC)"
fi

# --- 1c-quinquies. vol (one-word volume control) ----------------------------
# The companion to /usr/sbin/bright, and the same shape: a short typable
# name for something the device keyboard cannot otherwise reach (AGENTS.md,
# "The device keyboard cannot type many characters").
#
# It does not touch the mixer itself -- it writes one byte to mb-volume's
# control FIFO and lets the applet, which owns ALSA and /etc/zaurus/volumed,
# do the work. Same one-owner rule bright/brightd follow.
#
# It is a C program rather than the obvious two-line shell script because
# opening a FIFO for writing BLOCKS until there is a reader, and this
# device's /tmp is jffs2 rather than tmpfs -- so a stale FIFO from a dead
# session survives a reboot and would hang the shell with no ^C on the
# framebuffer console and no kill applet to escape with. O_NONBLOCK turns
# that into an error message, and there is no way to ask ash for it.
#
# Same -static reasoning as md5sum above.
VOL_SRC="$REPO/userspace/src/vol.c"
VOL_BIN="$REPO/userspace/src/vol"
if [ -f "$VOL_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$VOL_BIN" ] || [ "$VOL_SRC" -nt "$VOL_BIN" ]; then
        echo "==> building userspace/src/vol"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$VOL_BIN" "$VOL_SRC"
        "${CROSS_COMPILE}strip" "$VOL_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/vol already up to date"
    fi
else
    echo "==> skipping vol (no $VOL_SRC)"
fi

# --- 1c-bis. hwclock + ntpsync (the clock) ----------------------------------
# This busybox has no hwclock, no ntpd and no rdate applet, so with the
# kernel RTC driver alone there was still no way to persist a time change
# or to fetch an accurate one. Same -static reasoning as md5sum above.
#
# ntpsync execs /usr/sbin/hwclock to write the RTC, so the two ship together
# or neither is much use -- built in one loop for exactly that reason.
for _clock_tool in hwclock ntpsync; do
    CLOCK_SRC="$REPO/userspace/src/$_clock_tool.c"
    CLOCK_BIN="$REPO/userspace/src/$_clock_tool"
    if [ -f "$CLOCK_SRC" ]; then
        if [ "$FORCE" -eq 1 ] || [ ! -f "$CLOCK_BIN" ] || [ "$CLOCK_SRC" -nt "$CLOCK_BIN" ]; then
            echo "==> building userspace/src/$_clock_tool"
            "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
                -o "$CLOCK_BIN" "$CLOCK_SRC"
            "${CROSS_COMPILE}strip" "$CLOCK_BIN" 2>/dev/null || true
        else
            echo "==> userspace/src/$_clock_tool already up to date"
        fi
    else
        echo "==> skipping $_clock_tool (no $CLOCK_SRC)"
    fi
done
unset _clock_tool
# --- 1d. opkg (package manager) ---------------------------------------------
# Exactly the same hole tools/build-toasters.sh and userspace/src/kill were
# in before they were added here: tools/build-opkg.sh existed and worked,
# tools/chunked-deploy.sh section 6d already deployed its output -- but
# nothing ever CALLED it, so userspace/stage-target/usr/bin/opkg never
# appeared, the deploy's `if [ -x ... ]` gate never fired, and every run
# printed "no staged opkg -- skipping" as though that were a setting. The
# package manager has simply never been on the device.
#
# Not gated behind a --skip flag: the build is a single static ~520KB
# binary and the script skips itself once staged, so it costs nothing on
# any subsequent run. Not fatal either -- it needs libarchive from
# tools/build-thirdparty-deps.sh, and a machine without that staged should
# still get the rest of userspace built rather than stopping here.
if [ -x "$REPO/tools/build-opkg.sh" ]; then
    echo "==> building opkg (tools/build-opkg.sh)"
    if ! sh "$REPO/tools/build-opkg.sh" $FORCE_ARG; then
        echo "==> opkg build FAILED -- continuing without a package manager" >&2
        echo "    (it needs libarchive staged by tools/build-thirdparty-deps.sh;" >&2
        echo "     re-run tools/build-opkg.sh directly to see the full output)" >&2
    fi
fi

# --- 2. SSH file transfer (scp + sftp-server, and a reproducible dropbear) --
# Deliberately early and unconditional: this is the transport everything
# else in this list is delivered over (AGENTS.md -- no USB, no serial), so
# a build that skips it to save time is saving time on the wrong thing.
# It is also the only step here with no external dependency on another
# staging tree, so it can never be blocked by an earlier failure.
if [ "$SKIP_SSH" -eq 0 ]; then
    echo "==> building SSH file transfer (scp + sftp-server + dropbear)"
    sh "$REPO/tools/build-ssh.sh" $FORCE_ARG
else
    echo "==> --skip-ssh: not building scp/sftp-server/dropbear"
fi

# --- 3. ALSA (must precede MPlayer -- MPlayer links libasound.a from it) ----
if [ "$SKIP_ALSA" -eq 0 ]; then
    echo "==> building ALSA userspace (alsa-lib + alsa-utils)"
    sh "$REPO/tools/build-alsa.sh" $FORCE_ARG
else
    echo "==> --skip-alsa: not building alsa-lib/alsa-utils"
fi

# --- 4. MPlayer -------------------------------------------------------------
if [ "$SKIP_MPLAYER" -eq 0 ]; then
    echo "==> building MPlayer"
    sh "$REPO/tools/build-mplayer.sh" $FORCE_ARG
else
    echo "==> --skip-mplayer: not building MPlayer"
fi

# --- 5. SDL 1.2 (independent of ALSA/MPlayer -- video only, audio disabled) -
if [ "$SKIP_SDL" -eq 0 ]; then
    echo "==> building SDL 1.2"
    sh "$REPO/tools/build-sdl.sh" $FORCE_ARG
else
    echo "==> --skip-sdl: not building SDL"
fi

# --- 6. st (needs the X11 stack already staged -- see header) --------------
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

# --- 7. FLTK (needs the X11 stack already staged -- see header) ------------
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

# --- 8. toasters (needs the X11 stack already staged -- see header) --------
if [ "$SKIP_TOASTERS" -eq 0 ]; then
    if [ -f "$REPO/userspace/stage-target/usr/include/X11/Xlib.h" ]; then
        echo "==> building toasters"
        sh "$REPO/tools/build-toasters.sh" $FORCE_ARG
    else
        echo "==> skipping toasters (userspace/stage-target has no X11 stack staged yet)"
    fi
else
    echo "==> --skip-toasters: not building toasters"
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
if [ -f "$FLIPD_BIN" ]; then
    echo "    flipd:   $FLIPD_BIN"
fi
if [ -f "$KILL_BIN" ]; then
    echo "    kill:    $KILL_BIN"
fi
if [ -f "$CARDSWAP_BIN" ]; then
    echo "    cardswap: $CARDSWAP_BIN"
fi
if [ -f "$ZRAMSWAP_BIN" ]; then
    echo "    zramswap: $ZRAMSWAP_BIN"
fi
if [ -d "$REPO/userspace/stage-ssh" ]; then
    echo "    ssh:     $REPO/userspace/stage-ssh ($(du -sh "$REPO/userspace/stage-ssh" 2>/dev/null | cut -f1))"
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
if [ -f "$REPO/userspace/src/toasters" ]; then
    echo "    toasters: $REPO/userspace/src/toasters ($(du -h "$REPO/userspace/src/toasters" 2>/dev/null | cut -f1))"
fi
echo ""
echo "    Deploy with tools/build-and-deploy.sh (or tools/chunked-deploy.sh)."
echo "    NOTE: the X11/matchbox stack is NOT built here -- see the header."
