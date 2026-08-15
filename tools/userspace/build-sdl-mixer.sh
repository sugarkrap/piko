#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SDL_MIXER_VERSION="${SDL_MIXER_VERSION:-1.2.12}"
SRC_DIR="$REPO/userspace/src"

SDLMIX_SRC_DIR="${SDLMIX_SRC_DIR:-$SRC_DIR/SDL_mixer-$SDL_MIXER_VERSION}"
SDLMIX_TARBALL="${SDLMIX_TARBALL:-$SRC_DIR/SDL_mixer-$SDL_MIXER_VERSION.tar.gz}"
SDLMIX_URL="https://www.libsdl.org/projects/SDL_mixer/release/SDL_mixer-$SDL_MIXER_VERSION.tar.gz"
SDLMIX_SHA256="1644308279a975799049e4826af2cfc787cad2abb11aa14562e402521f86992a"

SDL_STAGE_DIR="${SDL_STAGE_DIR:-$REPO/userspace/stage-sdl}"
STAGE_DIR="${STAGE_DIR:-$REPO/userspace/stage-sdl}"
THIRDPARTY_STAGE="${THIRDPARTY_STAGE:-$REPO/userspace/stage-target}"
RUNTIME_DIR="${RUNTIME_DIR:-$REPO/userspace/stage-sdl-runtime}"

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
    echo "tools/userspace/build-sdl-mixer.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"

if [ ! -x "$SDL_STAGE_DIR/usr/bin/sdl-config" ]; then
    echo "tools/userspace/build-sdl-mixer.sh: $SDL_STAGE_DIR/usr/bin/sdl-config not found." >&2
    echo "Run tools/userspace/build-sdl.sh first." >&2
    exit 1
fi

if [ "$FORCE" -eq 1 ] && [ -d "$SDLMIX_SRC_DIR" ]; then
    echo "==> --force: removing existing $SDLMIX_SRC_DIR"
    rm -rf "$SDLMIX_SRC_DIR"
fi

if [ ! -f "$SDLMIX_TARBALL" ]; then
    echo "==> downloading $SDLMIX_URL"
    curl -fL -o "$SDLMIX_TARBALL.partial" "$SDLMIX_URL"
    mv "$SDLMIX_TARBALL.partial" "$SDLMIX_TARBALL"
else
    echo "==> reusing cached $SDLMIX_TARBALL"
fi

echo "==> verifying sha256"
actual_sha256="$(sha256sum "$SDLMIX_TARBALL" | cut -d' ' -f1)"
if [ "$actual_sha256" != "$SDLMIX_SHA256" ]; then
    echo "tools/userspace/build-sdl-mixer.sh: SHA-256 mismatch for $SDLMIX_TARBALL" >&2
    echo "  expected: $SDLMIX_SHA256" >&2
    echo "  actual:   $actual_sha256" >&2
    exit 1
fi

if [ ! -d "$SDLMIX_SRC_DIR" ]; then
    echo "==> extracting SDL_mixer-$SDL_MIXER_VERSION to $SRC_DIR"
    tar xzf "$SDLMIX_TARBALL" -C "$SRC_DIR"
fi
if [ ! -f "$SDLMIX_SRC_DIR/configure" ]; then
    echo "tools/userspace/build-sdl-mixer.sh: $SDLMIX_SRC_DIR doesn't look like a configure-based tree" >&2
    exit 1
fi

SDL_CONFIG_WRAPPER="$SRC_DIR/.sdl-config-wrapper-mixer"
cat > "$SDL_CONFIG_WRAPPER" <<EOF
#!/bin/sh
exec "$SDL_STAGE_DIR/usr/bin/sdl-config" --prefix="$SDL_STAGE_DIR/usr" "\$@"
EOF
chmod +x "$SDL_CONFIG_WRAPPER"

echo "==> configuring SDL_mixer $SDL_MIXER_VERSION"
(
    cd "$SDLMIX_SRC_DIR"
    [ -f Makefile ] && make distclean >/dev/null 2>&1
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --disable-static --enable-shared \
        --disable-sdltest \
        --disable-music-mod \
        --disable-music-ogg \
        --disable-music-mp3 \
        --disable-music-flac \
        --enable-music-wave \
        --enable-music-midi \
        --enable-music-timidity-midi \
        --disable-music-fluidsynth-midi \
        --disable-music-native-midi \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
        SDL_CONFIG="$SDL_CONFIG_WRAPPER" \
        LDFLAGS="-L$THIRDPARTY_STAGE/usr/lib -Wl,-rpath-link=$THIRDPARTY_STAGE/usr/lib"
    echo "==> building SDL_mixer"
    make -j"$JOBS" build/libSDL_mixer.la
    echo "==> installing SDL_mixer to $STAGE_DIR"
    make install-hdrs install-lib DESTDIR="$STAGE_DIR"
)

LIBSDLMIX_LA="$STAGE_DIR/usr/lib/libSDL_mixer.la"
if [ -f "$LIBSDLMIX_LA" ]; then
    sed -i "s|^libdir='/usr/lib'|libdir='$STAGE_DIR/usr/lib'|" "$LIBSDLMIX_LA"
fi

SDLMIX_SO_REAL="$(cd "$STAGE_DIR/usr/lib" && ls libSDL_mixer-1.2.so.0.* 2>/dev/null | head -1)"
if [ -z "$SDLMIX_SO_REAL" ]; then
    echo "tools/userspace/build-sdl-mixer.sh: build finished but no libSDL_mixer-1.2.so.0.* found in $STAGE_DIR/usr/lib" >&2
    exit 1
fi

echo "==> verifying ELF class of $SDLMIX_SO_REAL"
elf_flags="$("$READELF" -h "$STAGE_DIR/usr/lib/$SDLMIX_SO_REAL" | sed -n 's/^ *Flags: *//p')"
case "$elf_flags" in
    0x5000200*) : ;;
    *)
        echo "tools/userspace/build-sdl-mixer.sh: unexpected ELF Flags: $elf_flags (want 0x5000200, Version5 EABI, soft-float ABI)" >&2
        exit 1
        ;;
esac
echo "    Flags: $elf_flags"

needed="$("$READELF" -d "$STAGE_DIR/usr/lib/$SDLMIX_SO_REAL" 2>/dev/null | awk '/NEEDED/{print $NF}' | tr -d '[]')"
echo "    NEEDED: $(echo "$needed" | tr '\n' ' ')"
case "$needed" in
    *libvorbis*|*libmad*|*libmikmod*|*libFLAC*)
        echo "tools/userspace/build-sdl-mixer.sh: unexpectedly linked against an optional codec library" >&2
        exit 1
        ;;
esac
case "$needed" in
    *libSDL-1.2.so.0*) : ;;
    *)
        echo "tools/userspace/build-sdl-mixer.sh: $SDLMIX_SO_REAL does not NEED libSDL-1.2.so.0" >&2
        exit 1
        ;;
esac

mkdir -p "$RUNTIME_DIR/usr/lib"
cp "$STAGE_DIR/usr/lib/$SDLMIX_SO_REAL" "$RUNTIME_DIR/usr/lib/"
chmod u+w "$RUNTIME_DIR/usr/lib/$SDLMIX_SO_REAL"
"$STRIP" --strip-unneeded "$RUNTIME_DIR/usr/lib/$SDLMIX_SO_REAL"
ln -sf "$SDLMIX_SO_REAL" "$RUNTIME_DIR/usr/lib/libSDL_mixer-1.2.so.0"

echo ""
echo "==> done: $RUNTIME_DIR/usr/lib/$SDLMIX_SO_REAL"
echo "    $RUNTIME_DIR/usr/lib/libSDL_mixer-1.2.so.0 -> $SDLMIX_SO_REAL"
