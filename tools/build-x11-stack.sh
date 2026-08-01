#!/bin/sh
set -eu

# Cross-builds the ENTIRE X11/Matchbox desktop stack from the tracked git
# submodules under userspace/src/, from scratch: X.Org (xtrans through
# xserver/xkbcomp/xev) then Matchbox (libmatchbox + the four apps). This is
# the automated version of the "configure+make by hand, per component" step
# docs/HOWTO-MATCHBOX-DESKTOP.md has described since 2026-07-31 -- every
# flag here is taken from that doc and cross-checked against the config.log
# of the real cross-compiled build it documents, not re-derived from
# scratch.
#
# Prerequisites this script does NOT do for you:
#   git submodule update --init --recursive
#   tools/build-uclibc-toolchain.sh       (or set TOOLCHAIN_BIN_DIR/CROSS_HOST)
#   tools/build-thirdparty-deps.sh        zlib expat libpng freetype
#                                         fontconfig xkeyboard-config dejavu
# tools/setup-x11-src.sh (local patches) and tools/build-libiw.sh (libiw,
# needed for matchbox-panel's --enable-proc-apm build below to also pick up
# mb-applet-wireless) ARE run automatically, same as tools/build-and-deploy.sh
# does for tools/setup-kernel-src.sh -- both are cheap and idempotent, so
# there's no reason to make callers remember them.
#
# Host prerequisite this script cannot install for you: xorgproto (Arch) /
# x11proto-dev (Debian/Ubuntu) -- the arch-independent X protocol headers
# (xproto, kbproto, randrproto, ...), expected on the HOST's own
# /usr/share/pkgconfig. Every X.Org submodule here was merged long enough
# ago that upstream no longer vendors these; PKG_CONFIG_LIBDIR below
# deliberately widens to the host's /usr/share/pkgconfig to find them
# (safe: these are headers only, nothing compiled, so there is no
# host/target contamination risk the way there would be for a real lib).
#
# Usage:
#   tools/build-x11-stack.sh [--force] [PKG ...]
#
# With no PKG arguments it builds everything, in the one dependency order
# that matters (do not reorder the default list). Each component is
# idempotent -- skipped if its install marker is already present -- so this
# is cheap to call unconditionally, e.g. from tools/build-and-deploy.sh.
# --force rebuilds everything (re-runs autogen too). Pass one or more PKG
# names to build/rebuild only those (dependencies are NOT built for you in
# that case -- this is for iterating on one component you know is ready).
#
# Env overrides (same names/defaults as tools/build-thirdparty-deps.sh):
#   CROSS_HOST         default arm-unknown-linux-uclibcgnueabi
#   TOOLCHAIN_BIN_DIR  default <repo>/toolchain/x-tools/$CROSS_HOST/bin
#
# Exit codes:
#   0   every requested component built (or already up to date)
#   1   a hard failure -- the failing component's own configure/make output
#       is never swallowed, it goes straight to the terminal

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC="$REPO/userspace/src"
STAGE="$REPO/userspace/stage-target"

HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST/bin}"
BUILD_ARCH="$(uname -m)-pc-linux-gnu"

# Separate DESTDIRs for the four end-user Matchbox apps -- same variable
# names as tools/build-matchbox-payload.sh, which reads these back out.
# They were built in parallel by separate agents the first time around and
# would otherwise race installing into one tree.
D_WM="${D_WM:-/tmp/mbwm-stage}"
D_DESKTOP="${D_DESKTOP:-/tmp/mb-stage-desktop}"
D_PANEL="${D_PANEL:-/tmp/mb-stage-panel}"
D_COMMON="${D_COMMON:-/tmp/mb-stage-common}"

FORCE=0
PKGS=""
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -*) echo "FAILED: unknown option: $arg" >&2; exit 1 ;;
        *) PKGS="$PKGS $arg" ;;
    esac
done
# Dependency order -- see docs/HOWTO-MATCHBOX-DESKTOP.md "Build order" and
# docs/archive/HANDOFF-2026-07-28-X11-XFBDEV.md for why this chain is what
# it is (xcb-proto before libxcb: code generation input, not just a link
# dependency; libXrender/libXft/libmatchbox before the matchbox-* apps).
[ -n "$PKGS" ] || PKGS="xorg-macros xtrans libfontenc libXfont xcb-proto \
libxcb libXau libXdmcp libX11 libXext pixman libxkbfile xserver xkbcomp xev \
libXrender libXft libmatchbox matchbox-window-manager \
matchbox-desktop-classic matchbox-panel matchbox-common"

if [ ! -d "$TOOLCHAIN_BIN_DIR" ]; then
    echo "FAILED: toolchain bin dir not found: $TOOLCHAIN_BIN_DIR" >&2
    echo "Run tools/build-uclibc-toolchain.sh first, or set TOOLCHAIN_BIN_DIR." >&2
    exit 1
fi
if [ ! -f "$STAGE/usr/lib/pkgconfig/freetype2.pc" ]; then
    echo "FAILED: third-party deps not staged (no freetype2.pc in $STAGE)." >&2
    echo "Run tools/build-thirdparty-deps.sh first." >&2
    exit 1
fi

PATH="$TOOLCHAIN_BIN_DIR:$PATH"
export PATH
export CC="${HOST}-gcc"
export CXX="${HOST}-g++"
export AR="${HOST}-ar"
export RANLIB="${HOST}-ranlib"
export STRIP="${HOST}-strip"
export PKG_CONFIG_SYSROOT_DIR="$STAGE"
export PKG_CONFIG_LIBDIR="$STAGE/usr/lib/pkgconfig:$STAGE/usr/share/pkgconfig:/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
export CPPFLAGS="-I$STAGE/usr/include"
# -rpath-link (not -rpath): lets the cross-linker resolve *indirect*
# dependencies at link time (e.g. libfreetype needs libz) without baking
# any path into the binary -- the device still resolves these from /lib at
# runtime. See tools/build-thirdparty-deps.sh for the same note.
export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"

# Applied unconditionally, same reasoning as tools/build-and-deploy.sh
# calling tools/setup-kernel-src.sh unconditionally: idempotent and cheap,
# so there is no reason to make every caller remember a separate step.
echo "==> applying local X11 patches (tools/setup-x11-src.sh)"
if [ "$FORCE" -eq 1 ]; then
    "$REPO/tools/setup-x11-src.sh" --force
else
    "$REPO/tools/setup-x11-src.sh"
fi
echo ""

# libiw, so matchbox-panel's configure below also enables mb-applet-wireless
# (--enable-proc-apm alone only gets the battery applet). Must run before
# matchbox-panel builds; harmless/cheap before anything else too.
echo "==> building libiw (tools/build-libiw.sh, needed for mb-applet-wireless)"
if [ "$FORCE" -eq 1 ]; then
    "$REPO/tools/build-libiw.sh" --force
else
    "$REPO/tools/build-libiw.sh"
fi
echo ""

# extra_configure_args NAME -- flags beyond --host/--build/--prefix, kept
# minimal on purpose (64MB RAM, no accelerator: optional features stay off).
# Taken verbatim from docs/HOWTO-MATCHBOX-DESKTOP.md, cross-checked against
# the real build's config.log.
extra_configure_args() {
    case "$1" in
    libX11)     echo "--disable-static --disable-xcursor --disable-composecache" ;;
    libxcb)     echo "--disable-static --without-doxygen" ;;
    libXfont)   echo "--disable-static --disable-freetype" ;;
    pixman)
        echo "--disable-static --disable-gtk --disable-libpng --disable-openmp \
--disable-arm-simd --disable-arm-neon --disable-arm-a64-neon --disable-arm-iwmmxt"
        ;;
    xserver)
        # kdrive/Xfbdev only -- no GLX/DRI (no GPU), no other server
        # flavours (Xorg/Xnest/Xvfb/Xwin/XQuartz/Xdmx all need things this
        # board doesn't have or want).
        echo "--disable-glx --disable-aiglx --disable-dri --disable-dri2 \
--disable-dmx --disable-xvfb --disable-xnest --disable-xorg --disable-xquartz \
--disable-xwin --enable-kdrive --enable-xfbdev --enable-kdrive-evdev \
--enable-kdrive-kbd --enable-kdrive-mouse"
        ;;
    matchbox-window-manager)
        # NOT --enable-standalone: that mode predates libmatchbox and its
        # own configure warns "does not support theming. It will be ugly."
        # --x-includes/--x-libraries are required because this package
        # finds X via the old AC_PATH_X macro (hardcoded search paths),
        # not pkg-config.
        echo "--x-includes=$STAGE/usr/include --x-libraries=$STAGE/usr/lib \
--disable-composite --disable-startup-notification --disable-gconf --disable-session"
        ;;
    matchbox-desktop-classic)
        # --sysconfdir=/etc, or MBCONFDIR bakes in as /usr/etc/matchbox.
        # USE_XSETTINGS is NOT a configure flag -- it has to be forced at
        # make time, see build_matchbox_desktop_classic() below for why.
        echo "--sysconfdir=/etc --disable-static" ;;
    matchbox-panel)
        # Without this, configure silently drops mb-applet-battery from
        # bin_PROGRAMS (neither upstream backend -- apm.h/-lapm, or
        # /proc/acpi -- exists here) and mb-applet-wireless never gets
        # picked up. modules/x11/matchbox-session lists both; without them
        # tools/build-matchbox-payload.sh's applet-presence check fails.
        # See docs/HOWTO-MATCHBOX-DESKTOP.md "Panel applets".
        echo "--disable-static --enable-proc-apm" ;;
    *) echo "--disable-static" ;;
    esac
}

# needs_host NAME -- xorg-macros and xcb-proto compile nothing (macros /
# XML+pkgconfig data only), so --host is meaningless for them and was not
# used in the real build (confirmed via their config.log).
needs_host() {
    case "$1" in
    xorg-macros|xcb-proto) return 1 ;;
    *) return 0 ;;
    esac
}

# destdir_for NAME -- where `make install` goes. Empty means "don't
# install at all" (xserver/xkbcomp/xev: their binaries are read directly
# out of the build tree by tools/build-matchbox-payload.sh; xorg-macros:
# nothing to install, it only needs its .m4 generated in place).
destdir_for() {
    case "$1" in
    xorg-macros|xserver|xkbcomp|xev) echo "" ;;
    matchbox-window-manager)         echo "$D_WM" ;;
    matchbox-desktop-classic)        echo "$D_DESKTOP" ;;
    matchbox-panel)                  echo "$D_PANEL" ;;
    matchbox-common)                 echo "$D_COMMON" ;;
    *)                               echo "$STAGE" ;;
    esac
}

# marker_for NAME -- a file that only exists once this component is truly
# built+installed. Real per-package pkgconfig/binary paths pulled from the
# actual staged tree, not guessed.
marker_for() {
    case "$1" in
    xorg-macros)              echo "$SRC/xorg-macros/xorg-macros.m4" ;;
    xtrans)                   echo "$STAGE/usr/share/pkgconfig/xtrans.pc" ;;
    libfontenc)                echo "$STAGE/usr/lib/pkgconfig/fontenc.pc" ;;
    libXfont)                 echo "$STAGE/usr/lib/pkgconfig/xfont.pc" ;;
    xcb-proto)                echo "$STAGE/usr/share/pkgconfig/xcb-proto.pc" ;;
    libxcb)                   echo "$STAGE/usr/lib/pkgconfig/xcb.pc" ;;
    libXau)                   echo "$STAGE/usr/lib/pkgconfig/xau.pc" ;;
    libXdmcp)                 echo "$STAGE/usr/lib/pkgconfig/xdmcp.pc" ;;
    libX11)                   echo "$STAGE/usr/lib/pkgconfig/x11.pc" ;;
    libXext)                  echo "$STAGE/usr/lib/pkgconfig/xext.pc" ;;
    pixman)                   echo "$STAGE/usr/lib/pkgconfig/pixman-1.pc" ;;
    libxkbfile)                echo "$STAGE/usr/lib/pkgconfig/xkbfile.pc" ;;
    xserver)                  echo "$SRC/xserver/hw/kdrive/fbdev/Xfbdev" ;;
    xkbcomp)                  echo "$SRC/xkbcomp/xkbcomp" ;;
    xev)                      echo "$SRC/xev/xev" ;;
    libXrender)                echo "$STAGE/usr/lib/pkgconfig/xrender.pc" ;;
    libXft)                   echo "$STAGE/usr/lib/pkgconfig/xft.pc" ;;
    libmatchbox)               echo "$STAGE/usr/lib/pkgconfig/libmb.pc" ;;
    matchbox-window-manager)  echo "$D_WM/usr/bin/matchbox-window-manager" ;;
    matchbox-desktop-classic) echo "$D_DESKTOP/usr/bin/matchbox-desktop" ;;
    matchbox-panel)           echo "$D_PANEL/usr/bin/matchbox-panel" ;;
    matchbox-common)          echo "$D_COMMON/usr/bin/matchbox-session" ;;
    *) echo "FAILED: no marker known for $1" >&2; exit 1 ;;
    esac
}

# submodule_dir_for NAME -- almost always $SRC/$NAME, except where the
# .gitmodules path doesn't match the package name.
submodule_dir_for() {
    case "$1" in
    matchbox-desktop-classic) echo "$SRC/matchbox-desktop-classic" ;;
    *) echo "$SRC/$1" ;;
    esac
}

# configure_args_for NAME -- the FULL configure argument list. Not just
# used for the direct ./configure call: every autogen.sh in this tree
# forwards "$@" to its own internal configure invocation, and the two
# families do it differently --
#
#   xorg-macros/xcb-proto/libfontenc/libxcb/libXau/libXdmcp/xtrans/
#   libXft/libxkbfile/libXfont/pixman: honour NOCONFIGURE, else
#       exec "$srcdir"/configure "$@"
#
#   libX11/libXext/xserver/libXrender/matchbox-*: ALWAYS run
#       $srcdir/configure --enable-maintainer-mode "$@"
#       (there is no way to opt out; NOCONFIGURE is not read at all)
#
# So NOCONFIGURE cannot be relied on as a universal "just generate
# configure, don't run it" switch -- half of these scripts run configure
# themselves unconditionally. The fix that works for BOTH families: pass
# the real cross-compile args straight through autogen.sh every time
# configure needs to be (re)generated, and only invoke ./configure
# directly on the fast, already-generated path. Found 2026-07-31 when a
# forced xev rebuild silently configured native
# ("./configure --enable-maintainer-mode", no --host at all) and failed
# with "cannot run C compiled programs" instead of cross-compiling.
configure_args_for() {
    name="$1"
    if needs_host "$name"; then
        # shellcheck disable=SC2046
        echo "--host=$HOST --build=$BUILD_ARCH --prefix=/usr $(extra_configure_args "$name")"
    else
        echo "--prefix=/usr"
    fi
}

build_one() {
    name="$1"
    dir="$(submodule_dir_for "$name")"
    marker="$(marker_for "$name")"
    ddir="$(destdir_for "$name")"

    if [ ! -d "$dir" ]; then
        echo "FAILED: submodule not checked out: $dir" >&2
        echo "Run: git submodule update --init --recursive" >&2
        exit 1
    fi

    if [ "$FORCE" -eq 0 ] && [ -e "$marker" ]; then
        echo "==> $name: already built, skipping ($marker)"
        return 0
    fi

    echo "==> $name"
    [ "$FORCE" -eq 1 ] && rm -f "$dir/configure"

    ( cd "$dir"
      # $STAGE/usr/share/aclocal matters as much as xorg-macros: xtrans
      # installs xtrans.m4 there, and xserver's configure.ac calls
      # XTRANS_CONNECTION_FLAGS out of it. Leaving it off only bites when
      # autogen actually runs -- with a configure already generated (the
      # usual case) the macro is long since expanded -- so this failed
      # exactly once, on a --force rebuild of xserver in a fresh checkout,
      # with "undefined or overquoted macro: XTRANS_CONNECTION_FLAGS".
      export ACLOCAL_PATH="$SRC/xorg-macros:$STAGE/usr/share/aclocal${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
      # shellcheck disable=SC2046
      if [ -f ./configure ]; then
          echo "    configure already generated, running it directly"
          ./configure $(configure_args_for "$name")
      else
          echo "    generating + running configure (autogen.sh)"
          ./autogen.sh $(configure_args_for "$name")
      fi

      case "$name" in
      xserver)
          # CWARNFLAGS override: this 15-year-old codebase is full of
          # warnings modern GCC treats as errors by xserver's own default
          # -Werror policy. -Wno-error, not -w: still see them, just don't
          # fail the build over them.
          make -j"$(nproc 2>/dev/null || echo 4)" CWARNFLAGS='-Wall -Wno-error'
          ;;
      matchbox-desktop-classic)
          # USE_XSETTINGS cannot be a configure flag: configure decides
          # XSettings support by grepping `pkg-config --libs libmb` for the
          # literal string "xsettings", which libmb.pc never contains --
          # the detection itself is simply wrong, even though libmatchbox
          # compiles xsettings-client.c unconditionally and exports the
          # symbols. Left off, mb->theme_name stays NULL forever and
          # matchbox-desktop can never pick up a theme or font at all. The
          # extra -I.../include/libmb is needed because mbdesktop.h
          # includes <xsettings-client.h> unqualified while libmb installs
          # it under include/libmb/.
          make -j"$(nproc 2>/dev/null || echo 4)" \
              CPPFLAGS="-I$STAGE/usr/include -I$STAGE/usr/include/libmb -DUSE_XSETTINGS"
          ;;
      *)
          make -j"$(nproc 2>/dev/null || echo 4)"
          ;;
      esac

      if [ -n "$ddir" ]; then
          make install DESTDIR="$ddir"
      fi
    )

    # libtool .la files record an absolute libdir=/usr/lib and make the
    # cross-linker pick the HOST copy of a library over ours -- same
    # reasoning as tools/build-thirdparty-deps.sh.
    [ -n "$ddir" ] && rm -f "$ddir"/usr/lib/*.la 2>/dev/null || true

    if [ ! -e "$marker" ]; then
        echo "FAILED: $name built but marker is missing: $marker" >&2
        exit 1
    fi
    echo "    built: $name"
}

for p in $PKGS; do
    build_one "$p"
done

echo ""
echo "==> X11/Matchbox stack ready."
echo "    Libraries + xkbcomp/xev/Xfbdev:  $STAGE, and in-tree under userspace/src/"
echo "    Matchbox apps:                  $D_WM $D_DESKTOP $D_PANEL $D_COMMON"
echo "    Package into a payload with:    tools/build-matchbox-payload.sh"
