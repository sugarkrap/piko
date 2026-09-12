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

PIKO_TOOL_BIN="${PIKO_TOOL_BIN:-$REPO/build/target/bin}"
mkdir -p "$PIKO_TOOL_BIN"

for _tool in md5sum untar brightd piko-splash flipd kill mhz pkillx fbtext \
             cardswap zramswap vol hwclock ntpsync; do
    _src="$REPO/userspace/src/$_tool.c"
    _bin="$PIKO_TOOL_BIN/$_tool"
    if [ ! -f "$_src" ]; then
        echo "==> skipping $_tool (no $_src)"
        continue
    fi
    if [ "$FORCE" -eq 0 ] && [ -f "$_bin" ] && [ ! "$_src" -nt "$_bin" ]; then
        echo "==> $_tool already up to date"
        continue
    fi
    echo "==> building $_tool -> $_bin"
    "${CROSS_COMPILE}gcc" -march=armv5te -O2 -static -Wall -Wextra \
        -o "$_bin" "$_src"
    "${CROSS_COMPILE}strip" "$_bin" 2>/dev/null || true
done
unset _tool _src _bin
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
    echo "==> building libpikorom"
    sh "$REPO/tools/userspace/build-libpikorom.sh"
    echo "==> building pikoemu"
    sh "$REPO/tools/userspace/build-pikoemu.sh"
else
    echo "==> --skip-sdl: not building SDL"
fi

if [ "$SKIP_ST" -eq 0 ]; then
    if [ -f "$REPO/build/target/usr/include/X11/Xlib.h" ]; then
        echo "==> building st"
        sh "$REPO/tools/userspace/build-st.sh" $FORCE_ARG
    else
        echo "==> skipping st (build/target has no X11 stack staged yet)"
    fi
else
    echo "==> --skip-st: not building st"
fi

if [ "$SKIP_FLTK" -eq 0 ]; then
    if [ -f "$REPO/build/target/usr/lib/pkgconfig/xft.pc" ]; then
        echo "==> building FLTK"
        sh "$REPO/tools/userspace/build-fltk.sh" $FORCE_ARG
    else
        echo "==> skipping FLTK (build/target has no X11/Xft stack staged yet)"
    fi
else
    echo "==> --skip-fltk: not building FLTK"
fi

if [ "$SKIP_TOASTERS" -eq 0 ]; then
    if [ -f "$REPO/build/target/usr/include/X11/Xlib.h" ]; then
        echo "==> building toasters"
        sh "$REPO/tools/userspace/build-toasters.sh" $FORCE_ARG
    else
        echo "==> skipping toasters (build/target has no X11 stack staged yet)"
    fi
else
    echo "==> --skip-toasters: not building toasters"
fi

echo ""
echo "==> userspace build complete"
if [ -d "$PIKO_TOOL_BIN" ]; then
    echo "    tools:   $PIKO_TOOL_BIN ($(ls -1 "$PIKO_TOOL_BIN" | wc -l) binaries)"
fi
if [ -d "$REPO/build/stage-ssh" ]; then
    echo "    ssh:     $REPO/build/stage-ssh ($(du -sh "$REPO/build/stage-ssh" 2>/dev/null | cut -f1))"
fi
if [ -d "$REPO/build/stage-alsa-runtime" ]; then
    echo "    alsa:    $REPO/build/stage-alsa-runtime ($(du -sh "$REPO/build/stage-alsa-runtime" 2>/dev/null | cut -f1))"
fi
if [ -f "$REPO/build/stage-mplayer/usr/bin/mplayer" ]; then
    echo "    mplayer: $REPO/build/stage-mplayer/usr/bin/mplayer ($(du -h "$REPO/build/stage-mplayer/usr/bin/mplayer" 2>/dev/null | cut -f1))"
fi
if [ -d "$REPO/build/stage-sdl-runtime" ]; then
    echo "    sdl:     $REPO/build/stage-sdl-runtime ($(du -sh "$REPO/build/stage-sdl-runtime" 2>/dev/null | cut -f1))"
fi
if [ -f "$REPO/userspace/src/st/st" ]; then
    echo "    st:      $REPO/userspace/src/st/st ($(du -h "$REPO/userspace/src/st/st" 2>/dev/null | cut -f1))"
fi
if [ -f "$REPO/build/target/usr/lib/libfltk.so.1.3" ]; then
    echo "    fltk:    $REPO/build/target/usr/lib/libfltk.so.1.3 ($(du -h "$REPO/build/target/usr/lib/libfltk.so.1.3" 2>/dev/null | cut -f1))"
fi
if [ -f "$REPO/build/target/bin/toasters" ]; then
    echo "    toasters: $REPO/build/target/bin/toasters ($(du -h "$REPO/build/target/bin/toasters" 2>/dev/null | cut -f1))"
fi
echo ""
echo "    Deploy with tools/build-and-deploy.sh."
echo "    NOTE: the X11/matchbox stack is NOT built here -- see the header."
