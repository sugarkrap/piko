#!/bin/sh
set -eu

# Cross-compiles the SSH file-transfer half of this device's only remote
# access path (AGENTS.md: no USB, no serial -- WiFi->SSH is it) for the
# Zaurus SL-C760 (PXA255, ARMv5TE, soft-float, uClibc):
#
#   usr/bin/scp            dropbear's scp (OpenSSH's scp.c, vendored)
#   usr/libexec/sftp-server OpenSSH's sftp-server
#   usr/sbin/dropbear      the SSH server itself (see "WHY REBUILD" below)
#   usr/bin/dbclient       dropbear's SSH client -- scp's transport
#   usr/bin/dropbearkey    host/user key generation
#
# assembled into userspace/stage-ssh/, already usr/... rooted so the whole
# tree can be copied onto the device root as-is (same convention as
# tools/build-alsa.sh's stage-alsa-runtime and tools/build-sdl.sh's
# stage-sdl-runtime).
#
# WHY BOTH scp AND sftp-server, they are not redundant:
#   - `scp` is the classic rcp-over-ssh protocol. The binary is needed on
#     the DEVICE because the remote end of any `scp file zaurus:/tmp` is
#     `scp -t /tmp` executed on the device. Dropbear ships scp.c but
#     builds it as a SEPARATE binary (`make scp`), not part of the server.
#   - `sftp-server` is a different protocol and is NOT part of dropbear at
#     all. Dropbear only ever exec()s an external one (svr-chansession.c,
#     guarded by DROPBEAR_SFTPSERVER, path SFTPSERVER_PATH). The binary
#     itself has to come from OpenSSH portable, which is why this script
#     builds two upstream source trees rather than one.
#   - It is not "sftp is the modern one, skip scp": OpenSSH 9.0+ clients
#     use the SFTP protocol for `scp` BY DEFAULT, so without sftp-server
#     every `scp` from a current desktop fails until you remember `-O`;
#     and without scp, `scp -O` (and any older client) fails instead.
#     Shipping one without the other leaves a broken-looking transfer in
#     one direction of client-version space.
#
# WHY REBUILD DROPBEAR AT ALL: because nothing in this repo could
# reproduce it. The tree is a gitignored vendor drop (userspace/src/
# dropbear-2025.88/) that a bring-up session built by hand, and it is no
# longer on any disk -- the SSH server this project's only remote path
# depends on had no build script. It does now. The two non-default build
# decisions that took a full debugging session to find are baked in here
# so they can never be lost again (docs/DEADLETTER-DROPBEAR-PTY.md):
#
#   1. USE_DEV_PTMX must be #defined in config.h by hand. configure
#      probes for it by RUNNING a test binary, which cross-compiling
#      cannot do, so it leaves every PTY method undef and sshpty.c falls
#      through to the legacy /dev/pty?? BSD loop -- which this kernel
#      (CONFIG_LEGACY_PTYS=n) cannot satisfy. Symptom: "PTY allocation
#      request failed on channel 0". HAVE_OPENPTY is deliberately NOT the
#      fix: this uClibc has no openpty (no libutil), so --disable-openpty
#      is passed to keep configure from even considering it.
#   2. It must be a single-binary (non-MULTI) build. The MULTI=1 link
#      drops extra LIBS, which is how the historical missing-syscall()
#      link failure appeared. This toolchain's uClibc DOES provide
#      syscall() (checked at the top of the build below), so no shim is
#      needed here -- but the check is explicit rather than assumed,
#      because a silent regression there breaks dbutil.c's
#      gettime_wrapper at link time with a confusing error.
#
# The rebuilt dropbear is NOT automatically installed over the running
# one -- see tools/chunked-deploy.sh's SSH step. Replacing the live SSH
# server over SSH on a board with no serial console is the one deploy
# that can strand the machine, so it is opt-in (--replace-dropbear) and
# staged rename-aside, while scp/sftp-server (new files, nothing depends
# on them yet) always ship.
#
# WHY THE EXISTING SERVER PROBABLY ALREADY DOES SFTP: DROPBEAR_SFTPSERVER
# is 1 and SFTPSERVER_PATH is "/usr/libexec/sftp-server" in dropbear's
# OWN default_options.h, and the on-device build used stock options. So
# dropping sftp-server at that exact path is expected to be sufficient
# with no server change at all. "Expected" is not "verified" -- the
# device was unreachable when this was written. Verify on hardware with:
#     ssh -i ~/.ssh/zaurus_ed25519 root@<dev> -s sftp   # should not exit
#   (or just `sftp root@<dev>`). If it fails with "subsystem request
#   failed", the shipped dropbear here is the fix -- deploy it with
#   tools/chunked-deploy.sh --replace-dropbear.
#
# STATIC, like ALSA/MPlayer and unlike SDL/X11: these three binaries are
# the recovery path. If a bad X11 payload ever leaves /lib/ld-uClibc*.so
# half-written, a dynamically-linked scp/sftp-server would fail exactly
# when it is most needed to push the fix. tools/build-alsa.sh's header
# has the longer version of this argument.
#
# STATIC IS NOT ENOUGH -- THE LIBC MUST BE uClibc. Unlike every other
# component here, "static glibc, the libc doesn't matter once it's linked
# in" (flash/build-update-package.sh's reasoning for piko-update, which is
# correct FOR piko-update) does NOT hold for these. sftp-server's main()
# calls getpwuid(), and dropbear calls getpwnam()/getspnam(); glibc
# implements those through NSS, which dlopen()s libnss_files.so.2 AT
# RUNTIME even from a fully static binary. That file does not exist on
# this rootfs, so a glibc-static sftp-server would link cleanly, pass a
# `file`/readelf check, and then die with "No user found for uid 0" on the
# device. So CI must use this project's uClibc toolchain
# (tools/build-uclibc-toolchain.sh), not the apt-installed
# gcc-arm-linux-gnueabi it uses elsewhere -- and the NSS check at the
# bottom of this script enforces it rather than trusting the caller.
#
# OpenSSH is configured --without-openssl --without-zlib: sftp-server
# does no key exchange and no compression, so both dependencies are pure
# build-time cost (and cross-built OpenSSL for this target does not
# exist in this repo). OpenSSH's bundled crypto covers what libssh.a
# still references.
#
# Usage:
#   tools/build-ssh.sh [--force]
#
# --force wipes and re-extracts both source trees (the configure/build
# steps always rerun regardless -- they are only a couple of minutes).
#
# Env overrides:
#   DROPBEAR_VERSION    default 2025.88   (matches what is on the device)
#   OPENSSH_VERSION     default 10.4p1
#   TOOLCHAIN_BIN_DIR   default <repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
#   CROSS_COMPILE       default arm-unknown-linux-uclibcgnueabi-
#   STAGE_DIR           default <repo>/userspace/stage-ssh
#   JOBS                default: nproc
#
# Exit codes:
#   0   $STAGE_DIR assembled and every binary passed its ELF + content
#       checks (right ABI, actually static, sftp path/ptmx strings
#       present in dropbear)
#   1   a hard failure (download, checksum, configure, build, or any
#       verification step)

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
SRC_DIR="$REPO/userspace/src"

DROPBEAR_VERSION="${DROPBEAR_VERSION:-2025.88}"
DROPBEAR_SRC_DIR="$SRC_DIR/dropbear-$DROPBEAR_VERSION"
DROPBEAR_TARBALL="$SRC_DIR/dropbear-$DROPBEAR_VERSION.tar.bz2"
DROPBEAR_URL="https://matt.ucc.asn.au/dropbear/releases/dropbear-$DROPBEAR_VERSION.tar.bz2"
DROPBEAR_SHA256="783f50ea27b17c16da89578fafdb6decfa44bb8f6590e5698a4e4d3672dc53d4"

OPENSSH_VERSION="${OPENSSH_VERSION:-10.4p1}"
OPENSSH_SRC_DIR="$SRC_DIR/openssh-$OPENSSH_VERSION"
OPENSSH_TARBALL="$SRC_DIR/openssh-$OPENSSH_VERSION.tar.gz"
OPENSSH_URL="https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-$OPENSSH_VERSION.tar.gz"
OPENSSH_SHA256="ef6026dd2aea8d56059638d5d3262902c892ceba9f88395835e0d06d3fb63238"

# Where dropbear will exec the sftp server from, and therefore where this
# script installs it. Changing this means rebuilding dropbear too (it is
# compiled into the server), which is why it is one variable used for
# both the install path and the localoptions.h define below.
SFTPSERVER_PATH="${SFTPSERVER_PATH:-/usr/libexec/sftp-server}"

STAGE_DIR="${STAGE_DIR:-$REPO/userspace/stage-ssh}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"
CROSS_HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"
# Spelled out rather than relying on the toolchain's built-in defaults (which
# do already match): these two flags are what the ELF-flags check below
# actually verifies, so a toolchain whose defaults drift fails the check
# instead of silently producing binaries this PXA255 cannot run.
TARGET_CFLAGS="${TARGET_CFLAGS:--march=armv5te -mfloat-abi=soft -O2}"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help)
            sed -n '3,110p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "tools/build-ssh.sh: unknown argument: $arg" >&2
            echo "Usage: tools/build-ssh.sh [--force]" >&2
            exit 1
            ;;
    esac
done

mkdir -p "$SRC_DIR"

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/build-ssh.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    echo "A fresh machine builds it with tools/build-uclibc-toolchain.sh." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"
BUILD_TRIPLET="$(uname -m)-pc-linux-gnu"

# --- syscall() sanity check (see header note 2) -----------------------------
# dbutil.c's gettime_wrapper calls syscall() directly. The buildroot-era
# toolchain a previous session used had no syscall() in its uClibc, which is
# where docs/DEADLETTER-DROPBEAR-PTY.md's syscall_shim.o came from. Prove the
# current toolchain has it rather than discovering it as an "undefined
# reference to syscall" 200 object files into the build.
SYSCALL_TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$SYSCALL_TEST_DIR"' EXIT INT TERM
cat > "$SYSCALL_TEST_DIR/t.c" <<'EOF'
#include <unistd.h>
#include <sys/syscall.h>
int main(void) { return (int)syscall(SYS_getpid); }
EOF
if ! "$CC" -o "$SYSCALL_TEST_DIR/t" "$SYSCALL_TEST_DIR/t.c" >"$SYSCALL_TEST_DIR/log" 2>&1; then
    echo "tools/build-ssh.sh: this toolchain's libc has no syscall()." >&2
    echo "dropbear's dbutil.c needs it; see docs/DEADLETTER-DROPBEAR-PTY.md for the" >&2
    echo "syscall_shim.o workaround that a previous toolchain required." >&2
    sed 's/^/    /' "$SYSCALL_TEST_DIR/log" >&2
    exit 1
fi

fetch_verify_extract() {
    tarball="$1"; url="$2"; want_sha="$3"; srcdir="$4"; tar_flag="$5"

    if [ "$FORCE" -eq 1 ] && [ -d "$srcdir" ]; then
        echo "==> --force: removing existing $srcdir"
        rm -rf "$srcdir"
    fi

    if [ ! -f "$tarball" ]; then
        echo "==> downloading $url"
        curl -fL -o "$tarball.partial" "$url"
        mv "$tarball.partial" "$tarball"
    else
        echo "==> reusing cached $tarball"
    fi

    echo "==> verifying sha256 of $(basename "$tarball")"
    actual="$(sha256sum "$tarball" | cut -d' ' -f1)"
    if [ "$actual" != "$want_sha" ]; then
        echo "tools/build-ssh.sh: SHA-256 mismatch for $tarball" >&2
        echo "  expected: $want_sha" >&2
        echo "  actual:   $actual" >&2
        echo "Refusing to build from a tarball that doesn't match -- remove it and" >&2
        echo "rerun if you deliberately changed the version/URL above." >&2
        exit 1
    fi

    if [ ! -d "$srcdir" ]; then
        echo "==> extracting $(basename "$tarball") to $SRC_DIR"
        tar "x${tar_flag}f" "$tarball" -C "$SRC_DIR"
    fi
    if [ ! -f "$srcdir/configure" ]; then
        echo "tools/build-ssh.sh: $srcdir doesn't look like a configure-based tree" >&2
        exit 1
    fi
}

fetch_verify_extract "$DROPBEAR_TARBALL" "$DROPBEAR_URL" "$DROPBEAR_SHA256" \
                     "$DROPBEAR_SRC_DIR" j
fetch_verify_extract "$OPENSSH_TARBALL" "$OPENSSH_URL" "$OPENSSH_SHA256" \
                     "$OPENSSH_SRC_DIR" z

rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/usr/bin" "$STAGE_DIR/usr/sbin" \
         "$STAGE_DIR$(dirname "$SFTPSERVER_PATH")"

# ---------------------------------------------------------------------------
# 1. dropbear: server + scp + dbclient + dropbearkey
# ---------------------------------------------------------------------------
# --disable-zlib:    the device's dropbear has never had it; nothing on the
#                    link is compression-bound and it is one less static lib.
# --disable-openpty: uClibc has no openpty (no libutil). Without this,
#                    configure could latch onto a host-looking answer and
#                    produce a server that fails PTY allocation.
# --disable-harden:  the hardening flags (PIE + full RELRO) are not a
#                    combination this armv5 static build is known-good with,
#                    and a non-booting SSH server is not worth the trade on a
#                    device whose only access path it is.
# --enable-static:   see header. Also makes the "no NEEDED entries" check
#                    below meaningful.
# --disable-lastlog/utmp/wtmp/pututline: this rootfs has none of those files;
#                    every one of them is a write that silently fails.
echo "==> configuring dropbear $DROPBEAR_VERSION"
(
    cd "$DROPBEAR_SRC_DIR"
    [ -f Makefile ] && make clean >/dev/null 2>&1
    ./configure \
        --host="$CROSS_HOST" \
        --build="$BUILD_TRIPLET" \
        --prefix=/usr \
        --enable-static \
        --disable-zlib \
        --disable-openpty \
        --disable-harden \
        --disable-lastlog \
        --disable-utmp --disable-utmpx \
        --disable-wtmp --disable-wtmpx \
        --disable-pututline --disable-pututxline \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
        CFLAGS="$TARGET_CFLAGS" \
        >/dev/null
)

# The USE_DEV_PTMX fix (header note 1). config.h is autoconf output, so this
# edits it after configure rather than carrying a patch: configure emits the
# line as a commented-out /* #undef USE_DEV_PTMX */, replace it in place if
# present, append otherwise. Fail loudly if neither works -- a silently
# unpatched config.h is exactly the bug that cost a debugging session.
echo "==> forcing USE_DEV_PTMX in dropbear config.h (cross-compile can't probe it)"
DB_CONFIG_H="$DROPBEAR_SRC_DIR/config.h"
if grep -q "^/\* #undef USE_DEV_PTMX \*/" "$DB_CONFIG_H"; then
    sed -i 's|^/\* #undef USE_DEV_PTMX \*/|#define USE_DEV_PTMX 1|' "$DB_CONFIG_H"
elif ! grep -q "^#define USE_DEV_PTMX" "$DB_CONFIG_H"; then
    printf '\n/* forced by tools/build-ssh.sh -- see docs/DEADLETTER-DROPBEAR-PTY.md */\n#define USE_DEV_PTMX 1\n' >> "$DB_CONFIG_H"
fi
if ! grep -q "^#define USE_DEV_PTMX 1" "$DB_CONFIG_H"; then
    echo "tools/build-ssh.sh: failed to set USE_DEV_PTMX in $DB_CONFIG_H" >&2
    exit 1
fi
if grep -q "^#define HAVE_OPENPTY" "$DB_CONFIG_H"; then
    echo "tools/build-ssh.sh: HAVE_OPENPTY got defined -- this uClibc has no openpty," >&2
    echo "so the link would fail (or worse, the PTY path would be wrong)." >&2
    exit 1
fi

# localoptions.h is dropbear's supported "don't edit default_options.h" hook.
# DROPBEAR_SFTPSERVER/SFTPSERVER_PATH already default to exactly these values
# upstream; they are pinned here anyway so a future upstream default flip
# cannot quietly remove sftp support from a rebuilt server, and so grepping
# this repo for the sftp path finds the server side too.
echo "==> writing dropbear localoptions.h (sftp subsystem -> $SFTPSERVER_PATH)"
cat > "$DROPBEAR_SRC_DIR/localoptions.h" <<EOF
/* Generated by tools/build-ssh.sh -- do not edit in the (gitignored) tree. */
#define DROPBEAR_SFTPSERVER 1
#define SFTPSERVER_PATH "$SFTPSERVER_PATH"
EOF

# Single-binary (non-MULTI) build, per header note 2. scp is its own target in
# dropbear's Makefile (it shares no objects with the server), hence listing it
# separately rather than in PROGRAMS.
echo "==> building dropbear + dbclient + dropbearkey + scp"
(
    cd "$DROPBEAR_SRC_DIR"
    make -j"$JOBS" PROGRAMS="dropbear dbclient dropbearkey" >/dev/null
    make -j"$JOBS" scp >/dev/null
)

# ---------------------------------------------------------------------------
# 2. OpenSSH sftp-server
# ---------------------------------------------------------------------------
# Only the sftp-server target is built (`make sftp-server`), not the whole
# suite: sshd/ssh would duplicate dropbear, need OpenSSL, and are far bigger
# than this NAND budget wants. --with-privsep-path etc. are irrelevant to
# sftp-server but configure insists on resolving them, so they are left at
# their defaults deliberately rather than tuned.
#
# --without-openssl needs --disable-security-key too on some versions (the
# sk-* code paths assume libcrypto); passing it unconditionally is harmless.
echo "==> configuring openssh $OPENSSH_VERSION (sftp-server only)"
(
    cd "$OPENSSH_SRC_DIR"
    [ -f Makefile ] && make clean >/dev/null 2>&1
    ./configure \
        --host="$CROSS_HOST" \
        --build="$BUILD_TRIPLET" \
        --prefix=/usr \
        --libexecdir="$(dirname "$SFTPSERVER_PATH")" \
        --without-openssl \
        --without-zlib \
        --without-pam \
        --without-selinux \
        --without-security-key-builtin \
        --disable-security-key \
        --disable-strip \
        --disable-lastlog \
        --disable-utmp --disable-utmpx \
        --disable-wtmp --disable-wtmpx \
        CC="$CC" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
        CFLAGS="$TARGET_CFLAGS" \
        LDFLAGS="-static" \
        >/dev/null
)
echo "==> building sftp-server"
( cd "$OPENSSH_SRC_DIR" && make -j"$JOBS" sftp-server >/dev/null )

# ---------------------------------------------------------------------------
# 3. assemble + verify the device payload
# ---------------------------------------------------------------------------
install_bin() {
    src="$1"; dst="$STAGE_DIR$2"
    if [ ! -f "$src" ]; then
        echo "tools/build-ssh.sh: build finished but $src is missing" >&2
        exit 1
    fi
    cp "$src" "$dst"
    chmod 0755 "$dst"
    "$STRIP" --strip-unneeded "$dst"
}

install_bin "$DROPBEAR_SRC_DIR/dropbear"     /usr/sbin/dropbear
install_bin "$DROPBEAR_SRC_DIR/dbclient"     /usr/bin/dbclient
install_bin "$DROPBEAR_SRC_DIR/dropbearkey"  /usr/bin/dropbearkey
install_bin "$DROPBEAR_SRC_DIR/scp"          /usr/bin/scp
install_bin "$OPENSSH_SRC_DIR/sftp-server"   "$SFTPSERVER_PATH"

# Every binary: right ABI for this board, and genuinely static. "It compiled"
# is not the bar here -- a wrong-ABI or dynamically-linked binary fails at
# exec() time on the device with busybox ash's generic ENOENT message, which
# reads like a typo rather than a build error (tools/build-alsa.sh header).
echo "==> verifying staged binaries"
for rel in /usr/sbin/dropbear /usr/bin/dbclient /usr/bin/dropbearkey \
           /usr/bin/scp "$SFTPSERVER_PATH"; do
    bin="$STAGE_DIR$rel"
    flags="$("$READELF" -h "$bin" | sed -n 's/^ *Flags: *//p')"
    case "$flags" in
        0x5000200*) : ;;
        *)
            echo "tools/build-ssh.sh: $rel has unexpected ELF Flags: $flags" >&2
            echo "  want 0x5000200 (Version5 EABI, soft-float ABI) as on every other" >&2
            echo "  binary this project ships." >&2
            exit 1
            ;;
    esac
    # The NSS trap (see header): a static glibc binary still dlopen()s
    # libnss_*.so for getpwnam/getpwuid, and this rootfs has none. The
    # tell-tale is glibc's "libnss_%s.so.%d" template string.
    if strings -a "$bin" | grep -q "libnss_"; then
        echo "tools/build-ssh.sh: $rel contains glibc NSS strings -- it was built" >&2
        echo "  against glibc, not uClibc. It would link fine and then fail on the" >&2
        echo "  device at getpwnam/getpwuid time (no libnss_files.so.2 there)." >&2
        echo "  Build with this project's uClibc toolchain (tools/build-uclibc-toolchain.sh)." >&2
        exit 1
    fi
    if "$READELF" -d "$bin" 2>/dev/null | grep -q NEEDED; then
        echo "tools/build-ssh.sh: $rel is dynamically linked (has NEEDED entries)." >&2
        "$READELF" -d "$bin" | awk '/NEEDED/{print "    " $NF}' >&2
        echo "  These must be static -- they are the recovery path when the dynamic" >&2
        echo "  loader itself is what's broken. See this script's header." >&2
        exit 1
    fi
    printf '    %-28s %8s bytes  Flags: %s  static\n' \
        "$rel" "$(wc -c < "$bin")" "$flags"
done

# Content checks, not just link checks: these two strings are the actual
# features this build exists to guarantee.
if ! strings -a "$STAGE_DIR/usr/sbin/dropbear" | grep -qF "$SFTPSERVER_PATH"; then
    echo "tools/build-ssh.sh: the built dropbear has no '$SFTPSERVER_PATH' string --" >&2
    echo "  DROPBEAR_SFTPSERVER did not take effect, so 'sftp zaurus' would still fail." >&2
    exit 1
fi
if ! strings -a "$STAGE_DIR/usr/sbin/dropbear" | grep -q "/dev/ptmx"; then
    echo "tools/build-ssh.sh: the built dropbear has no /dev/ptmx string -- USE_DEV_PTMX" >&2
    echo "  did not take effect, so interactive SSH would fail with 'PTY allocation" >&2
    echo "  request failed'. See docs/DEADLETTER-DROPBEAR-PTY.md." >&2
    exit 1
fi
if strings -a "$STAGE_DIR/usr/sbin/dropbear" | grep -q "Failed to open any /dev/pty"; then
    echo "tools/build-ssh.sh: the built dropbear still contains the legacy BSD /dev/ptyXX" >&2
    echo "  fallback path -- config.h patch did not fully apply." >&2
    exit 1
fi
echo "    dropbear: sftp subsystem -> $SFTPSERVER_PATH, PTY via /dev/ptmx"

echo ""
echo "==> done: $STAGE_DIR assembled"
echo "    $STAGE_DIR/usr/bin/scp"
echo "    $STAGE_DIR$SFTPSERVER_PATH"
echo "    $STAGE_DIR/usr/bin/dbclient, dropbearkey"
echo "    $STAGE_DIR/usr/sbin/dropbear   (NOT deployed unless --replace-dropbear)"
echo ""
echo "    Deploy with tools/chunked-deploy.sh (or tools/build-and-deploy.sh)."
echo "    CI picks it up via flash/build-mtd3-jffs2.sh and"
echo "    flash/build-update-package.sh."
