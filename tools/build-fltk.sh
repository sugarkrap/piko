#!/bin/sh
set -eu

# Cross-builds FLTK 1.3 (userspace/src/fltk, a tracked git submodule) as a
# SHARED library into the X11 staging tree at userspace/stage-target, plus a
# tiny smoke-test app (userspace/src/fltktest.cxx) that opens a real window
# on the device.
#
# WHY IT LIVES WITH THE X11 STACK, NOT WITH ALSA/MPLAYER/SDL: FLTK here is
# an X11 toolkit -- it links libX11/libXft/libXrender/libXext/fontconfig/
# freetype out of userspace/stage-target and is useless without them. So it
# installs into that same staging tree (like tools/build-thirdparty-deps.sh
# does for zlib/freetype/fontconfig), and tools/build-matchbox-payload.sh
# ships libfltk*.so.1.3 in the X11/Matchbox payload rather than inventing a
# separate stage-fltk-runtime the way SDL needed one. This script fails
# loudly rather than silently skipping when that stage doesn't exist --
# same policy as tools/build-st.sh.
#
# WHY 1.3 AND NOT 1.4/1.5: 1.3.11 is the last release of the autotools,
# C++98, X11-only series -- exactly the shape every other component in this
# tree has. 1.4 makes CMake the primary build system, wants C++11, and
# defaults to a Wayland backend plus Pango/Cairo font handling; on a PXA255
# with 64MB of RAM and a kdrive X server, all of that is either dead weight
# or an extra dependency chain for no gain.
#
# CONFIGURE CHOICES THAT ARE NOT OBVIOUS:
#
#   --x-includes / --x-libraries
#       FLTK finds X through AC_PATH_XTRA, which searches a hardcoded list
#       of HOST paths and knows nothing about a cross sysroot. Without these
#       it either finds nothing or -- worse -- finds the build machine's own
#       /usr/include/X11. Same reason build-x11-stack.sh passes them to
#       matchbox-window-manager.
#
#   --enable-xft  (not merely "left at its default yes")
#       This device has NO core X bitmap fonts at all: the only fonts in the
#       payload are the DejaVu TTFs that fontconfig serves. Without Xft,
#       FLTK falls back to core X fonts and every label renders blank. The
#       explicit --enable-xft makes configure ABORT if Xft is missing
#       instead of quietly producing that unusable library.
#
#   --enable-localjpeg but --disable-localzlib --disable-localpng
#       FLTK bundles copies of all three. zlib and libpng are already
#       cross-built and staged for the X11 stack, and shipping a second copy
#       of each inside libfltk_images on a flash-constrained device is
#       pointless -- so those two come from the stage. There is no system
#       libjpeg staged (nothing else here needs one), so JPEG stays bundled;
#       it is compiled -fPIC along with everything else (configure sets
#       OPTIM="-fPIC" whenever --enable-shared is on) so linking the static
#       libfltk_jpeg.a into a shared libfltk_images.so is fine.
#
#   --disable-xinerama --disable-xfixes --disable-xcursor --disable-gl
#       None of these are staged and none make sense here (one screen, no
#       GPU). They would autodetect to "off" anyway; passing them explicitly
#       means a build host that happens to have them installed cannot change
#       what gets built. Same reasoning as tools/build-sdl.sh's --disable-*
#       wall.
#
# Only the image libs and src/ are built. Upstream's default `make` also
# builds fluid/, test/ and documentation/: fluid is a TARGET binary that
# cannot run on the build host, test/ needs to run it to turn .fl files into
# .cxx, and documentation/ needs doxygen. None of that ships to the device.
#
# Usage:
#   tools/build-fltk.sh [--force]
#
# --force reconfigures and rebuilds from scratch (make distclean first),
# even if the library is already staged.
#
# Env overrides:
#   CROSS_HOST          default arm-unknown-linux-uclibcgnueabi
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/$CROSS_HOST/bin
#   STAGE               default <repo>/userspace/stage-target
#   JOBS                default: nproc
#
# Exit codes:
#   0   libfltk.so.1.3 is staged in $STAGE (or was already current)
#   1   a hard failure (missing submodule, toolchain, X11 stage, or a
#       configure/build/install failure -- output is never swallowed)

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$REPO/userspace/src"
FLTK_SRC_DIR="$SRC/fltk"
STAGE="${STAGE:-$REPO/userspace/stage-target}"

HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST/bin}"
BUILD_ARCH="$(uname -m)-pc-linux-gnu"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"

# The FLTK 1.3 DSO version. Both the real filename and the SONAME are
# libfltk.so.1.3 (FLTK versions its shared libs major.minor, not major),
# which is why tools/build-matchbox-payload.sh can ship the file as-is.
FL_DSO_VERSION=1.3
MARKER="$STAGE/usr/lib/libfltk.so.$FL_DSO_VERSION"

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -d "$FLTK_SRC_DIR" ] || [ ! -f "$FLTK_SRC_DIR/configure.ac" ]; then
    echo "tools/build-fltk.sh: $FLTK_SRC_DIR is not checked out" >&2
    echo "Run: git submodule update --init userspace/src/fltk" >&2
    exit 1
fi

if [ ! -d "$TOOLCHAIN_BIN_DIR" ]; then
    echo "tools/build-fltk.sh: toolchain bin dir not found: $TOOLCHAIN_BIN_DIR" >&2
    echo "Run tools/build-uclibc-toolchain.sh first, or set TOOLCHAIN_BIN_DIR." >&2
    exit 1
fi

# FLTK is C++ -- the only component in this tree that is. A C-only toolchain
# would fail deep inside the build with a confusing error, so check up front.
if ! command -v "$TOOLCHAIN_BIN_DIR/$HOST-g++" >/dev/null 2>&1; then
    echo "tools/build-fltk.sh: no C++ cross compiler at $TOOLCHAIN_BIN_DIR/$HOST-g++" >&2
    echo "FLTK is C++; rebuild the toolchain with C++ support." >&2
    exit 1
fi

if [ ! -f "$STAGE/usr/include/X11/Xlib.h" ] || [ ! -f "$STAGE/usr/lib/pkgconfig/xft.pc" ]; then
    echo "tools/build-fltk.sh: no X11 stack staged at $STAGE" >&2
    echo "FLTK links libX11/libXft/libXrender/fontconfig/freetype from there --" >&2
    echo "build the X11/Matchbox stack first (docs/HOWTO-MATCHBOX-DESKTOP.md)." >&2
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
# -rpath-link (not -rpath): lets the cross-linker resolve INDIRECT
# dependencies at link time -- libXft needs libfreetype, which needs libz --
# without baking any host path into the .so. The device resolves these from
# /lib at runtime. Same note as tools/build-thirdparty-deps.sh.
export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"

# Only the LIBRARY build is skipped when already staged -- fltktest,
# matchbox-fbrun and mb-wallpaper-picker below each have their own
# independent up-to-date check against their own binary, same as every
# other per-component check in tools/build-x11-stack.sh (mb-volume,
# mb-brightness, ...). This used to be a hard `exit 0` right here, which
# meant a worktree that already had libfltk.so.1.3 staged from an earlier
# build silently never got a NEWLY ADDED downstream binary at all -- found
# 2026-08-03 when mb-wallpaper-picker was added to this script: every
# worktree with FLTK already staged from before that change kept skipping
# straight past its build, failing later at payload-assembly time with a
# confusing "missing .../usr/bin/mb-wallpaper-picker" instead of building
# it here where the fix actually belongs.
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

        # The submodule is a git checkout, so there is no generated configure --
        # only configure.ac. autogen.sh runs autoconf/automake and then runs
        # configure itself unless NOCONFIGURE is set; keep the two steps apart so
        # the configure line below is the one that matters and is visible.
        if [ ! -f configure ]; then
            echo "    generating configure (autogen.sh)"
            if ! command -v autoconf >/dev/null 2>&1; then
                echo "tools/build-fltk.sh: autoconf not installed on this host" >&2
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

        # IMAGEDIRS is whichever bundled image libraries configure decided to
        # build (just jpeg/, given the --disable-local{zlib,png} above). Read it
        # back rather than hardcoding, so flipping one of those flags later
        # cannot silently leave a bundled lib unbuilt.
        imagedirs="$(sed -n 's/^IMAGEDIRS[[:space:]]*=[[:space:]]*//p' makeinclude)"
        echo "    building:$(printf ' %s' $imagedirs) src"
        for d in $imagedirs src; do
            make -C "$d" -j"$JOBS"
        done

        # DIRS override: upstream's install walks FL + IMAGEDIRS + src + fluid +
        # test + documentation. Only the first three exist for us (see header).
        echo "    installing into $STAGE"
        make install DESTDIR="$STAGE" DIRS="$imagedirs src"
    )

    if [ ! -f "$MARKER" ]; then
        echo "tools/build-fltk.sh: build finished but $MARKER is missing" >&2
        exit 1
    fi

    # Match the rest of the staging tree: .la files record an absolute
    # libdir=/usr/lib and make the cross-linker prefer the HOST copy of a
    # library over ours. FLTK does not generate any today; delete defensively so
    # a future libtool-ised release cannot reintroduce the trap silently.
    rm -f "$STAGE"/usr/lib/libfltk*.la 2>/dev/null || true

    echo "==> verifying the staged library is ARM, soft-float, and shared"
    elf_flags="$("$HOST-readelf" -h "$MARKER" | sed -n 's/^ *Flags: *//p')"
    case "$elf_flags" in
        0x5000200*) : ;;
        *)
            echo "tools/build-fltk.sh: unexpected ELF Flags: $elf_flags" >&2
            echo "(want 0x5000200 -- Version5 EABI, soft-float, matching Xfbdev/libX11)" >&2
            exit 1
            ;;
    esac
    soname="$("$HOST-readelf" -d "$MARKER" | sed -n 's/.*SONAME.*\[\(.*\)\]/\1/p')"
    if [ "$soname" != "libfltk.so.$FL_DSO_VERSION" ]; then
        echo "tools/build-fltk.sh: SONAME is '$soname', expected libfltk.so.$FL_DSO_VERSION" >&2
        echo "tools/build-matchbox-payload.sh ships the file under its own name and" >&2
        echo "relies on that being the SONAME; a mismatch would deploy a library the" >&2
        echo "dynamic linker can never find." >&2
        exit 1
    fi
    echo "    Flags: $elf_flags  SONAME: $soname"
fi

# --- the smoke-test app -------------------------------------------------
# Same idea as tools/build-sdl.sh's sdltest: the smallest program that
# proves the freshly built shared library actually loads, connects to
# Xfbdev, and draws. Linked against the STAGED libfltk, not a static one,
# so a broken .so shows up here on the host instead of on the device.
FLTKTEST_SRC="$SRC/fltktest.cxx"
if [ -f "$FLTKTEST_SRC" ]; then
    echo "==> building fltktest against the freshly staged libfltk"
    # LDLIBS is the exact library list configure settled on (Xft, Xrender,
    # fontconfig, X11, pthread, ...). Reusing it beats re-deriving the set
    # by hand and keeps this in step with the --disable-* flags above.
    fltk_ldlibs="$(sed -n 's/^LDLIBS[[:space:]]*=[[:space:]]*//p' "$FLTK_SRC_DIR/makeinclude")"
    mkdir -p "$STAGE/usr/bin"
    # -isystem, not -I: FLTK 1.3's own headers emit hundreds of
    # -Wunused-parameter warnings that would bury any real warning in
    # fltktest.cxx itself.
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
            echo "tools/build-fltk.sh: fltktest does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping fltktest (no $FLTKTEST_SRC)"
fi

# --- matchbox-fbrun -----------------------------------------------------
# A system tool, not part of FLTK -- it hands the whole machine over to a
# framebuffer application and takes it back afterwards (see
# userspace/src/matchbox-fbrun.cxx). It is built here rather than with the
# plain-C utilities in tools/build-userspace.sh for one reason: it draws its
# confirmation dialog with FLTK, so it needs this staging tree and exactly
# these link flags.
#
# tools/build-matchbox-payload.sh ships it to /usr/sbin, in the same payload
# that already carries libfltk and libstdc++.
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

    # Same guard as fltktest: a binary that linked against the wrong FLTK
    # would fail on the device, not here, so check before shipping it.
    needed="$("$HOST-readelf" -d "$STAGE/usr/bin/matchbox-fbrun" | grep -oE '\[lib[^]]+\]' | tr -d '[]' | tr '\n' ' ')"
    echo "    NEEDED: $needed"
    case " $needed " in
        *" libfltk.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/build-fltk.sh: matchbox-fbrun does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping matchbox-fbrun (no $FBRUN_SRC)"
fi

# --- mb-wallpaper-picker -------------------------------------------------
# The desktop's wallpaper setter (see userspace/src/mb-wallpaper-picker.cxx).
# It ships to /usr/local/bin, same as pikostore -- see that binary's own
# comment in tools/build-matchbox-payload.sh for why that is on the device's
# PATH for desktop-launched apps despite matchbox-fbrun needing /usr/sbin.
#
# Unlike fltktest/matchbox-fbrun this one decodes PNG/JPEG/BMP thumbnails,
# so it needs libfltk_images and its own dependencies (-lpng -lz) ahead of
# -lfltk -- see docs/HOWTO-FLTK.md, "Add image loading".
#
# It is now a thin main() over panels/panel-wallpaper.cxx, which is the
# SAME source piko-settings compiles in to show the picker inside its own
# window (docs/HOWTO-SETTINGS-APP.md, "One panel, two ways in"). Both
# binaries therefore carry a copy of the panel's object code, which is the
# deliberate trade: one copy of the SOURCE, and no dlopen, no plugin ABI
# and no second process at runtime. -I"$SRC" is what makes the
# "panels/..." includes resolve.
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
            echo "tools/build-fltk.sh: mb-wallpaper-picker does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac
    case " $needed " in
        *" libfltk_images.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/build-fltk.sh: mb-wallpaper-picker does not NEED libfltk_images.so.$FL_DSO_VERSION" >&2
            echo "-- thumbnails would fail to decode on the device." >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping mb-wallpaper-picker (no $WALLPAPER_PICKER_SRC)"
fi

# --- piko-settings -------------------------------------------------------
# The ROM's settings window (see userspace/src/piko-settings.cxx): every
# .desktop file with Categories=Settings, grouped under its
# X-Piko-Settings-Group heading. Ships to /usr/local/bin, same as pikostore
# and mb-wallpaper-picker.
#
# Needs libfltk_images for the same reason mb-wallpaper-picker does -- it
# draws each entry's Icon=, which is a PNG -- so -lfltk_images -lpng -lz go
# ahead of -lfltk. See docs/HOWTO-FLTK.md, "Add image loading".
#
# $PANEL_SRCS is every embeddable settings panel, the same list the
# standalone binaries above compile in. This is what lets tapping "Set
# Wallpaper" open the picker INSIDE this window rather than launching a
# second program -- one copy of the source, two ways in. A new panel is
# added to PANEL_SRCS, to PANELS[] in piko-settings.cxx, and to its own
# .desktop file as X-Piko-Settings-Panel=<name>; nothing else changes.
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
            echo "tools/build-fltk.sh: piko-settings does not NEED libfltk.so.$FL_DSO_VERSION" >&2
            echo "-- it linked statically or against the wrong library." >&2
            exit 1
            ;;
    esac
    case " $needed " in
        *" libfltk_images.so.$FL_DSO_VERSION "*) : ;;
        *)
            echo "tools/build-fltk.sh: piko-settings does not NEED libfltk_images.so.$FL_DSO_VERSION" >&2
            echo "-- every entry's icon would fail to decode on the device." >&2
            exit 1
            ;;
    esac
else
    echo "==> skipping piko-settings (no $PIKO_SETTINGS_SRC)"
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
echo "        tools/build-matchbox-payload.sh [--deploy]"
