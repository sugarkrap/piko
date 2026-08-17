#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"

FORCE=0
SKIP_SSH=0
SKIP_ALSA=0
SKIP_MPLAYER=0
SKIP_SDL=0
SKIP_ST=0
SKIP_FLTK=0
SKIP_TOASTERS=0
SKIP_KEXEC=0
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
        --skip-kexec)     SKIP_KEXEC=1;     shift ;;
        -h|--help)
            sed -n '3,101p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "tools/userspace/build-userspace.sh: unknown argument: $1" >&2
            echo "Usage: tools/userspace/build-userspace.sh [--force] [--skip-ssh] [--skip-alsa] [--skip-mplayer] [--skip-sdl] [--skip-st] [--skip-fltk] [--skip-toasters] [--skip-kexec]" >&2
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
    echo "tools/userspace/build-userspace.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE." >&2
    exit 1
fi
export TOOLCHAIN_BIN_DIR CROSS_COMPILE

echo "==> userspace build using $CROSS_COMPILE (from $TOOLCHAIN_BIN_DIR)"

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

UNTAR_SRC="$REPO/userspace/src/untar.c"
UNTAR_BIN="$REPO/userspace/src/untar"
if [ -f "$UNTAR_SRC" ]; then
    if [ "$FORCE" -eq 1 ] || [ ! -f "$UNTAR_BIN" ] || [ "$UNTAR_SRC" -nt "$UNTAR_BIN" ]; then
        echo "==> building userspace/src/untar"
        "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
            -o "$UNTAR_BIN" "$UNTAR_SRC"
        "${CROSS_COMPILE}strip" "$UNTAR_BIN" 2>/dev/null || true
    else
        echo "==> userspace/src/untar already up to date"
    fi
else
    echo "==> skipping untar (no $UNTAR_SRC)"
fi

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
if [ -x "$REPO/tools/userspace/build-opkg.sh" ]; then
    echo "==> building opkg (tools/userspace/build-opkg.sh)"
    if ! sh "$REPO/tools/userspace/build-opkg.sh" $FORCE_ARG; then
        echo "==> opkg build FAILED -- continuing without a package manager" >&2
        echo "    (it needs libarchive staged by tools/userspace/build-thirdparty-deps.sh;" >&2
        echo "     re-run tools/userspace/build-opkg.sh directly to see the full output)" >&2
    fi
fi

if [ "$SKIP_SSH" -eq 0 ]; then
    echo "==> building SSH file transfer (scp + sftp-server + dropbear)"
    sh "$REPO/tools/userspace/build-ssh.sh" $FORCE_ARG
else
    echo "==> --skip-ssh: not building scp/sftp-server/dropbear"
fi

if [ "$SKIP_KEXEC" -eq 0 ]; then
    echo "==> building kexec (the bootstrap's kexec into stage 2 needs this)"
    sh "$REPO/tools/userspace/build-kexec.sh" $FORCE_ARG
else
    echo "==> --skip-kexec: not building kexec -- the bootstrap will hang forever without it"
fi

if [ "$SKIP_ALSA" -eq 0 ]; then
    echo "==> building ALSA userspace (alsa-lib + alsa-utils)"
    sh "$REPO/tools/userspace/build-alsa.sh" $FORCE_ARG
else
    echo "==> --skip-alsa: not building alsa-lib/alsa-utils"
fi

if [ "$SKIP_MPLAYER" -eq 0 ]; then
    echo "==> building MPlayer"
    sh "$REPO/tools/userspace/build-mplayer.sh" $FORCE_ARG
else
    echo "==> --skip-mplayer: not building MPlayer"
fi

if [ "$SKIP_SDL" -eq 0 ]; then
    echo "==> building SDL 1.2"
    sh "$REPO/tools/userspace/build-sdl.sh" $FORCE_ARG
    echo "==> building SDL_image"
    sh "$REPO/tools/userspace/build-sdl-image.sh" $FORCE_ARG
    echo "==> building SDL_mixer"
    sh "$REPO/tools/userspace/build-sdl-mixer.sh" $FORCE_ARG
else
    echo "==> --skip-sdl: not building SDL"
fi

if [ "$SKIP_ST" -eq 0 ]; then
    if [ -f "$REPO/userspace/stage-target/usr/include/X11/Xlib.h" ]; then
        echo "==> building st"
        sh "$REPO/tools/userspace/build-st.sh" $FORCE_ARG
    else
        echo "==> skipping st (userspace/stage-target has no X11 stack staged yet)"
    fi
else
    echo "==> --skip-st: not building st"
fi

if [ "$SKIP_FLTK" -eq 0 ]; then
    if [ -f "$REPO/userspace/stage-target/usr/lib/pkgconfig/xft.pc" ]; then
        echo "==> building FLTK"
        sh "$REPO/tools/userspace/build-fltk.sh" $FORCE_ARG
    else
        echo "==> skipping FLTK (userspace/stage-target has no X11/Xft stack staged yet)"
    fi
else
    echo "==> --skip-fltk: not building FLTK"
fi

if [ "$SKIP_TOASTERS" -eq 0 ]; then
    if [ -f "$REPO/userspace/stage-target/usr/include/X11/Xlib.h" ]; then
        echo "==> building toasters"
        sh "$REPO/tools/userspace/build-toasters.sh" $FORCE_ARG
    else
        echo "==> skipping toasters (userspace/stage-target has no X11 stack staged yet)"
    fi
else
    echo "==> --skip-toasters: not building toasters"
fi

echo ""
echo "==> userspace build complete"
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
