#!/bin/sh
set -eu

# Cross-compiles MPlayer for the Zaurus SL-C760 (PXA255, ARMv5TE, soft-float,
# NO FPU/VFP/NEON, uClibc) to play video with ALSA audio (WM8731/Corgi,
# card 0), via EITHER -vo fbdev (straight to /dev/fb0, 640x480/16bpp, for the
# console) OR -vo x11 (into an X window). The X11 output is what the FLTK GUI
# needs: userspace/src/piko-player.cxx embeds this MPlayer in a child window
# with -wid and drives it in slave mode. Both video outputs are compiled in;
# the caller picks one at runtime.
#
# WHY MPLAYER: mpv dropped vo_fbdev years ago (needs GL/DRM/X11, none of
# which exist here); VLC is a heavy plugin/C++/threaded stack that dropped
# fbdev too. MPlayer still has native -vo fbdev/fbdev2 straight to /dev/fb0,
# -ao alsa/-ao oss, and bundles its own FFmpeg-derived decoders in one
# static-ish binary -- it was historically THE video player on PXA-class
# Zaurus hardware, and is the only one of the three that can actually start
# here.
#
# WHY A TARBALL, NOT A SUBMODULE: upstream's own site (mplayerhq.hu) no
# longer resolves to anything trustworthy (TLS cert mismatch, looks
# abandoned/parked). Fossies (https://fossies.org/linux/misc/) mirrors the
# real MPlayer-1.5 release tarball (the last real release, 2022) verified
# against its SHA-256 below -- more reliable than chasing a live git mirror,
# and per the task instructions a tarball beats a submodule here. The
# source tree includes a bundled ffmpeg/ snapshot (self-contained, no
# separate ffmpeg cross-build needed).
#
# Cross-compile traps hit getting this to build (see inline comments at
# each configure/make step for the mechanism):
#
#   1. --as=<cross>-as breaks ARM asm: MPlayer's configure defaults AS to
#      $CC (gcc used as an assembler/preprocessor driver) when left alone.
#      Explicitly pointing --as at raw binutils `as` makes it receive full
#      gcc-style CFLAGS ($(AS) $(ASFLAGS) where ASFLAGS=$(CFLAGS)) that raw
#      `as` can't parse ("invalid option -- 'u'"). Fix: never pass --as=;
#      let it default to $CC.
#   2. GCC >=14 makes implicit-function-declaration a hard error by
#      default; ffmpeg's own configure additionally forces
#      -Werror=implicit-function-declaration. The bundled ffmpeg's
#      libavformat/udp.c and unix.c call closesocket() (a macro that only
#      resolves when their surrounding CONFIG_*_PROTOCOL is on) -- MPlayer's
#      --disable-networking correctly turns off UDP/TCP/etc but does NOT
#      catch UDPLITE or UNIX (different component names, not covered by the
#      generic "networking=no" filter, and UNIX's sys/un.h header check
#      succeeds on this uClibc regardless of networking). Same story for
#      HTTPPROXY/ICECAST and the RTMP-family sub-protocols: they share
#      http.c/network.c internals but aren't disabled by --disable-networking
#      either. Fixed with explicit --disable-protocol= for each. We don't
#      need any of the network stream/protocol code for local AVI playback.
#   3. libavcodec/arm/mlpdsp_armv5te.S (MLP/TrueHD lossless-audio DSP,
#      pulled in by CONFIG_MLP_DECODER/CONFIG_TRUEHD_DECODER) fails to
#      assemble under this binutils ("junk at end of line" / "garbage
#      following instruction" on its numeric local labels) -- an
#      upstream/binutils-version mismatch in long-dead hand ARM asm, not
#      anything specific to this project. MLP/TrueHD (lossless Dolby
#      surround) is never going to run on a 400MHz PXA255 anyway, so it's
#      simply disabled rather than chased further.
#
# Per docs/archive/DEADLETTER-WIFI-SSH.md's documented trap: never pass
# CFLAGS=/LDFLAGS= on the `make` command line (clobbers Makefile +=
# accumulation) -- this script only ever sets them via ./configure's own
# --extra-cflags=/--extra-ldflags=, never on `make`.
#
# ALSA: a parallel effort in this repo (tools/build-alsa.sh) cross-builds
# alsa-lib into userspace/stage-alsa (dev headers + libasound.a). That
# build produces a STATIC-ONLY libasound.a (no .so at all) -- confirmed via
# its installed libasound.la: dependency_libs is just '-lpthread -lrt', no
# -ldl, meaning alsa-lib's PCM/control plugins (hw, plug, dmix, ...) are
# compiled directly into libasound.a rather than dlopen()'d at runtime, so
# static linking doesn't lose any of them. So ALSA adds no runtime dependency
# either way -- it is baked into the binary. (This was once a fully static
# mplayer with zero NEEDED entries; enabling -vo x11 ended that, since the
# X11 stack ships as shared libs only. The binary is now dynamic, NEEDing
# libX11.so.6 and its helpers plus libc.so.0 -- all already in the ROM. See
# the configure and ELF-verification comments below.)
#
# Usage:
#   tools/build-mplayer.sh [--force]
#
# --force re-extracts the source tree and reruns configure + a full rebuild
# even if a build already looks up to date.
#
# Env overrides:
#   MPLAYER_VERSION     default 1.5
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE       default arm-unknown-linux-uclibcgnueabi-
#   ALSA_STAGE_DIR      default <repo>/userspace/stage-alsa (headers + libasound.a for linking)
#   X11_STAGE_DIR       default <repo>/userspace/stage-target (X11 headers + libX11.so for -vo x11)
#   OUT_DIR             default <repo>/userspace/stage-mplayer (device payload, wholesale-copyable)
#   JOBS                default: nproc
#
# Exit codes:
#   0   $OUT_DIR/usr/bin/mplayer built (or already up to date) and verified
#   1   a hard failure (download/checksum, missing ALSA or X11 dependency,
#       configure, build, or a wrong-ABI binary / one that did not link -vo
#       x11 / one that NEEDs a lib the ROM does not ship)

REPO="$(cd "$(dirname "$0")/.." && pwd)"
MPLAYER_VERSION="${MPLAYER_VERSION:-1.5}"
SRC_DIR="$REPO/userspace/src"
BUILD_DIR="$SRC_DIR/MPlayer-$MPLAYER_VERSION"
TARBALL="$SRC_DIR/MPlayer-$MPLAYER_VERSION.tar.xz"
MPLAYER_URL="https://fossies.org/linux/misc/MPlayer-$MPLAYER_VERSION.tar.xz"
MPLAYER_SHA256="650cd55bb3cb44c9b39ce36dac488428559799c5f18d16d98edb2b7256cbbf85"

ALSA_STAGE_DIR="${ALSA_STAGE_DIR:-$REPO/userspace/stage-alsa}"
# The X11/Matchbox stack (tools/build-x11-stack.sh) stages its headers and
# shared libs here. We build MPlayer's -vo x11 against them so it can render
# into a window -- specifically the child window piko-player hands it with
# -wid (see userspace/src/piko-player.cxx). This is the FLTK GUI's video
# engine; without X11 here, -wid is a no-op and the GUI shows nothing.
X11_STAGE_DIR="${X11_STAGE_DIR:-$REPO/userspace/stage-target}"
OUT_DIR="${OUT_DIR:-$REPO/userspace/stage-mplayer}"

TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

mkdir -p "$SRC_DIR"

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/build-mplayer.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi

# ALSA is a hard dependency for -ao alsa: fail loudly (not silently degrade
# to OSS-only) if tools/build-alsa.sh hasn't been run yet, so the missing
# piece is obvious rather than discovered later at link time. We link
# statically against libasound.a (see header comment) -- there is no .so
# to depend on at runtime.
ALSA_HEADER="$ALSA_STAGE_DIR/usr/include/alsa/asoundlib.h"
ALSA_LIB="$ALSA_STAGE_DIR/usr/lib/libasound.a"
if [ ! -f "$ALSA_HEADER" ] || [ ! -f "$ALSA_LIB" ]; then
    echo "tools/build-mplayer.sh: ALSA dev files not found under $ALSA_STAGE_DIR" >&2
    echo "  expected: $ALSA_HEADER" >&2
    echo "  expected: $ALSA_LIB" >&2
    echo "MPlayer's -ao alsa needs alsa-lib's headers + libasound.a to link against." >&2
    echo "Run tools/build-alsa.sh first (a separate effort in this repo builds it" >&2
    echo "into userspace/stage-alsa)." >&2
    exit 1
fi

# X11 is likewise a hard dependency now: -vo x11 needs libX11's headers and
# the shared library to link against, both staged by tools/build-x11-stack.sh.
# Fail loudly rather than let MPlayer's configure quietly decide X11 is
# unavailable and build a framebuffer-only binary that -wid cannot drive.
X11_HEADER="$X11_STAGE_DIR/usr/include/X11/Xlib.h"
X11_LIB="$X11_STAGE_DIR/usr/lib/libX11.so"
if [ ! -f "$X11_HEADER" ] || [ ! -f "$X11_LIB" ]; then
    echo "tools/build-mplayer.sh: X11 dev files not found under $X11_STAGE_DIR" >&2
    echo "  expected: $X11_HEADER" >&2
    echo "  expected: $X11_LIB" >&2
    echo "MPlayer's -vo x11 (what piko-player embeds via -wid) needs libX11's" >&2
    echo "headers + shared lib to link against." >&2
    echo "Run tools/build-x11-stack.sh first (it stages them into" >&2
    echo "userspace/stage-target)." >&2
    exit 1
fi

if [ "$FORCE" -eq 1 ]; then
    rm -rf "$BUILD_DIR"
fi

if [ ! -f "$TARBALL" ]; then
    echo "==> downloading $MPLAYER_URL"
    curl -fL -o "$TARBALL.partial" "$MPLAYER_URL"
    mv "$TARBALL.partial" "$TARBALL"
else
    echo "==> reusing cached $TARBALL"
fi

echo "==> verifying sha256"
actual_sha256="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
if [ "$actual_sha256" != "$MPLAYER_SHA256" ]; then
    echo "tools/build-mplayer.sh: SHA-256 mismatch for $TARBALL" >&2
    echo "  expected: $MPLAYER_SHA256" >&2
    echo "  actual:   $actual_sha256" >&2
    echo "Refusing to build from a tarball that doesn't match -- remove it and rerun" >&2
    echo "if you deliberately changed MPLAYER_VERSION/MPLAYER_URL." >&2
    exit 1
fi

if [ ! -d "$BUILD_DIR" ]; then
    echo "==> extracting to $SRC_DIR"
    tar xf "$TARBALL" -C "$SRC_DIR"
fi
if [ ! -f "$BUILD_DIR/configure" ]; then
    echo "tools/build-mplayer.sh: $BUILD_DIR doesn't look like an MPlayer tree (no configure)" >&2
    exit 1
fi

BIN_OUT="$OUT_DIR/usr/bin/mplayer"
if [ "$FORCE" -eq 0 ] && [ -f "$BIN_OUT" ] && [ -f "$BUILD_DIR/mplayer" ] \
   && [ "$BUILD_DIR/mplayer" -nt "$BUILD_DIR/configure" ] && [ "$BIN_OUT" -nt "$BUILD_DIR/mplayer" ]; then
    echo "==> $BIN_OUT already up to date (pass --force to rebuild)"
    exit 0
fi

cd "$BUILD_DIR"

echo "==> configuring MPlayer $MPLAYER_VERSION for arm-linux/armv5te"
# See the header comment above for why: no --as= (breaks ARM .S assembly),
# --enable-alsa (needs $ALSA_STAGE_DIR's headers/libasound.a via
# extra-cflags/extra-ldflags below). --enable-ossaudio kept as a cheap
# fallback ao that needs nothing beyond the kernel's snd-pcm-oss/snd-mixer-oss
# OSS-emulation modules. Toolchain defaults already target -march=armv5tej
# -mfloat-abi=soft (confirmed via `gcc -Q --help=target`) matching the PXA255
# exactly -- no extra -march/-mfpu flags needed or wanted.
#
# NOT fully static any more (the old --enable-static is gone): -vo x11 links
# the X11/Matchbox stack's shared libX11, which is shipped as a .so only (no
# .a), so a fully static link is impossible. The binary comes out dynamic,
# NEEDing libX11.so.6 and its helpers (libXext/libxcb/libXau/libXdmcp) -- all
# already in the ROM's /lib (build-matchbox-payload.sh ships them), plus
# libc.so.0 from the rootfs. libasound is still static (libasound.a has no
# .so), so ALSA adds no runtime dep. extra-cflags/-ldflags now point at BOTH
# the ALSA and the X11 staging trees; -rpath-link lets the cross-linker
# follow libX11's own NEEDED at link time without hardcoding a runtime path.
./configure \
  --target=arm-linux \
  --cc="${CROSS_COMPILE}gcc" \
  --host-cc=gcc \
  --ar="${CROSS_COMPILE}ar" \
  --ranlib="${CROSS_COMPILE}ranlib" \
  --strip="${CROSS_COMPILE}strip" \
  --nm="${CROSS_COMPILE}nm" \
  --enable-cross-compile \
  --extra-cflags="-I${ALSA_STAGE_DIR}/usr/include -I${X11_STAGE_DIR}/usr/include" \
  --extra-ldflags="-L${ALSA_STAGE_DIR}/usr/lib -L${X11_STAGE_DIR}/usr/lib -Wl,-rpath-link,${X11_STAGE_DIR}/usr/lib" \
  --extra-libs="-lasound -lpthread -lrt" \
  --enable-armv5te --disable-armv6 --disable-armv6t2 \
  --disable-armvfp --disable-vfpv3 --disable-neon --disable-iwmmxt --disable-thumb \
  --disable-runtime-cpudetection \
  --disable-relocatable \
  \
  --disable-mencoder \
  --disable-gui \
  --enable-x11 --disable-xv --disable-xvmc --disable-vdpau --disable-vda \
  --disable-gl --disable-matrixview --disable-direct3d --disable-directx \
  --disable-vm --disable-xinerama --disable-xss --disable-xshape \
  --disable-dga1 --disable-dga2 \
  --disable-sdl --disable-ggi --disable-ggiwmh --disable-aa --disable-caca \
  --disable-directfb --disable-svga --disable-vesa \
  --disable-mga --disable-xmga --disable-wii --disable-3dfx --disable-tdfxfb \
  --disable-tdfxvid --disable-s3fb --disable-zr --disable-bl --disable-dxr2 \
  --disable-dxr3 --disable-v4l2 --disable-dvb --disable-vidix --disable-vidix-pcidb \
  --disable-xvr100 --disable-kva --disable-corevideo --disable-quartz --disable-mlib \
  --disable-tga --disable-pnm --disable-md5sum --disable-yuv4mpeg \
  --enable-fbdev \
  \
  --enable-alsa --enable-ossaudio \
  --disable-arts --disable-esd --disable-pulse --disable-jack --disable-openal \
  --disable-nas --disable-sgiaudio --disable-sndio --disable-sunaudio --disable-kai \
  --disable-dart --disable-win32waveout --disable-coreaudio \
  --disable-termcap \
  \
  --disable-networking --disable-smb --disable-live --disable-nemesi \
  --disable-librtmp --disable-ftp --disable-vstream --disable-gnutls \
  --disable-protocol=udplite --disable-protocol=unix \
  --disable-protocol=ftp --disable-protocol=gophers --disable-protocol=hls \
  --disable-protocol=icecast --disable-protocol=httpproxy \
  --disable-protocol=rtmpe --disable-protocol=rtmps --disable-protocol=rtmpt \
  --disable-protocol=rtmpte --disable-protocol=rtmpts --disable-protocol=ffrtmphttp \
  --disable-protocol=srtp --disable-protocol=prompeg --disable-protocol=async \
  \
  --disable-vcd --disable-bluray --disable-dvdnav --disable-dvdread \
  --disable-cdparanoia --disable-cddb --disable-libcdio --disable-liblzo \
  --disable-freetype --disable-fontconfig --disable-fribidi --disable-enca \
  --disable-ass --disable-ass-internal --disable-menu --disable-unrarexec \
  \
  --disable-radio --disable-radio-capture --disable-radio-v4l2 --disable-radio-bsdbt848 \
  --disable-tv --disable-tv-v4l1 --disable-tv-v4l2 --disable-tv-bsdbt848 \
  --disable-pvr --disable-rtc --disable-lirc --disable-lircc \
  --disable-apple-remote --disable-apple-ir \
  \
  --disable-win32dll --disable-qtx --disable-xanim --disable-real \
  --disable-xvid --disable-xvid-lavc --disable-x264 --disable-x264-lavc \
  --disable-libvpx-lavc --disable-libdav1d-lavc --disable-libaom-lavc \
  --disable-libnut --disable-libopenjpeg --disable-libopencore_amrnb \
  --disable-libopencore_amrwb --disable-crystalhd --disable-musepack \
  --disable-libmpeg2 --disable-libdca --disable-liba52 \
  --disable-mpg123 --disable-mad --disable-mp3lame --disable-mp3lame-lavc \
  --disable-toolame --disable-twolame --disable-libgsm --disable-libbs2b \
  --disable-libdv --disable-libilbc --disable-libopus --disable-speex \
  --disable-tremor --disable-libvorbis --disable-theora --disable-faad \
  --disable-faac --disable-ladspa --disable-xmms \
  --disable-libxml2 \
  --disable-libavcodec_mpegaudio_hp \
  --disable-decoder=mlp --disable-decoder=truehd

echo "==> building (make -j$JOBS)"
make -j"$JOBS"

if [ ! -f mplayer ]; then
    echo "tools/build-mplayer.sh: build finished but ./mplayer is missing" >&2
    exit 1
fi

echo "==> verifying ELF class"
READELF="${CROSS_COMPILE}readelf"
elf_flags="$("$READELF" -h mplayer | sed -n 's/^ *Flags: *//p')"
case "$elf_flags" in
    0x5000200*) : ;;
    *)
        echo "tools/build-mplayer.sh: unexpected ELF Flags: $elf_flags (want 0x5000200, Version5 EABI, soft-float ABI -- matching normal userspace binaries in this repo, e.g. Xfbdev)" >&2
        exit 1
        ;;
esac
# This is the X11 build (see the configure comment): a dynamic binary, no
# longer the old zero-dep static one. The one thing that MUST be true is that
# -vo x11 actually linked -- if configure had silently decided X11 was
# unavailable it would build a framebuffer-only mplayer that -wid cannot
# drive, and it would do so without failing. libX11.so.6 in DT_NEEDED is the
# proof it did not. Every NEEDED lib beyond libc.so.0 must be one the ROM's
# /lib already carries (build-matchbox-payload.sh's LIBS); we check the X11
# set explicitly and print the rest for eyeballing.
needed="$("$READELF" -d mplayer 2>/dev/null | awk '/NEEDED/{print $NF}' | tr -d '[]' | tr '\n' ' ')"
case " $needed " in
    *" libX11.so.6 "*) : ;;
    *)
        echo "tools/build-mplayer.sh: mplayer does not NEED libX11.so.6 -- -vo x11" >&2
        echo "did not link. configure likely failed to find X11 under" >&2
        echo "$X11_STAGE_DIR and quietly built a framebuffer-only binary that" >&2
        echo "piko-player's -wid embedding cannot use. NEEDED was: $needed" >&2
        exit 1
        ;;
esac
# Every one of these is shipped by the ROM payload (libX11 libXext libxcb
# libXau libXdmcp) or provided by the rootfs (libc.so.0). Anything else here
# would be a lib nothing ships -- flag it now rather than at exec time on the
# device with a bare "not found".
for n in $needed; do
    case "$n" in
        libX11.so.*|libXext.so.*|libxcb.so.*|libXau.so.*|libXdmcp.so.*|libc.so.*) : ;;
        *)
            echo "tools/build-mplayer.sh: mplayer NEEDs $n, which the ROM does not ship." >&2
            echo "Add it to build-matchbox-payload.sh's LIBS, or find why it got linked." >&2
            echo "Full NEEDED: $needed" >&2
            exit 1
            ;;
    esac
done
echo "    Flags: $elf_flags"
echo "    NEEDED: $needed"

echo "==> staging into $OUT_DIR"
mkdir -p "$OUT_DIR/usr/bin"
cp mplayer "$BIN_OUT"
"${CROSS_COMPILE}strip" "$BIN_OUT"

size_bytes="$(wc -c < "$BIN_OUT")"
echo "==> done: $BIN_OUT ($size_bytes bytes stripped, dynamic; NEEDED: $needed)"
