#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
MPLAYER_VERSION="${MPLAYER_VERSION:-1.5}"
SRC_DIR="$REPO/userspace/src"
BUILD_DIR="$SRC_DIR/MPlayer-$MPLAYER_VERSION"
TARBALL="$SRC_DIR/MPlayer-$MPLAYER_VERSION.tar.xz"
MPLAYER_URL="https://fossies.org/linux/misc/MPlayer-$MPLAYER_VERSION.tar.xz"
MPLAYER_SHA256="650cd55bb3cb44c9b39ce36dac488428559799c5f18d16d98edb2b7256cbbf85"

ALSA_STAGE_DIR="${ALSA_STAGE_DIR:-$REPO/userspace/stage-alsa}"
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
    echo "tools/userspace/build-mplayer.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi

ALSA_HEADER="$ALSA_STAGE_DIR/usr/include/alsa/asoundlib.h"
ALSA_LIB="$ALSA_STAGE_DIR/usr/lib/libasound.a"
if [ ! -f "$ALSA_HEADER" ] || [ ! -f "$ALSA_LIB" ]; then
    echo "tools/userspace/build-mplayer.sh: ALSA dev files not found under $ALSA_STAGE_DIR" >&2
    echo "  expected: $ALSA_HEADER" >&2
    echo "  expected: $ALSA_LIB" >&2
    echo "MPlayer's -ao alsa needs alsa-lib's headers + libasound.a to link against." >&2
    echo "Run tools/userspace/build-alsa.sh first (a separate effort in this repo builds it" >&2
    echo "into userspace/stage-alsa)." >&2
    exit 1
fi

X11_HEADER="$X11_STAGE_DIR/usr/include/X11/Xlib.h"
X11_LIB="$X11_STAGE_DIR/usr/lib/libX11.so"
if [ ! -f "$X11_HEADER" ] || [ ! -f "$X11_LIB" ]; then
    echo "tools/userspace/build-mplayer.sh: X11 dev files not found under $X11_STAGE_DIR" >&2
    echo "  expected: $X11_HEADER" >&2
    echo "  expected: $X11_LIB" >&2
    echo "MPlayer's -vo x11 (what piko-player embeds via -wid) needs libX11's" >&2
    echo "headers + shared lib to link against." >&2
    echo "Run tools/userspace/build-x11-stack.sh first (it stages them into" >&2
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
    echo "tools/userspace/build-mplayer.sh: SHA-256 mismatch for $TARBALL" >&2
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
    echo "tools/userspace/build-mplayer.sh: $BUILD_DIR doesn't look like an MPlayer tree (no configure)" >&2
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
    echo "tools/userspace/build-mplayer.sh: build finished but ./mplayer is missing" >&2
    exit 1
fi

echo "==> verifying ELF class"
READELF="${CROSS_COMPILE}readelf"
elf_flags="$("$READELF" -h mplayer | sed -n 's/^ *Flags: *//p')"
case "$elf_flags" in
    0x5000200*) : ;;
    *)
        echo "tools/userspace/build-mplayer.sh: unexpected ELF Flags: $elf_flags (want 0x5000200, Version5 EABI, soft-float ABI -- matching normal userspace binaries in this repo, e.g. Xfbdev)" >&2
        exit 1
        ;;
esac
needed="$("$READELF" -d mplayer 2>/dev/null | awk '/NEEDED/{print $NF}' | tr -d '[]' | tr '\n' ' ')"
case " $needed " in
    *" libX11.so.6 "*) : ;;
    *)
        echo "tools/userspace/build-mplayer.sh: mplayer does not NEED libX11.so.6 -- -vo x11" >&2
        echo "did not link. configure likely failed to find X11 under" >&2
        echo "$X11_STAGE_DIR and quietly built a framebuffer-only binary that" >&2
        echo "piko-player's -wid embedding cannot use. NEEDED was: $needed" >&2
        exit 1
        ;;
esac
for n in $needed; do
    case "$n" in
        libX11.so.*|libXext.so.*|libxcb.so.*|libXau.so.*|libXdmcp.so.*|libc.so.*) : ;;
        *)
            echo "tools/userspace/build-mplayer.sh: mplayer NEEDs $n, which the ROM does not ship." >&2
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
