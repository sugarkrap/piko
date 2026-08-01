#!/bin/sh
set -eu

# Cross-compiles alsa-lib + alsa-utils (aplay/amixer/alsactl/speaker-test/etc)
# for the Zaurus SL-C760 (PXA255, ARMv5TE, soft-float, uClibc) so a real WAV
# can be played over the WM8731/Corgi ALSA stack the kernel side already
# exposes at /dev/snd/* (see AGENTS.md / handoff docs -- kernel ALSA is done,
# this is the userspace half).
#
# Downloads pristine alsa-project.org release tarballs, extracts them under
# userspace/src/ (gitignored vendor trees, same philosophy as the
# dropbear/wpa_supplicant/kexec-tools trees already there), configures +
# builds + installs both packages, then assembles two output trees:
#
#   userspace/stage-alsa/          full DESTDIR install (prefix=/usr),
#                                   headers + .pc/.la + man pages included --
#                                   handy if something else ever needs to
#                                   link against libasound, but NOT meant to
#                                   be copied to the device as-is.
#   userspace/stage-alsa-runtime/  pruned payload: binaries, libasound.so*,
#                                   the /usr/share/alsa config tree, and the
#                                   alsa-utils sample WAVs -- copy this whole
#                                   directory onto the device root (it is
#                                   already usr/... rooted).
#
# Two cross-build traps this script works around (see comments inline at
# each step for the mechanism):
#
#   1. libtool .la collision: alsa-lib is configured with --prefix=/usr (so
#      the ALSA_CONFIG_DIR/ALSA_PLUGIN_DIR strings baked into libasound.so
#      are the correct on-device paths, /usr/share/alsa and
#      /usr/lib/alsa-lib). But that means the installed libasound.la records
#      libdir='/usr/lib' -- and when alsa-utils' cross-build later resolves
#      "-lasound" via that .la, libtool trusts the RECORDED libdir over the
#      directory the .la actually sits in, and silently links against the
#      *build host's own native* /usr/lib/libasound.so instead of our staged
#      ARM one (this actually happened on the first attempt: fully linked,
#      "wrong ELF class" style failure). Fixed by rewriting libdir= in the
#      installed .la to point at the real staging path before building
#      alsa-utils -- purely a host-side build bookkeeping fix, the .la is
#      never shipped to the device.
#   2. Bogus RPATH: because of (1), every alsa-utils binary that links
#      -lasound gets a libtool-injected -rpath baked in pointing at this
#      build's staging directory (an absolute host path, meaningless -- and
#      on a From-clean-checkout rebuild, WRONG -- on the device). chrpath and
#      patchelf are not available in this environment, so a tiny inline
#      Python pass blanks the RPATH string in-place (turns it into an empty,
#      zero-length string -- safe: ld.so treats a zero-length RPATH as no
#      extra search dirs, not as "search cwd").
#
# Per docs/archive/DEADLETTER-WIFI-SSH.md's documented trap: never pass
# CFLAGS=/LDFLAGS= on the make command line (clobbers Makefile += accumulation
# in some packages) -- this script only ever passes them at ./configure time,
# as env/arg assignments, never to `make`.
#
# Usage:
#   tools/build-alsa.sh [--force]
#
# --force wipes and re-extracts both source trees even if already present
# (the configure/build/install steps always rerun regardless -- cheap).
#
# Env overrides:
#   ALSA_LIB_VERSION    default 1.2.12
#   ALSA_UTILS_VERSION  default 1.2.12
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE       default arm-unknown-linux-uclibcgnueabi-
#   STAGE_DIR           default <repo>/userspace/stage-alsa (full dev install)
#   RUNTIME_DIR         default <repo>/userspace/stage-alsa-runtime (device payload)
#   JOBS                default: nproc
#
# Exit codes:
#   0   $RUNTIME_DIR was assembled successfully
#   1   a hard failure (download, configure, build, or install failure)

REPO="$(cd "$(dirname "$0")/.." && pwd)"
ALSA_LIB_VERSION="${ALSA_LIB_VERSION:-1.2.12}"
ALSA_UTILS_VERSION="${ALSA_UTILS_VERSION:-1.2.12}"
SRC_DIR="$REPO/userspace/src"

ALSA_LIB_SRC_DIR="${ALSA_LIB_SRC_DIR:-$SRC_DIR/alsa-lib-$ALSA_LIB_VERSION}"
ALSA_LIB_TARBALL="${ALSA_LIB_TARBALL:-$SRC_DIR/alsa-lib-$ALSA_LIB_VERSION.tar.bz2}"
ALSA_LIB_URL="https://www.alsa-project.org/files/pub/lib/alsa-lib-$ALSA_LIB_VERSION.tar.bz2"

ALSA_UTILS_SRC_DIR="${ALSA_UTILS_SRC_DIR:-$SRC_DIR/alsa-utils-$ALSA_UTILS_VERSION}"
ALSA_UTILS_TARBALL="${ALSA_UTILS_TARBALL:-$SRC_DIR/alsa-utils-$ALSA_UTILS_VERSION.tar.bz2}"
ALSA_UTILS_URL="https://www.alsa-project.org/files/pub/utils/alsa-utils-$ALSA_UTILS_VERSION.tar.bz2"

STAGE_DIR="${STAGE_DIR:-$REPO/userspace/stage-alsa}"
RUNTIME_DIR="${RUNTIME_DIR:-$REPO/userspace/stage-alsa-runtime}"

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
    echo "tools/build-alsa.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"

# --- fetch + extract helper ------------------------------------------------
fetch_and_extract() {
    _tarball="$1"; _url="$2"; _srcdir="$3"; _label="$4"

    if [ ! -f "$_tarball" ]; then
        echo "==> downloading $_url"
        curl -fL -o "$_tarball.partial" "$_url"
        mv "$_tarball.partial" "$_tarball"
    else
        echo "==> reusing cached $_tarball"
    fi

    if [ "$FORCE" -eq 1 ] && [ -d "$_srcdir" ]; then
        echo "==> --force: removing existing $_srcdir"
        rm -rf "$_srcdir"
    fi

    if [ ! -d "$_srcdir" ]; then
        echo "==> extracting $_label to $SRC_DIR"
        tar xjf "$_tarball" -C "$SRC_DIR"
    else
        echo "==> reusing existing source tree $_srcdir"
    fi

    if [ ! -f "$_srcdir/configure" ]; then
        echo "tools/build-alsa.sh: $_srcdir doesn't look like a configure-based tree" >&2
        exit 1
    fi
}

fetch_and_extract "$ALSA_LIB_TARBALL" "$ALSA_LIB_URL" "$ALSA_LIB_SRC_DIR" "alsa-lib-$ALSA_LIB_VERSION"
fetch_and_extract "$ALSA_UTILS_TARBALL" "$ALSA_UTILS_URL" "$ALSA_UTILS_SRC_DIR" "alsa-utils-$ALSA_UTILS_VERSION"

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

# --- 1. alsa-lib ------------------------------------------------------------
# --prefix=/usr (NOT a staging path) so ALSA_CONFIG_DIR/ALSA_PLUGIN_DIR baked
# into libasound are the real on-device paths. --with-softfloat matches this
# hardware (PXA255: no FPU/VFP/NEON at all). UCM/topology/python are unneeded
# for aplay/amixer and dropped to save size.
#
# --disable-shared --enable-static: this device's rootfs ships NO dynamic
# linker at all (no /lib/ld-uClibc.so.0, no /usr/lib) -- every working binary
# on it (kexec, dropbear, iwconfig, wpa_supplicant) is fully static, per this
# project's established convention (see AGENTS.md). The first build of this
# script used autotools' shared-library default and produced an aplay/amixer
# that referenced /lib/ld-uClibc.so.0 -- confirmed on real hardware to fail
# with "not found" (busybox ash's generic exec()-ENOENT message, easily
# mistaken for a missing-file typo rather than a missing dynamic linker).
echo "==> configuring alsa-lib $ALSA_LIB_VERSION"
(
    cd "$ALSA_LIB_SRC_DIR"
    [ -f Makefile ] && make distclean >/dev/null 2>&1
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --disable-shared --enable-static \
        --disable-python \
        --disable-old-symbols \
        --disable-ucm \
        --disable-topology \
        --with-softfloat \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP"
    echo "==> building alsa-lib"
    make -j"$JOBS"
    echo "==> installing alsa-lib to $STAGE_DIR"
    make install DESTDIR="$STAGE_DIR"
)

# Trap #1 fix (see header comment): rewrite the installed .la's libdir= from
# the real device path (/usr/lib, matching the build host's own libdir) to
# the actual staging location, so alsa-utils' cross-link resolves OUR
# cross-compiled libasound.so instead of accidentally matching the build
# host's native one at the same recorded path.
LIBASOUND_LA="$STAGE_DIR/usr/lib/libasound.la"
if [ -f "$LIBASOUND_LA" ]; then
    sed -i "s|^libdir='/usr/lib'|libdir='$STAGE_DIR/usr/lib'|" "$LIBASOUND_LA"
fi

# --- 2. alsa-utils ------------------------------------------------------------
# --with-alsa-prefix/--with-alsa-inc-prefix point at the alsa-lib we just
# staged (not pkg-config -- PKG_CONFIG=false so nothing accidentally
# resolves against the build host's own alsa.pc). alsamixer/bat/alsaloop/
# nhlt/alsaconf/xmlto/rst2man are all dropped: ncurses, fftw3f, libsamplerate,
# perl, and doc-toolchain dependencies respectively, none of which exist (or
# are wanted) on this device -- aplay/amixer/alsactl/speaker-test/axfer are
# the deliverables and none of them need any of that.
echo "==> configuring alsa-utils $ALSA_UTILS_VERSION"
(
    cd "$ALSA_UTILS_SRC_DIR"
    [ -f Makefile ] && make distclean >/dev/null 2>&1
    ./configure \
        --host=arm-unknown-linux-uclibcgnueabi \
        --build="$(./config.guess 2>/dev/null || echo x86_64-pc-linux-gnu)" \
        --prefix=/usr \
        --with-alsa-prefix="$STAGE_DIR/usr/lib" \
        --with-alsa-inc-prefix="$STAGE_DIR/usr/include" \
        --disable-alsamixer \
        --disable-bat \
        --disable-alsaloop \
        --disable-nhlt \
        --disable-alsaconf \
        --disable-xmlto \
        --disable-rst2man \
        --disable-nls \
        --disable-rpath \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
        PKG_CONFIG=false
    echo "==> building alsa-utils"
    # -all-static is a libtool-only flag (forces truly static linking, no
    # dynamic deps at all -- unlike raw "-static", which libtool only takes
    # as a hint to prefer .a over .so for -lfoo, still leaving libc.so.0
    # NEEDED in practice). It must NOT be passed at ./configure time: the
    # AC_PROG_CC sanity check invokes the compiler directly, without
    # libtool, and "-all-static" isn't a real gcc/ld flag -- passing it
    # there breaks "checking whether the C compiler works" outright. Passing
    # it only here, at make time, is safe: alsa-utils' configure records an
    # empty LDFLAGS (none given above), so this replaces nothing that
    # mattered -- unlike the CFLAGS/LDFLAGS-on-the-make-command-line trap
    # documented in docs/archive/DEADLETTER-WIFI-SSH.md, which was about
    # clobbering a package's own already-recorded, load-bearing flags.
    make -j"$JOBS" LDFLAGS="-all-static"
    echo "==> installing alsa-utils to $STAGE_DIR"
    make install DESTDIR="$STAGE_DIR"
)

# Trap #2 fix (see header comment): blank the bogus staging-path RPATH that
# libtool baked into every alsa-utils binary that links -lasound. chrpath/
# patchelf aren't available here, so do it with a small in-place byte patch:
# find the known staging-path string wherever it appears NUL-terminated in
# the ELF (that's exactly how RPATH/RUNPATH entries are stored) and null it
# out. A zero-length RPATH is a documented no-op for the dynamic linker.
echo "==> stripping bogus build-host RPATH from installed binaries"
python3 - "$STAGE_DIR/usr/lib" "$STAGE_DIR"/usr/bin/* "$STAGE_DIR"/usr/sbin/* <<'PYEOF'
import sys

def strip(path, needle):
    try:
        with open(path, 'rb') as f:
            data = bytearray(f.read())
    except IsADirectoryError:
        return False
    start = 0
    found = False
    while True:
        idx = data.find(needle, start)
        if idx == -1:
            break
        end = data.find(b'\x00', idx)
        if end == -1:
            end = idx + len(needle)
        for i in range(idx, end):
            data[i] = 0
        found = True
        start = end + 1
    if found:
        with open(path, 'wb') as f:
            f.write(data)
    return found

needle = sys.argv[1].encode()
for path in sys.argv[2:]:
    import os
    if os.path.islink(path) or not os.path.isfile(path):
        continue
    strip(path, needle)
PYEOF

# --- 3. strip debug info to keep the payload small --------------------------
echo "==> stripping debug symbols"
for f in "$STAGE_DIR"/usr/bin/* "$STAGE_DIR"/usr/sbin/*; do
    [ -L "$f" ] && continue
    [ -f "$f" ] || continue
    "$STRIP" --strip-unneeded "$f" 2>/dev/null || true
done

# --- 4. assemble the pruned runtime-only payload -----------------------------
# Drop everything that's only needed to build OTHER software against
# libasound (headers, static archive, .la/.pc, aclocal macro, man pages) --
# with --disable-shared, every alsa-utils binary has libasound baked in
# statically, so none of usr/lib is needed to run aplay on the device.
echo "==> assembling runtime payload at $RUNTIME_DIR"
rm -rf "$RUNTIME_DIR"
mkdir -p "$RUNTIME_DIR"
cp -a "$STAGE_DIR/usr" "$RUNTIME_DIR/usr"
rm -rf \
    "$RUNTIME_DIR/usr/include" \
    "$RUNTIME_DIR/usr/lib" \
    "$RUNTIME_DIR/usr/share/aclocal" \
    "$RUNTIME_DIR/usr/share/man"
# alsactl wants somewhere to save/restore mixer state.
mkdir -p "$RUNTIME_DIR/var/lib/alsa"

echo "==> done"
echo "    full dev install:    $STAGE_DIR"
echo "    device runtime tree: $RUNTIME_DIR"
echo "    total runtime size:  $(du -sh "$RUNTIME_DIR" | cut -f1)"
echo "    ELF check (aplay):"
"$READELF" -h "$RUNTIME_DIR/usr/bin/aplay" 2>/dev/null | grep -iE "type|flags" | sed 's/^/      /'
echo "    dynamic deps (should be none -- fully static):"
"$READELF" -d "$RUNTIME_DIR/usr/bin/aplay" 2>/dev/null | grep -i needed | sed 's/^/      /'
