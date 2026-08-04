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
#   tools/build-x11-stack.sh [--force] [--skip-st] [PKG ...]
#
# With no PKG arguments it builds everything, in the one dependency order
# that matters (do not reorder the default list). Each component is
# idempotent -- skipped if its install marker is present AND was built from
# the sources currently checked out (see stamp_for/source_state) -- so this
# is cheap to call unconditionally, e.g. from tools/build-and-deploy.sh,
# without that cheapness costing you a stale component.
# --force rebuilds everything (re-runs autogen too). Pass one or more PKG
# names to build/rebuild only those (dependencies are NOT built for you in
# that case -- this is for iterating on one component you know is ready).
#
# WHAT "EVERYTHING" MEANS, and why it grew: the job of this script is to
# leave behind exactly what tools/build-matchbox-payload.sh needs, because
# that is the only thing anyone builds this stack FOR. Every caller of the
# two together -- tools/build-and-deploy.sh, flash/build-mtd3-jffs2.sh,
# flash/build-update-package.sh -- was independently responsible for
# remembering the leftovers, and each of them forgot a different one, so
# the payload step failed on all three (2026-08-01; see
# docs/HOWTO-MATCHBOX-DESKTOP.md "Build order"). The leftovers now live
# here:
#
#   mb-applet-card  a full package below, like the other Matchbox apps.
#                   It is a separate repo (its own plain Makefile, no
#                   autotools -- see build_one) and was simply never in
#                   this list, so $D_CARD never existed and the payload
#                   died on "missing component DESTDIR" every single run.
#   mb-volume       same story, same shape -- own repo, own plain
#                   Makefile, into $D_VOLUME. The one difference: it also
#                   links libasound, statically, out of userspace/stage-alsa
#                   -- a SEPARATE staging tree this script does not
#                   populate (tools/build-alsa.sh does, for the
#                   MPlayer/zplay audio stack). build_one's mb-volume case
#                   checks for it explicitly rather than failing on a
#                   confusing link error.
#   st              tools/build-st.sh, at the end -- it is an X11 client
#                   that links libX11/libXft out of the stage this script
#                   populates, and the payload ships it.
#   FLTK            tools/build-fltk.sh, at the end -- libfltk*.so.1.3
#                   plus fltktest, matchbox-fbrun and mb-wallpaper-picker,
#                   likewise X11 clients of this stage, likewise shipped.
#
# st and FLTK are also built by tools/build-userspace.sh, which skips both
# when the stage is not populated yet. That skip is what made the ordering
# a trap: build-userspace.sh runs BEFORE this script on a fresh machine, so
# it skipped them, and nothing ever came back for them. Both scripts are
# idempotent, so whichever runs second is a no-op -- there is no double
# build, only a guarantee that they happen at all.
#
# --skip-st leaves st out (and only st). It exists for the same reason
# tools/build-userspace.sh's --skip-st does: st is the one component whose
# source is a submodule of a non-GitHub upstream, and when that is
# unavailable userspace/src/st is an empty directory that fails the build
# at the very last step. Pass it to tools/build-matchbox-payload.sh too, or
# the payload will still demand the binary.
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

# Separate DESTDIRs for the end-user Matchbox apps -- same variable names
# as tools/build-matchbox-payload.sh, which reads these back out.
# They were built in parallel by separate agents the first time around and
# would otherwise race installing into one tree.
D_WM="${D_WM:-/tmp/mbwm-stage}"
D_DESKTOP="${D_DESKTOP:-/tmp/mb-stage-desktop}"
D_PANEL="${D_PANEL:-/tmp/mb-stage-panel}"
D_COMMON="${D_COMMON:-/tmp/mb-stage-common}"
D_CARD="${D_CARD:-/tmp/mb-stage-card}"
D_VOLUME="${D_VOLUME:-/tmp/mb-stage-volume}"
D_BRIGHT="${D_BRIGHT:-/tmp/mb-stage-brightness}"
D_PIKAFFEINE="${D_PIKAFFEINE:-/tmp/mb-stage-pikaffeine}"

FORCE=0
SKIP_ST=0
PKGS=""
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        --skip-st) SKIP_ST=1 ;;
        -*) echo "FAILED: unknown option: $arg" >&2; exit 1 ;;
        *) PKGS="$PKGS $arg" ;;
    esac
done
# Whether this is a full build or an explicit "just these packages" run.
# st/FLTK at the end are part of "everything" and must NOT run when the
# caller asked for one component it is iterating on.
FULL_BUILD=0
[ -n "$PKGS" ] || FULL_BUILD=1
# Dependency order -- see docs/HOWTO-MATCHBOX-DESKTOP.md "Build order" and
# docs/archive/HANDOFF-2026-07-28-X11-XFBDEV.md for why this chain is what
# it is (xcb-proto before libxcb: code generation input, not just a link
# dependency; libXrender/libXft/libmatchbox before the matchbox-* apps).
# libXau/libXdmcp before libxcb for the same "not just a link dependency"
# reason as xcb-proto: libxcb's own configure.ac PKG_CHECK_MODULES for
# xau/xdmcp fails outright, before anything compiles, if their .pc files
# are not staged yet -- this previously listed libxcb first and only
# never failed because whoever wrote it had xau/xdmcp available some
# other way.
[ -n "$PKGS" ] || PKGS="xorg-macros xtrans libfontenc libXfont xcb-proto \
libXau libXdmcp libxcb libX11 libXext libXpm pixman libxkbfile xserver xkbcomp xev \
libXrender libXft libmatchbox matchbox-window-manager \
matchbox-desktop-classic matchbox-panel matchbox-common mb-applet-card mb-volume \
mb-brightness mb-applet-pikaffeine"

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

# xorgproto (X11/Xproto.h, X11/Xfuncproto.h, ...): pure protocol headers,
# no compiled code, so copying them into the staging tree carries none of
# the host/target contamination risk a real library would. The comment at
# the top of this file describes the OTHER way this is meant to work --
# PKG_CONFIG_LIBDIR widened to the host's /usr/share/pkgconfig, so
# xproto.pc's own Cflags supplies -I -- and that is enough on distros
# whose xproto.pc actually emits one (Debian/Ubuntu). Arch's does not:
# Cflags is empty there because /usr/include is already a default search
# path for a NATIVE compiler, which the ARM cross-gcc does not share, so
# libXfont's very first compile failed with "X11/Xfuncproto.h: No such
# file or directory" despite pkg-config finding xproto.pc successfully.
# Copying only the files `pacman -Ql xorgproto` actually owns (not a
# blanket copy of /usr/include/X11, which also holds unrelated packages'
# headers -- Xft, Xcursor, Xaw, ... -- that would shadow this project's
# own staged versions of the same) fixes it on both distros without an
# extra -I flag that would risk exposing the rest of /usr/include (glibc
# headers use x86_64-specific builtins like _Float128 that do not exist
# for this ARM/uclibc target).
echo "==> staging xorgproto headers (host package, headers only)"
if command -v pacman >/dev/null 2>&1 && pacman -Qq xorgproto >/dev/null 2>&1; then
    pacman -Ql xorgproto | awk '{print $2}' | grep '^/usr/include/.*[^/]$' | while read -r f; do
        rel="${f#/usr/include/}"
        mkdir -p "$STAGE/usr/include/$(dirname "$rel")"
        cp -n "$f" "$STAGE/usr/include/$rel" 2>/dev/null || true
    done
    echo "    staged: $(pacman -Ql xorgproto | awk '{print $2}' | grep -c '^/usr/include/.*[^/]$') headers from the xorgproto package"
elif command -v dpkg-query >/dev/null 2>&1 && dpkg-query -s x11proto-dev >/dev/null 2>&1; then
    dpkg-query -L x11proto-dev | grep '^/usr/include/.*[^/]$' | while read -r f; do
        rel="${f#/usr/include/}"
        mkdir -p "$STAGE/usr/include/$(dirname "$rel")"
        cp -n "$f" "$STAGE/usr/include/$rel" 2>/dev/null || true
    done
    echo "    staged: $(dpkg-query -L x11proto-dev | grep -c '^/usr/include/.*[^/]$') headers from the x11proto-dev package"
elif [ -f "$STAGE/usr/include/X11/Xfuncproto.h" ]; then
    echo "    already staged"
else
    echo "FAILED: xorgproto headers not staged and neither pacman nor dpkg found it." >&2
    echo "Install xorgproto (Arch) / x11proto-dev (Debian/Ubuntu) or copy" >&2
    echo "its /usr/include/X11 files into $STAGE/usr/include/X11 by hand." >&2
    exit 1
fi
echo ""

# xsha1-compat, so xserver's configure below finds a SHA1 implementation.
# See tools/build-xsha1-compat.sh: nothing in this cross toolchain
# provides -lmd (or libc/libgcrypt/openssl SHA1), and xserver's configure
# fails outright -- "No suitable SHA1 implementation found" -- without
# one. Must run before xserver configures.
echo "==> building xsha1-compat (tools/build-xsha1-compat.sh, needed for xserver's SHA1 check)"
if [ "$FORCE" -eq 1 ]; then
    "$REPO/tools/build-xsha1-compat.sh" --force
else
    "$REPO/tools/build-xsha1-compat.sh"
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
    libXpm)
        # Needed only to decode the toasters screensaver's XPM sprite
        # sheets (userspace/src/toasters.c). --with-localedir=no drops the
        # gettext/libintl lookup, which uClibc has no business satisfying
        # for a library whose only messages are decoder errors nothing
        # displays; the two z-file options drop popen()-ing gzip/compress
        # to read compressed .xpm files off disk, which this never does --
        # its data is compiled in.
        echo "--disable-static --disable-open-zfile --disable-stat-zfile \
--with-localedir=no"
        ;;
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
    mb-applet-card)                  echo "$D_CARD" ;;
    mb-volume)                       echo "$D_VOLUME" ;;
    mb-brightness)                   echo "$D_BRIGHT" ;;
    mb-applet-pikaffeine)            echo "$D_PIKAFFEINE" ;;
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
    libXpm)                   echo "$STAGE/usr/lib/pkgconfig/xpm.pc" ;;
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
    mb-applet-card)           echo "$D_CARD/usr/bin/mb-applet-card" ;;
    mb-volume)                echo "$D_VOLUME/usr/bin/mb-volume" ;;
    mb-brightness)            echo "$D_BRIGHT/usr/bin/mb-brightness" ;;
    mb-applet-pikaffeine)     echo "$D_PIKAFFEINE/usr/bin/mb-applet-pikaffeine" ;;
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

# uses_autotools NAME -- false only for mb-applet-card, mb-volume,
# mb-brightness and mb-applet-pikaffeine, all one-or-two-source-file
# packages against a couple of pkg-config modules that deliberately ship a
# plain Makefile instead of autotools (their own Makefiles say why).
# Neither has a configure or an autogen.sh, so the generate-and-run-configure
# step below would fail on them with a bare "no such file or directory".
uses_autotools() {
    case "$1" in
    mb-applet-card|mb-volume|mb-brightness|mb-applet-pikaffeine) return 1 ;;
    *) return 0 ;;
    esac
}

# stamp_for NAME -- where the source state that produced this component's
# marker is recorded. Kept beside the staging tree, NOT beside the marker:
# a marker in a DESTDIR gets copied wholesale into the payload, and a
# build-system dotfile has no business landing in /usr/bin on the device.
# $STAGE persists in-repo, so a stamp outlives the /tmp DESTDIRs -- which
# is right, because a vanished DESTDIR already forces a rebuild by itself.
stamp_for() {
    echo "$STAGE/.piko-build-stamps/$1"
}

# source_state DIR -- what the sources currently ARE, as a string to
# compare against the recorded stamp.
#
# HEAD plus a hash of the tracked diff, and deliberately NOT the untracked
# file list: every one of these packages builds in-tree and leaves dozens
# of untracked, un-gitignored artefacts behind (65 in matchbox-panel, 83 in
# matchbox-window-manager), so folding those in would change the state on
# every build and rebuild the world forever. The tracked diff is clean
# after a build in every submodule here, which is what makes it usable.
#
# The cost of that choice: a brand-new UNTRACKED source file does not
# trigger a rebuild. --force covers it, and it is the rarer case by far
# than "the submodule moved to a new commit", which is exactly what this
# is here to catch.
source_state() {
    dir="$1"
    if [ -e "$dir/.git" ]; then
        head="$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo unknown)"
        # Content, not filenames: `git status --porcelain` would call two
        # different edits of one file identical.
        diff="$(git -C "$dir" diff HEAD 2>/dev/null | md5sum | cut -d' ' -f1)"
        echo "$head $diff"
    else
        echo "not-a-git-checkout"
    fi
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

# maintainer_mode_args NAME -- --enable-maintainer-mode, but only for the
# packages whose own autogen.sh passes it.
#
# The fast path below runs ./configure directly when one has already been
# generated, and that quietly produced a DIFFERENTLY configured tree than
# the from-scratch path for the second family above: autogen.sh hardcodes
# --enable-maintainer-mode, ./configure on its own does not, and
# maintainer mode is what installs the `Makefile.in: Makefile.am` rebuild
# rules. Without them automake never re-runs, so a component that gains a
# source file keeps building from the old file list -- silently, with a
# link error naming a function nobody can find any reference to.
#
# That is not hypothetical: matchbox-desktop-classic gained
# mbdesktop_watch.c in Makefile.am, its generated Makefile still says
# MAINT = # (maintainer mode off) and lists no such object, and it now
# fails to link with "undefined reference to mbdesktop_watch_init".
#
# Detected by reading each package's autogen.sh rather than hardcoding the
# list in the comment above, so a submodule that changes its mind about
# this cannot leave the two paths disagreeing again.
maintainer_mode_args() {
    d="$(submodule_dir_for "$1")"
    if [ -f "$d/autogen.sh" ] && grep -q -- '--enable-maintainer-mode' "$d/autogen.sh"; then
        echo "--enable-maintainer-mode"
    fi
}

# drop_host_libdir_from_libtool -- stop `make install` from putting the
# HOST's /usr/lib on the cross-linker's search path. Run in the package's
# build dir, just before install.
#
# At install time libtool RELINKS every library that was built against
# another not-yet-installed libtool library, so the installed copy names
# the final -L path rather than the build tree's. Building that relink
# command it emits, for each such dependency:
#
#     -L$libdir  -L$inst_prefix_dir$libdir   (ltmain "we'll fake it" branch)
#
# $libdir here is where the library will live ON THE DEVICE -- /usr/lib --
# and $inst_prefix_dir is our DESTDIR, so the second -L is the one that
# means anything to us and the first is pure poison: it points the ARM
# linker at the build host's own /usr/lib. libtool computes it from the
# .la's absolute libdir, so neither LDFLAGS ordering nor deleting .la files
# after install (which we already do, below) can head it off -- by then the
# damage is done.
#
# It went unnoticed for as long as it did because ld merely WARNS on an
# incompatible .so ("skipping incompatible /usr/lib/libc.so") and moves on
# to the right one. A host glibc that also ships /usr/lib/libc.a turns the
# same search into a hard error -- "file format not recognized" -- and the
# whole stack dies at libxcb, the first package here with sub-libraries
# (libxcb-composite and 20 more) that link against a sibling .la.
#
# Emptying the bare `-L$libdir` is the whole fix: $inst_prefix_dir$libdir
# still follows it, so the relink resolves against the staging tree, and
# the toolchain's own sysroot supplies -lc as it should. Nothing about the
# device's /usr/lib is knowable at cross-link time anyway.
drop_host_libdir_from_libtool() {
    [ -f ./libtool ] || return 0
    sed -i 's|^\([[:space:]]*\)add_dir=-L\$lt_sysroot\$libdir$|\1add_dir=|' ./libtool
}

build_one() {
    name="$1"
    dir="$(submodule_dir_for "$name")"
    marker="$(marker_for "$name")"
    ddir="$(destdir_for "$name")"

    # An EMPTY directory counts as "not checked out", not as checked out.
    # A submodule whose worktree has been emptied still has its .git file
    # (and `git submodule update --init` reports success on it, because the
    # gitlink is already at the right commit), so `[ -d ]` alone says yes
    # and the build then dies much later with something unrelated-looking.
    # Ignore .git itself when deciding, since that is exactly what is left.
    if [ ! -d "$dir" ] || [ -z "$(ls -A "$dir" 2>/dev/null | grep -v '^\.git$')" ]; then
        echo "FAILED: submodule not checked out: $dir" >&2
        echo "Run: git submodule update --init --recursive" >&2
        echo "(if that reports nothing to do, the worktree was emptied --" >&2
        echo " restore it with: git -C $dir checkout HEAD -- .)" >&2
        echo " HEAD is needed there: a plain 'checkout -- .' fails when the" >&2
        echo " deletions are already staged, which is how this usually looks." >&2
        exit 1
    fi

    # "The marker exists" answers "was this ever built", not "is what was
    # built still what the sources say" -- and the second question is the
    # one that matters, because the staged output is what gets packaged and
    # deployed. Until 2026-08-01 only the first was asked, so a submodule
    # bumped to a new commit was silently repackaged from the old staged
    # copy for as long as its DESTDIR survived: updated applets went out as
    # their old selves, with nothing anywhere reporting a problem.
    stamp="$(stamp_for "$name")"
    state="$(source_state "$dir")"
    if [ "$FORCE" -eq 0 ] && [ -e "$marker" ]; then
        if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$state" ]; then
            echo "==> $name: already built and current, skipping ($marker)"
            return 0
        fi
        if [ -f "$stamp" ]; then
            echo "==> $name: sources changed since the staged copy -- rebuilding"
        else
            # One-time, on the first run after this check was added: there
            # is no record of what the existing staged copy was built from,
            # and assuming it is current is the exact bug above.
            echo "==> $name: staged but unstamped -- rebuilding once to record one"
        fi
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
      # A package's own m4/ (xserver and libfontenc both carry
      # m4/fontutil-compat.m4, the local stand-in for the real font-util
      # package -- see setup-x11-src.sh) is not on aclocal's default
      # search path unless configure.ac declares AC_CONFIG_MACRO_DIR,
      # which neither does. Without it here, autogen only succeeds on a
      # host that happens to have font-util installed system-wide,
      # exactly what the compat macro exists to make unnecessary.
      [ -d "$dir/m4" ] && ACLOCAL_PATH="$dir/m4${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
      export ACLOCAL_PATH="$SRC/xorg-macros:$STAGE/usr/share/aclocal${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
      # shellcheck disable=SC2046
      if ! uses_autotools "$name"; then
          echo "    plain make, nothing to configure"
      elif [ -f ./configure ]; then
          echo "    configure already generated, running it directly"
          ./configure $(maintainer_mode_args "$name") $(configure_args_for "$name")
      else
          echo "    generating + running configure (autogen.sh)"
          ./autogen.sh $(configure_args_for "$name")
      fi

      case "$name" in
      mb-applet-card)
          # Everything its Makefile reads -- CC, CPPFLAGS, LDFLAGS and the
          # PKG_CONFIG_* trio it resolves libmb through -- is already
          # exported above, and `CC ?= cc` in that Makefile yields to the
          # environment. CC is passed explicitly anyway so a reader does
          # not have to know that rule to see this is a cross build.
          #
          # --force needs an explicit clean here: for every other package
          # the forced rebuild is driven by deleting ./configure, and this
          # one has none, so an up-to-date .o would otherwise be reused.
          [ "$FORCE" -eq 1 ] && make clean >/dev/null 2>&1
          make -j"$(nproc 2>/dev/null || echo 4)" CC="$CC"
          ;;
      mb-volume)
          # Same shape as mb-applet-card, plus one extra dependency: it
          # links libasound out of userspace/stage-alsa, a staging tree
          # THIS script does not populate -- tools/build-alsa.sh does, for
          # the MPlayer/zplay audio stack (see the header comment). Check
          # for it explicitly rather than let the link fail on a bare
          # "-lasound: No such file or directory" with no hint why.
          alsa_stage="$REPO/userspace/stage-alsa"
          if [ ! -f "$alsa_stage/usr/lib/libasound.a" ]; then
              echo "FAILED: alsa-lib not staged at $alsa_stage (no libasound.a)." >&2
              echo "Run tools/build-alsa.sh first." >&2
              exit 1
          fi
          [ "$FORCE" -eq 1 ] && make clean >/dev/null 2>&1
          make -j"$(nproc 2>/dev/null || echo 4)" CC="$CC" ALSA_STAGE="$alsa_stage"
          ;;
      mb-brightness)
          # Same plain-Makefile shape as mb-applet-card, and simpler than
          # mb-volume above: no ALSA, no second staging tree. libmb is the
          # only dependency -- this applet only READS /sys/class/backlight
          # and brightd owns everything else about the backlight.
          #
          # Same --force clean reasoning as mb-applet-card: no ./configure
          # to delete, so an up-to-date .o would otherwise be reused.
          [ "$FORCE" -eq 1 ] && make clean >/dev/null 2>&1
          make -j"$(nproc 2>/dev/null || echo 4)" CC="$CC"
          ;;
      mb-applet-pikaffeine)
          # Same plain-Makefile shape as mb-applet-card/mb-brightness:
          # libmb is the only dependency, and it never touches the
          # backlight itself -- it only pokes brightd's own
          # /tmp/brightd.inhibit file. See that repo's README.
          [ "$FORCE" -eq 1 ] && make clean >/dev/null 2>&1
          make -j"$(nproc 2>/dev/null || echo 4)" CC="$CC"
          ;;
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
          # Immediately before install, NOT right after configure: `make`
          # lets config.status regenerate ./libtool (it is a config file
          # like any other), which would quietly undo the patch somewhere
          # between the two.
          drop_host_libdir_from_libtool
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
    # Only now, after the marker is confirmed: a stamp written for a build
    # that did not finish would make the next run skip a component that
    # never got staged.
    mkdir -p "$(dirname "$stamp")"
    printf '%s\n' "$state" > "$stamp"
    echo "    built: $name"
}

for p in $PKGS; do
    build_one "$p"
done

# --- X11 clients that are not X.Org/Matchbox packages ---------------------
# st and FLTK link against the stage the loop above just populated, and
# tools/build-matchbox-payload.sh ships everything they produce: st, the
# libfltk*.so trio, fltktest, matchbox-fbrun and mb-wallpaper-picker. See
# this script's header for why they live here rather than being every
# caller's problem.
#
# Both are idempotent and exit 0 when already current, so this costs a pair
# of marker checks on every subsequent run. Skipped entirely for an explicit
# "build just these packages" invocation: that mode is for iterating on one
# component and has no business rebuilding the world.
if [ "$FULL_BUILD" -eq 1 ]; then
    echo ""
    if [ "$SKIP_ST" -eq 1 ]; then
        echo "==> --skip-st: not building st"
    else
        echo "==> building st (tools/build-st.sh)"
        FORCE_ARG=""
        [ "$FORCE" -eq 1 ] && FORCE_ARG="--force"
        # Unquoted on purpose: an empty "" would be passed through as a
        # literal argument, and build-st.sh only accepts --force.
        # shellcheck disable=SC2086
        sh "$REPO/tools/build-st.sh" $FORCE_ARG
    fi

    echo ""
    echo "==> building FLTK + fltktest + matchbox-fbrun + mb-wallpaper-picker (tools/build-fltk.sh)"
    FORCE_ARG=""
    [ "$FORCE" -eq 1 ] && FORCE_ARG="--force"
    # shellcheck disable=SC2086
    sh "$REPO/tools/build-fltk.sh" $FORCE_ARG

    # The flying-toasters screensaver brightd launches on its idle timer.
    # Same shape as st: one Xlib client against this stage, shipped in the
    # payload, so it belongs to the same "build what the payload needs"
    # rule rather than being each caller's job to remember.
    echo ""
    echo "==> building the toasters screensaver (tools/build-toasters.sh)"
    FORCE_ARG=""
    [ "$FORCE" -eq 1 ] && FORCE_ARG="--force"
    # shellcheck disable=SC2086
    sh "$REPO/tools/build-toasters.sh" $FORCE_ARG
fi

echo ""
echo "==> X11/Matchbox stack ready."
echo "    Libraries + xkbcomp/xev/Xfbdev:  $STAGE, and in-tree under userspace/src/"
echo "    Matchbox apps:                  $D_WM $D_DESKTOP $D_PANEL $D_COMMON $D_CARD $D_VOLUME $D_PIKAFFEINE"
echo "    Package into a payload with:    tools/build-matchbox-payload.sh"
