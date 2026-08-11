#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC="$REPO/userspace/src"
FLTK_SRC_DIR="$SRC/fltk"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST/bin}"
BUILD_ARCH="$(uname -m)-pc-linux-gnu"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

FL_DSO_VERSION=1.3
MARKER="$STAGE/usr/lib/libfltk.so.$FL_DSO_VERSION"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -d "$FLTK_SRC_DIR" ] || [ ! -f "$FLTK_SRC_DIR/configure.ac" ]; then
    echo "tools/userspace/build-fltk.sh: $FLTK_SRC_DIR is not checked out" >&2
    echo "Run: git submodule update --init userspace/src/fltk" >&2
    exit 1
fi

if [ ! -d "$TOOLCHAIN_BIN_DIR" ]; then
    echo "tools/userspace/build-fltk.sh: toolchain bin dir not found: $TOOLCHAIN_BIN_DIR" >&2
    echo "Run tools/toolchain/build-uclibc-toolchain.sh first, or set TOOLCHAIN_BIN_DIR." >&2
    exit 1
fi

if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-g++" >/dev/null 2>&1; then
    echo "tools/userspace/build-fltk.sh: no C++ cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-g++" >&2
    echo "FLTK is C++; rebuild the toolchain with C++ support." >&2
    exit 1
fi

if [ ! -f "$STAGE/usr/include/X11/Xlib.h" ] || [ ! -f "$STAGE/usr/lib/pkgconfig/xft.pc" ]; then
    echo "tools/userspace/build-fltk.sh: no X11 stack staged at $STAGE" >&2
    echo "FLTK links libX11/libXft/libXrender/fontconfig/freetype from there --" >&2
    echo "build the X11/Matchbox stack first." >&2
    exit 1
fi

PATH="$TOOLCHAIN_BIN_DIR:$PATH"
export PATH
export CC="$HOST-gcc"
export CXX="$HOST-g++"
export AR="$HOST-ar"
export RANLIB="$HOST-ranlib"
export STRIP="$HOST-strip"
export PKG_CONFIG_SYSROOT_DIR="$STAGE"
export PKG_CONFIG_LIBDIR="$STAGE/usr/lib/pkgconfig:$STAGE/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
export CPPFLAGS="-I$STAGE/usr/include"
export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"

if [ "$FORCE" -eq 0 ] && [ -f "$MARKER" ]; then
    echo "==> FLTK library already staged, skipping build ($MARKER)"
else
    echo "==> FLTK $(git -C "$FLTK_SRC_DIR" describe --tags 2>/dev/null || echo '(unknown revision)')"

    (
        cd "$FLTK_SRC_DIR"

        if [ "$FORCE" -eq 1 ] && [ -f makeinclude ]; then
            echo "    --force: make distclean"
            make distclean >/dev/null 2>&1 || true
        fi

        if [ ! -f configure ]; then
            echo "    generating configure (autogen.sh)"
            if ! command -v autoconf >/dev/null 2>&1; then
                echo "tools/userspace/build-fltk.sh: autoconf not installed on this host" >&2
                echo "FLTK ships no pre-generated configure in git." >&2
                exit 1
            fi
            NOCONFIGURE=1 ./autogen.sh
        fi

        echo "    configuring"
        ./configure \
            --host="$HOST" \
            --build="$BUILD_ARCH" \
            --prefix=/usr \
            --x-includes="$STAGE/usr/include" \
            --x-libraries="$STAGE/usr/lib" \
            --enable-shared \
            --enable-threads \
            --enable-xft \
            --enable-xdbe \
            --enable-xrender \
            --enable-localjpeg \
            --disable-localzlib \
            --disable-localpng \
            --disable-gl \
            --disable-xinerama \
            --disable-xfixes \
            --disable-xcursor \
            --disable-cairo

        imagedirs="$(sed -n 's/^IMAGEDIRS[[:space:]]*=[[:space:]]*//p' makeinclude)"
        echo "    building:$(printf ' %s' $imagedirs) src"
        for d in $imagedirs src; do
            make -C "$d" -j"$JOBS"
        done

        echo "    installing into $STAGE"
        make install DESTDIR="$STAGE" DIRS="$imagedirs src"
    )

    if [ ! -f "$MARKER" ]; then
        echo "tools/userspace/build-fltk.sh: build finished but $MARKER is missing" >&2
        exit 1
    fi

    rm -f "$STAGE"/usr/lib/libfltk*.la 2>/dev/null || true

    echo "==> verifying the staged library is ARM, soft-float, and shared"
    elf_flags="$("$HOST-readelf" -h "$MARKER" | sed -n 's/^ *Flags: *//p')"
    case "$elf_flags" in
        0x5000200*) : ;;
        *)
            echo "tools/userspace/build-fltk.sh: unexpected ELF Flags: $elf_flags" >&2
            echo "(want 0x5000200 -- Version5 EABI, soft-float, matching Xfbdev/libX11)" >&2
            exit 1
            ;;
    esac
    soname="$("$HOST-readelf" -d "$MARKER" | sed -n 's/.*SONAME.*\[\(.*\)\]/\1/p')"
    if [ "$soname" != "libfltk.so.$FL_DSO_VERSION" ]; then
        echo "tools/userspace/build-fltk.sh: SONAME is '$soname', expected libfltk.so.$FL_DSO_VERSION" >&2
        echo "tools/userspace/build-matchbox-payload.sh ships the file under its own name and" >&2
        echo "relies on that being the SONAME; a mismatch would deploy a library the" >&2
        echo "dynamic linker can never find." >&2
        exit 1
    fi
    echo "    Flags: $elf_flags  SONAME: $soname"
fi

FLTKTEST_SRC="$SRC/fltktest.cxx"
if [ -f "$FLTKTEST_SRC" ]; then
    echo "==> building fltktest against the freshly staged libfltk"
    fltk_ldlibs="$(sed -n 's/^LDLIBS[[:space:]]*=[[:space:]]*//p' "$FLTK_SRC_DIR/makeinclude")"
    mkdir -p "$STAGE/usr/bin"
    "$CXX" -O2 -Wall -Wextra \
        -isystem "$STAGE/usr/include" \
        -o "$STAGE/usr/bin/fltktest" \
        "$FLTKTEST_SRC" \
        -L"$STAGE/usr/lib" -Wl,-rpath-link="$STAGE/usr/lib" \
        -lfltk $fltk_ldlibs

    needed="$("$HOST-readelf" -d "$STAGE/usr/bin/fltktest" | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
    echo "    NEEDED: $needed"
    case " $needed " in
        *" libfltk.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/userspace/build-fltk.sh: fltktest does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping fltktest (no $FLTKTEST_SRC)"
fi

FBRUN_SRC="$SRC/matchbox-fbrun.cxx"
if [ -f "$FBRUN_SRC" ]; then
    echo "==> building matchbox-fbrun"
    fbrun_ldlibs="$(sed -n 's/^LDLIBS[[:space:]]*=[[:space:]]*//p' "$FLTK_SRC_DIR/makeinclude")"
    mkdir -p "$STAGE/usr/bin"
    "$CXX" -O2 -Wall -Wextra \
        -isystem "$STAGE/usr/include" \
        -o "$STAGE/usr/bin/matchbox-fbrun" \
        "$FBRUN_SRC" \
        -L"$STAGE/usr/lib" -Wl,-rpath-link="$STAGE/usr/lib" \
        -lfltk $fbrun_ldlibs

    needed="$("$HOST-readelf" -d "$STAGE/usr/bin/matchbox-fbrun" | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
    echo "    NEEDED: $needed"
    case " $needed " in
        *" libfltk.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/userspace/build-fltk.sh: matchbox-fbrun does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping matchbox-fbrun (no $FBRUN_SRC)"
fi

WALLPAPER_PICKER_SRC="$SRC/mb-wallpaper-picker.cxx"
PANEL_SRCS="$SRC/panels/panel-wallpaper.cxx"
if [ -f "$WALLPAPER_PICKER_SRC" ]; then
    echo "==> building mb-wallpaper-picker"
    fbrun_ldlibs="$(sed -n 's/^LDLIBS[[:space:]]*=[[:space:]]*//p' "$FLTK_SRC_DIR/makeinclude")"
    mkdir -p "$STAGE/usr/bin"
    "$CXX" -O2 -Wall -Wextra \
        -isystem "$STAGE/usr/include" -I"$SRC" \
        -o "$STAGE/usr/bin/mb-wallpaper-picker" \
        "$WALLPAPER_PICKER_SRC" $PANEL_SRCS \
        -L"$STAGE/usr/lib" -Wl,-rpath-link="$STAGE/usr/lib" \
        -lfltk_images -lpng -lz -lfltk $fbrun_ldlibs

    needed="$("$HOST-readelf" -d "$STAGE/usr/bin/mb-wallpaper-picker" | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
    echo "    NEEDED: $needed"
    case " $needed " in
        *" libfltk.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/userspace/build-fltk.sh: mb-wallpaper-picker does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac
    case " $needed " in
        *" libfltk_images.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/userspace/build-fltk.sh: mb-wallpaper-picker does not NEED libfltk_images.so.$FL_DSO_VERSION" >&2
            echo "-- thumbnails would fail to decode on the device." >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping mb-wallpaper-picker (no $WALLPAPER_PICKER_SRC)"
fi

PIKO_SETTINGS_SRC="$SRC/piko-settings.cxx"
if [ -f "$PIKO_SETTINGS_SRC" ]; then
    echo "==> building piko-settings"
    settings_ldlibs="$(sed -n 's/^LDLIBS[[:space:]]*=[[:space:]]*//p' "$FLTK_SRC_DIR/makeinclude")"
    mkdir -p "$STAGE/usr/bin"
    "$CXX" -O2 -Wall -Wextra \
        -isystem "$STAGE/usr/include" -I"$SRC" \
        -o "$STAGE/usr/bin/piko-settings" \
        "$PIKO_SETTINGS_SRC" $PANEL_SRCS \
        -L"$STAGE/usr/lib" -Wl,-rpath-link="$STAGE/usr/lib" \
        -lfltk_images -lpng -lz -lfltk $settings_ldlibs

    needed="$("$HOST-readelf" -d "$STAGE/usr/bin/piko-settings" | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
    echo "    NEEDED: $needed"
    case " $needed " in
        *" libfltk.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/userspace/build-fltk.sh: piko-settings does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac
    case " $needed " in
        *" libfltk_images.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/userspace/build-fltk.sh: piko-settings does not NEED libfltk_images.so.$FL_DSO_VERSION" >&2
            echo "-- every entry's icon would fail to decode on the device." >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping piko-settings (no $PIKO_SETTINGS_SRC)"
fi

PIKO_PLAYER_SRC="$SRC/piko-player.cxx"
if [ -f "$PIKO_PLAYER_SRC" ]; then
    echo "==> building piko-player"
    player_ldlibs="$(sed -n 's/^LDLIBS[[:space:]]*=[[:space:]]*//p' "$FLTK_SRC_DIR/makeinclude")"
    mkdir -p "$STAGE/usr/bin"
    "$CXX" -O2 -Wall -Wextra \
        -isystem "$STAGE/usr/include" \
        -o "$STAGE/usr/bin/piko-player" \
        "$PIKO_PLAYER_SRC" \
        -L"$STAGE/usr/lib" -Wl,-rpath-link="$STAGE/usr/lib" \
        -lfltk $player_ldlibs

    needed="$("$HOST-readelf" -d "$STAGE/usr/bin/piko-player" | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
    echo "    NEEDED: $needed"
    case " $needed " in
        *" libfltk.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/userspace/build-fltk.sh: piko-player does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping piko-player (no $PIKO_PLAYER_SRC)"
fi

echo ""
echo "==> done: FLTK staged in $STAGE"
for f in "$STAGE"/usr/lib/libfltk*.so."$FL_DSO_VERSION"; do
    [ -f "$f" ] && echo "    $f ($(du -h "$f" | cut -f1))"
done
[ -f "$STAGE/usr/bin/fltktest" ] && \
    echo "    $STAGE/usr/bin/fltktest ($(du -h "$STAGE/usr/bin/fltktest" | cut -f1))"
echo ""
echo "    Ships to the device inside the X11/Matchbox payload:"
echo "        tools/userspace/build-matchbox-payload.sh [--deploy]"
