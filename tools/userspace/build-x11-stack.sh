#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC="$REPO/userspace/src"
STAGE="$REPO/userspace/stage-target"

HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST/bin}"
BUILD_ARCH="$(uname -m)-pc-linux-gnu"

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
FULL_BUILD=0
[ -n "$PKGS" ] || FULL_BUILD=1
[ -n "$PKGS" ] || PKGS="xorg-macros xtrans libfontenc libXfont xcb-proto \
libXau libXdmcp libxcb libX11 libXext libXpm pixman libxkbfile xserver xkbcomp xev \
libXrender libXft libmatchbox matchbox-window-manager \
matchbox-desktop-classic matchbox-panel matchbox-common mb-applet-card mb-volume \
mb-brightness mb-applet-pikaffeine"

if [ ! -d "$TOOLCHAIN_BIN_DIR" ]; then
    echo "FAILED: toolchain bin dir not found: $TOOLCHAIN_BIN_DIR" >&2
    echo "Run tools/toolchain/build-uclibc-toolchain.sh first, or set TOOLCHAIN_BIN_DIR." >&2
    exit 1
fi
if [ ! -f "$STAGE/usr/lib/pkgconfig/freetype2.pc" ]; then
    echo "FAILED: third-party deps not staged (no freetype2.pc in $STAGE)." >&2
    echo "Run tools/userspace/build-thirdparty-deps.sh first." >&2
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
export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"
export CFLAGS="${CFLAGS:--O2} -mcpu=xscale"
export CXXFLAGS="${CXXFLAGS:--O2} -mcpu=xscale"

echo "==> applying local X11 patches (tools/userspace/setup-x11-src.sh)"
if [ "$FORCE" -eq 1 ]; then
    "$REPO/tools/userspace/setup-x11-src.sh" --force
else
    "$REPO/tools/userspace/setup-x11-src.sh"
fi
echo ""

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

echo "==> building xsha1-compat (tools/userspace/build-xsha1-compat.sh, needed for xserver's SHA1 check)"
if [ "$FORCE" -eq 1 ]; then
    "$REPO/tools/userspace/build-xsha1-compat.sh" --force
else
    "$REPO/tools/userspace/build-xsha1-compat.sh"
fi
echo ""

echo "==> building libiw (tools/userspace/build-libiw.sh, needed for mb-applet-wireless)"
if [ "$FORCE" -eq 1 ]; then
    "$REPO/tools/userspace/build-libiw.sh" --force
else
    "$REPO/tools/userspace/build-libiw.sh"
fi
echo ""

extra_configure_args() {
    case "$1" in
    libX11)     echo "--disable-static --disable-xcursor --disable-composecache" ;;
    libxcb)     echo "--disable-static --without-doxygen" ;;
    libXfont)   echo "--disable-static --disable-freetype" ;;
    libXpm)
        echo "--disable-static --disable-open-zfile --disable-stat-zfile \
--with-localedir=no"
        ;;
    pixman)
        echo "--disable-static --disable-gtk --disable-libpng --disable-openmp \
--disable-arm-simd --disable-arm-neon --disable-arm-a64-neon --disable-arm-iwmmxt"
        ;;
    xserver)
        echo "--disable-glx --disable-aiglx --disable-dri --disable-dri2 \
--disable-dmx --disable-xvfb --disable-xnest --disable-xorg --disable-xquartz \
--disable-xwin --enable-kdrive --enable-xfbdev --enable-kdrive-evdev \
--enable-kdrive-kbd --enable-kdrive-mouse \
--with-default-xkb-rules=evdev --with-default-xkb-layout=zaurus"
        ;;
    matchbox-window-manager)
        echo "--x-includes=$STAGE/usr/include --x-libraries=$STAGE/usr/lib \
--disable-composite --disable-startup-notification --disable-gconf --disable-session"
        ;;
    matchbox-desktop-classic)
        echo "--sysconfdir=/etc --disable-static" ;;
    matchbox-panel)
        echo "--disable-static --enable-proc-apm" ;;
    *) echo "--disable-static" ;;
    esac
}

needs_host() {
    case "$1" in
    xorg-macros|xcb-proto) return 1 ;;
    *) return 0 ;;
    esac
}

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

submodule_dir_for() {
    case "$1" in
    matchbox-desktop-classic) echo "$SRC/matchbox-desktop-classic" ;;
    *) echo "$SRC/$1" ;;
    esac
}

uses_autotools() {
    case "$1" in
    mb-applet-card|mb-volume|mb-brightness|mb-applet-pikaffeine) return 1 ;;
    *) return 0 ;;
    esac
}

stamp_for() {
    echo "$STAGE/.piko-build-stamps/$1"
}

source_state() {
    dir="$1"
    if [ -e "$dir/.git" ]; then
        head="$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo unknown)"
        diff="$(git -C "$dir" diff HEAD 2>/dev/null | md5sum | cut -d' ' -f1)"
        echo "$head $diff"
    else
        echo "not-a-git-checkout"
    fi
}

configure_args_for() {
    name="$1"
    if needs_host "$name"; then
        echo "--host=$HOST --build=$BUILD_ARCH --prefix=/usr $(extra_configure_args "$name")"
    else
        echo "--prefix=/usr"
    fi
}

maintainer_mode_args() {
    d="$(submodule_dir_for "$1")"
    if [ -f "$d/autogen.sh" ] && grep -q -- '--enable-maintainer-mode' "$d/autogen.sh"; then
        echo "--enable-maintainer-mode"
    fi
}

drop_host_libdir_from_libtool() {
    [ -f ./libtool ] || return 0
    sed -i 's|^\([[:space:]]*\)add_dir=-L\$lt_sysroot\$libdir$|\1add_dir=|' ./libtool
}

build_one() {
    name="$1"
    dir="$(submodule_dir_for "$name")"
    marker="$(marker_for "$name")"
    ddir="$(destdir_for "$name")"

    if [ ! -d "$dir" ] || [ -z "$(ls -A "$dir" 2>/dev/null | grep -v '^\.git$')" ]; then
        echo "FAILED: submodule not checked out: $dir" >&2
        echo "Run: git submodule update --init --recursive" >&2
        echo "(if that reports nothing to do, the worktree was emptied --" >&2
        echo " restore it with: git -C $dir checkout HEAD -- .)" >&2
        echo " HEAD is needed there: a plain 'checkout -- .' fails when the" >&2
        echo " deletions are already staged, which is how this usually looks." >&2
        exit 1
    fi

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
            echo "==> $name: staged but unstamped -- rebuilding once to record one"
        fi
    fi

    echo "==> $name"
    [ "$FORCE" -eq 1 ] && rm -f "$dir/configure"

    ( cd "$dir"
      [ -d "$dir/m4" ] && ACLOCAL_PATH="$dir/m4${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
      export ACLOCAL_PATH="$SRC/xorg-macros:$STAGE/usr/share/aclocal${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
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
          [ "$FORCE" -eq 1 ] && make clean >/dev/null 2>&1
          make -j"$(nproc 2>/dev/null || echo 4)" CC="$CC"
          ;;
      mb-volume)
          alsa_stage="$REPO/userspace/stage-alsa"
          if [ ! -f "$alsa_stage/usr/lib/libasound.a" ]; then
              echo "FAILED: alsa-lib not staged at $alsa_stage (no libasound.a)." >&2
              echo "Run tools/userspace/build-alsa.sh first." >&2
              exit 1
          fi
          [ "$FORCE" -eq 1 ] && make clean >/dev/null 2>&1
          make -j"$(nproc 2>/dev/null || echo 4)" CC="$CC" ALSA_STAGE="$alsa_stage"
          ;;
      mb-brightness)
          [ "$FORCE" -eq 1 ] && make clean >/dev/null 2>&1
          make -j"$(nproc 2>/dev/null || echo 4)" CC="$CC"
          ;;
      mb-applet-pikaffeine)
          [ "$FORCE" -eq 1 ] && make clean >/dev/null 2>&1
          make -j"$(nproc 2>/dev/null || echo 4)" CC="$CC"
          ;;
      xserver)
          make -j"$(nproc 2>/dev/null || echo 4)" CWARNFLAGS='-Wall -Wno-error'
          ;;
      matchbox-desktop-classic)
          make -j"$(nproc 2>/dev/null || echo 4)" \
              CPPFLAGS="-I$STAGE/usr/include -I$STAGE/usr/include/libmb -DUSE_XSETTINGS"
          ;;
      *)
          make -j"$(nproc 2>/dev/null || echo 4)"
          ;;
      esac

      if [ -n "$ddir" ]; then
          drop_host_libdir_from_libtool
          make install DESTDIR="$ddir"
      fi
    )

    [ -n "$ddir" ] && rm -f "$ddir"/usr/lib/*.la 2>/dev/null || true

    if [ ! -e "$marker" ]; then
        echo "FAILED: $name built but marker is missing: $marker" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$stamp")"
    printf '%s\n' "$state" > "$stamp"
    echo "    built: $name"
}

for p in $PKGS; do
    build_one "$p"
done

if [ "$FULL_BUILD" -eq 1 ]; then
    echo ""
    if [ "$SKIP_ST" -eq 1 ]; then
        echo "==> --skip-st: not building st"
    else
        echo "==> building st (tools/userspace/build-st.sh)"
        FORCE_ARG=""
        [ "$FORCE" -eq 1 ] && FORCE_ARG="--force"
        sh "$REPO/tools/userspace/build-st.sh" $FORCE_ARG
    fi

    echo ""
    echo "==> building FLTK + fltktest + matchbox-apprun + mb-wallpaper-picker (tools/userspace/build-fltk.sh)"
    FORCE_ARG=""
    [ "$FORCE" -eq 1 ] && FORCE_ARG="--force"
    sh "$REPO/tools/userspace/build-fltk.sh" $FORCE_ARG

    echo ""
    echo "==> building found-file-browser (tools/userspace/build-found-file-browser.sh)"
    sh "$REPO/tools/userspace/build-found-file-browser.sh"

    echo ""
    echo "==> building pikostore (tools/userspace/build-pikostore.sh)"
    sh "$REPO/tools/userspace/build-pikostore.sh"

    echo ""
    echo "==> building the toasters screensaver (tools/userspace/build-toasters.sh)"
    FORCE_ARG=""
    [ "$FORCE" -eq 1 ] && FORCE_ARG="--force"
    sh "$REPO/tools/userspace/build-toasters.sh" $FORCE_ARG
fi

echo ""
echo "==> X11/Matchbox stack ready."
echo "    Libraries + xkbcomp/xev/Xfbdev:  $STAGE, and in-tree under userspace/src/"
echo "    Matchbox apps:                  $D_WM $D_DESKTOP $D_PANEL $D_COMMON $D_CARD $D_VOLUME $D_BRIGHT $D_PIKAFFEINE"
echo "    Package into a payload with:    tools/userspace/build-matchbox-payload.sh"
