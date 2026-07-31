#!/bin/sh
set -eu

# Cross-builds opkg (the package manager) for the device, into the ARM
# staging tree at userspace/stage-target.
#
# Usage:
#   tools/build-opkg.sh [--force]
#
# Prerequisite: tools/build-thirdparty-deps.sh libarchive
# (opkg cannot be built without libarchive -- see below).
#
#
# WHY STOCK OPKG, NOT A FORK
# ==========================
# The two device-specific behaviours we need -- installing to either NAND
# or the SD card, and refusing Sharp-era packages -- are both already
# expressible in stock opkg. Neither one is patched here:
#
#   * Two destinations is opkg's oldest feature, inherited from ipkg,
#     which was written for exactly this (a handheld with a tiny internal
#     flash and a removable card). `dest <name> <path>` in the config,
#     `opkg -d <name> install ...` to pick one. Each destination gets its
#     OWN status file and info dir under <path>/usr/lib/opkg (see
#     pkg_dest_init() in libopkg/pkg_dest.c), so packages installed to a
#     card are recorded on that card and travel with it.
#
#   * Refusing Sharp packages is the `arch` config directive plus the
#     wrapper's pre-flight check. See rootfs/etc/opkg/opkg.conf and
#     rootfs/usr/sbin/pkgadd for the split and why neither alone is
#     enough.
#
# The one thing that genuinely cannot come from opkg -- making the
# Matchbox desktop notice apps that appeared on a newly inserted card --
# is not an opkg concern at all and is handled in matchbox-desktop.
#
#
# LIBARCHIVE IS NOT OPTIONAL
# ==========================
# Every opkg release depends on libarchive: configure.ac has an
# unconditional PKG_CHECK_MODULES([LIBARCHIVE], [libarchive]) as far back
# as 0.3.0, and it is still there at git master. The bundled
# busybox-derived untar that people remember belonged to *ipkg*, opkg's
# predecessor, and has never been in an opkg release. There is no version
# of this program that avoids the dependency, so it is pinned and built
# alongside the other third-party libraries.
#
#
# WHY 0.6.3
# =========
# Last release of the autotools series, which is what every other build
# script in this tree drives. 0.7.0+ moved to CMake, which would mean
# carrying a CMake toolchain file for this one package; nothing in 0.7+
# is needed here. If that changes, the port is mechanical -- the configure
# switches below have one-to-one CMake option equivalents.

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
STAGE="$REPO/userspace/stage-target"
CACHE="${THIRDPARTY_CACHE:-$REPO/userspace/.thirdparty-cache}"
BUILD="${OPKG_BUILD:-/tmp/piko-opkg-build}"

VERSION=0.6.3
URL="https://downloads.yoctoproject.org/releases/opkg/opkg-$VERSION.tar.gz"
SHA256=f3938e359646b406c40d5d442a1467c7e72357f91ab822e442697529641e06de
MARKER="usr/bin/opkg"

HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/$HOST/bin}"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '3,10p' "$0"; exit 0 ;;
        *) echo "FAILED: unknown option: $arg" >&2; exit 1 ;;
    esac
done

if [ ! -d "$TOOLCHAIN_BIN_DIR" ]; then
    echo "FAILED: toolchain bin dir not found: $TOOLCHAIN_BIN_DIR" >&2
    echo "Run tools/build-uclibc-toolchain.sh, or set TOOLCHAIN_BIN_DIR." >&2
    exit 1
fi

if [ ! -f "$STAGE/usr/lib/pkgconfig/libarchive.pc" ]; then
    echo "FAILED: libarchive is not staged in $STAGE." >&2
    echo "Run:  tools/build-thirdparty-deps.sh libarchive" >&2
    exit 1
fi

if [ "$FORCE" -eq 0 ] && [ -f "$STAGE/$MARKER" ]; then
    echo "==> opkg $VERSION: already staged, skipping (--force to rebuild)"
    exit 0
fi

PATH="$TOOLCHAIN_BIN_DIR:$PATH"
export PATH
export CC="${HOST}-gcc"
export AR="${HOST}-ar"
export RANLIB="${HOST}-ranlib"
export STRIP="${HOST}-strip"
export PKG_CONFIG_SYSROOT_DIR="$STAGE"
export PKG_CONFIG_LIBDIR="$STAGE/usr/lib/pkgconfig:$STAGE/usr/share/pkgconfig"
export PKG_CONFIG_PATH=
export CPPFLAGS="-I$STAGE/usr/include"

# FULLY static, unlike the X11 stack. Three reasons, in order of weight:
#
#  1. opkg is the program that overwrites files on this system. Anything
#     it needs at runtime is something it can destroy mid-install and
#     then be unable to run in order to fix. A static opkg still starts
#     when the libraries it just replaced are broken.
#  2. It has to work in situations where the Matchbox payload has NOT
#     been deployed -- a freshly flashed board, or recovery. The dynamic
#     loader and libc.so.0 come from that payload (see
#     build-matchbox-payload.sh); a dynamically linked binary on a rootfs
#     without them dies with a bare "not found" that reads like a missing
#     file rather than a missing loader, which is exactly the trap
#     build-userspace.sh already calls out for md5sum.
#  3. It matches what every other standalone tool here does (md5sum,
#     alsa-utils, MPlayer).
#
# Note that the static link is requested at `make` time with libtool's
# -all-static, NOT by putting -static in LDFLAGS here. opkg links through
# libtool, and libtool reinterprets a plain -static as "prefer the static
# archive of the libtool libraries" rather than "produce a static
# executable" -- it then drops it from the final link line. The build
# succeeds, the binary looks fine, and it is still dynamically linked. The
# check at the end of this script exists because that failure is
# completely silent.
export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"

mkdir -p "$CACHE" "$BUILD" "$STAGE"

tarball="$CACHE/opkg-$VERSION.tar.gz"
if [ ! -f "$tarball" ]; then
    echo "    downloading opkg-$VERSION.tar.gz"
    curl -fL --retry 3 -o "$tarball.partial" "$URL"
    mv "$tarball.partial" "$tarball"
fi
got="$(sha256sum "$tarball" | cut -d' ' -f1)"
if [ "$got" != "$SHA256" ]; then
    echo "FAILED: sha256 mismatch for opkg-$VERSION.tar.gz" >&2
    echo "  expected $SHA256" >&2
    echo "  got      $got" >&2
    exit 1
fi

echo "==> opkg $VERSION"
srcdir="$BUILD/opkg-$VERSION"
rm -rf "$srcdir"
tar xf "$tarball" -C "$BUILD"

( cd "$srcdir"

  # --with-static-libopkg + --disable-shared: produce ONE binary with no
  # libopkg.so beside it. This is not (only) about size. opkg is the
  # program that overwrites files on this system, so anything it links
  # dynamically is something it can break by installing a bad package --
  # and if it breaks libopkg.so, there is no working opkg left to undo it
  # with. libarchive is static for the same reason (see
  # build-thirdparty-deps.sh). libc and libz stay dynamic: both are
  # already on the device, shared by everything else, and a rootfs whose
  # libc is broken has bigger problems than opkg.
  #
  # --disable-curl: there is no HTTP feed yet. Package feeds live on the
  # SD card and are reached with `src/gz ... file:/mnt/card/...`, and
  # file: URLs are handled by a plain copy in opkg_download_internal()
  # BEFORE any download backend is consulted (libopkg/opkg_download.c),
  # so local feeds work fully with curl compiled out -- `opkg update`
  # and `opkg install <name>` included. Enabling curl later is a
  # one-flag change and needs openssl for https.
  #
  # --disable-gpg: feed signing needs gpgme + gpg-error + libassuan, and
  # a card-local feed the user assembled themselves gains nothing from a
  # signature check against a keyring on the same card.
  #
  # --disable-sha256 / --disable-ssl-curl: both pull in openssl.
  # --without-libsolv: opkg's internal dependency solver is used instead;
  #   libsolv is a large C++-adjacent dependency for a device that will
  #   have a few dozen packages at most.
  # --without-acl / --without-xattr: uclibc has neither.
  #
  # --sysconfdir=/etc and --localstatedir=/var are BOTH mandatory, and for
  # the same autoconf reason fontconfig needs them (see the note in
  # build-thirdparty-deps.sh): with a bare --prefix=/usr, autoconf derives
  # sysconfdir as /usr/etc and localstatedir as /usr/var, and opkg bakes
  # both into the binary at compile time.
  #
  # Getting sysconfdir wrong is the dangerous one. opkg would look for its
  # config in /usr/etc/opkg/, find nothing, and run with NO configuration
  # -- and an opkg with no `arch` lines does not fail closed. It falls
  # back to a built-in list of {all, noarch, HOST_CPU_STR}, and
  # HOST_CPU_STR here is literally "arm", which is exactly what Sharp's
  # packages declare. Verified under qemu-arm: with the arch lines
  # removed from opkg.conf, a Sharp-format `Architecture: arm` package
  # installs cleanly and silently. The whole retro-compat gate rests on
  # this path being right.
  #
  # localstatedir wrong is merely ugly: the package database lands in
  # /usr/var/lib/opkg instead of /var/lib/opkg.
  ./configure --host="$HOST" --build="$(uname -m)-pc-linux-gnu" \
              --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
              --with-static-libopkg --disable-shared --enable-static \
              --disable-curl --disable-gpg --disable-sha256 \
              --disable-ssl-curl --disable-libopkg-api \
              --disable-xz --disable-bzip2 --disable-lz4 --disable-zstd \
              --without-libsolv --without-acl --without-xattr

  make -j"$(nproc 2>/dev/null || echo 4)" LDFLAGS="-L$STAGE/usr/lib -all-static"
  make install DESTDIR="$STAGE" LDFLAGS="-L$STAGE/usr/lib -all-static"
)

rm -f "$STAGE"/usr/lib/*.la

if [ ! -f "$STAGE/$MARKER" ]; then
    echo "FAILED: opkg built but $MARKER is missing from the staging tree" >&2
    exit 1
fi

# Prove the static link actually happened. libtool silently discarding
# -static (see the LDFLAGS note above) produces a working-looking build
# whose binary needs a dynamic loader the recovery rootfs may not have,
# and the symptom on the device is an unhelpful "not found". Catch it
# here, on the build host, where the message can say what is wrong.
if "$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$STAGE/$MARKER" 2>/dev/null | grep -q NEEDED; then
    echo "FAILED: opkg linked dynamically -- the -all-static link did not take." >&2
    echo "Shared libraries it still needs:" >&2
    "$TOOLCHAIN_BIN_DIR/$HOST-readelf" -d "$STAGE/$MARKER" | grep NEEDED >&2
    exit 1
fi

"$STRIP" "$STAGE/$MARKER" 2>/dev/null || true

echo "    staged: $MARKER ($(wc -c < "$STAGE/$MARKER") bytes, static)"
echo "==> opkg ready in $STAGE"
