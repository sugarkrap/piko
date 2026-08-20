#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SRC_DIR="$REPO/userspace/src"

DROPBEAR_VERSION="${DROPBEAR_VERSION:-2025.88}"
DROPBEAR_SRC_DIR="$SRC_DIR/dropbear-$DROPBEAR_VERSION"
DROPBEAR_TARBALL="$SRC_DIR/dropbear-$DROPBEAR_VERSION.tar.bz2"
DROPBEAR_URL="https://matt.ucc.asn.au/dropbear/releases/dropbear-$DROPBEAR_VERSION.tar.bz2"
DROPBEAR_SHA256="9d1c65e3f4b3f03da8390a119aebe6b14d9209b5fad0286291dabacd530dd3a3"

OPENSSH_VERSION="${OPENSSH_VERSION:-10.4p1}"
OPENSSH_SRC_DIR="$SRC_DIR/openssh-$OPENSSH_VERSION"
OPENSSH_TARBALL="$SRC_DIR/openssh-$OPENSSH_VERSION.tar.gz"
OPENSSH_URL="https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-$OPENSSH_VERSION.tar.gz"
OPENSSH_SHA256="8cd8b6e0cadc0e5c5227f72038512bdc00e64fc6250ac9024c94a31afa3869d9"

SFTPSERVER_PATH="${SFTPSERVER_PATH:-/usr/libexec/sftp-server}"

STAGE_DIR="${STAGE_DIR:-$REPO/userspace/stage-ssh}"
TOOLCHAIN_BIN_DIR="${TOOLCHAIN_BIN_DIR:-$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin}"
CROSS_COMPILE="${CROSS_COMPILE:-arm-unknown-linux-uclibcgnueabi-}"
CROSS_HOST="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
JOBS="${JOBS:-$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)}"
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
            echo "tools/userspace/build-ssh.sh: unknown argument: $arg" >&2
            echo "Usage: tools/userspace/build-ssh.sh [--force]" >&2
            exit 1
            ;;
    esac
done
PIKO_STAMP="$STAGE_DIR/.piko-stamp"
PIKO_STATE="$(sha256sum "$0" | cut -d' ' -f1) $DROPBEAR_VERSION $DROPBEAR_SHA256 $OPENSSH_VERSION $OPENSSH_SHA256"
if [ "$FORCE" -eq 0 ] && [ -f "$PIKO_STAMP" ] \
   && [ "$(cat "$PIKO_STAMP")" = "$PIKO_STATE" ] && [ -f "$STAGE_DIR/usr/sbin/dropbear" ]; then
    echo "==> dropbear/openssh already staged for these inputs, skipping (--force to rebuild)"
    exit 0
fi


mkdir -p "$SRC_DIR"

if [ -n "$TOOLCHAIN_BIN_DIR" ] && [ -d "$TOOLCHAIN_BIN_DIR" ]; then
    PATH="$TOOLCHAIN_BIN_DIR:$PATH"
fi
if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    echo "tools/userspace/build-ssh.sh: ${CROSS_COMPILE}gcc not found in PATH." >&2
    echo "Set TOOLCHAIN_BIN_DIR to your toolchain bin path, or export CROSS_COMPILE explicitly." >&2
    echo "A fresh machine builds it with tools/toolchain/build-uclibc-toolchain.sh." >&2
    exit 1
fi
CC="${CROSS_COMPILE}gcc"
AR="${CROSS_COMPILE}ar"
RANLIB="${CROSS_COMPILE}ranlib"
STRIP="${CROSS_COMPILE}strip"
READELF="${CROSS_COMPILE}readelf"
BUILD_TRIPLET="$(uname -m)-pc-linux-gnu"

SYSCALL_TEST_DIR="$(mktemp -d)"
trap 'rm -rf "$SYSCALL_TEST_DIR"' EXIT INT TERM
cat > "$SYSCALL_TEST_DIR/t.c" <<'EOF'
#include <unistd.h>
#include <sys/syscall.h>
int main(void) { return (int)syscall(SYS_getpid); }
EOF
if ! "$CC" -o "$SYSCALL_TEST_DIR/t" "$SYSCALL_TEST_DIR/t.c" >"$SYSCALL_TEST_DIR/log" 2>&1; then
    echo "tools/userspace/build-ssh.sh: this toolchain's libc has no syscall()." >&2
    echo "dropbear's dbutil.c needs it -- a previous toolchain required a" >&2
    echo "syscall_shim.o workaround." >&2
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
        curl -fL --http1.1 -o "$tarball.partial" "$url"
        mv "$tarball.partial" "$tarball"
    else
        echo "==> reusing cached $tarball"
    fi

    echo "==> verifying sha256 of $(basename "$tarball")"
    actual="$(sha256sum "$tarball" | cut -d' ' -f1)"
    if [ "$actual" != "$want_sha" ]; then
        echo "tools/userspace/build-ssh.sh: SHA-256 mismatch for $tarball" >&2
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
        echo "tools/userspace/build-ssh.sh: $srcdir doesn't look like a configure-based tree" >&2
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

echo "==> forcing USE_DEV_PTMX in dropbear config.h (cross-compile can't probe it)"
DB_CONFIG_H="$DROPBEAR_SRC_DIR/config.h"
if grep -q "^/\* #undef USE_DEV_PTMX \*/" "$DB_CONFIG_H"; then
    sed -i 's|^/\* #undef USE_DEV_PTMX \*/|#define USE_DEV_PTMX 1|' "$DB_CONFIG_H"
elif ! grep -q "^#define USE_DEV_PTMX" "$DB_CONFIG_H"; then
    printf '\n/* forced by tools/userspace/build-ssh.sh */\n#define USE_DEV_PTMX 1\n' >> "$DB_CONFIG_H"
fi
if ! grep -q "^#define USE_DEV_PTMX 1" "$DB_CONFIG_H"; then
    echo "tools/userspace/build-ssh.sh: failed to set USE_DEV_PTMX in $DB_CONFIG_H" >&2
    exit 1
fi
if grep -q "^#define HAVE_OPENPTY" "$DB_CONFIG_H"; then
    echo "tools/userspace/build-ssh.sh: HAVE_OPENPTY got defined -- this uClibc has no openpty," >&2
    echo "so the link would fail (or worse, the PTY path would be wrong)." >&2
    exit 1
fi

echo "==> writing dropbear localoptions.h (sftp subsystem -> $SFTPSERVER_PATH)"
cat > "$DROPBEAR_SRC_DIR/localoptions.h" <<EOF
/* Generated by tools/userspace/build-ssh.sh -- do not edit in the (gitignored) tree. */
#define DROPBEAR_SFTPSERVER 1
#define SFTPSERVER_PATH "$SFTPSERVER_PATH"
EOF

echo "==> building dropbear + dbclient + dropbearkey + scp"
(
    cd "$DROPBEAR_SRC_DIR"
    make -j"$JOBS" PROGRAMS="dropbear dbclient dropbearkey" >/dev/null
    make -j"$JOBS" scp >/dev/null
)

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

install_bin() {
    src="$1"; dst="$STAGE_DIR$2"
    if [ ! -f "$src" ]; then
        echo "tools/userspace/build-ssh.sh: build finished but $src is missing" >&2
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

echo "==> verifying staged binaries"
for rel in /usr/sbin/dropbear /usr/bin/dbclient /usr/bin/dropbearkey \
           /usr/bin/scp "$SFTPSERVER_PATH"; do
    bin="$STAGE_DIR$rel"
    flags="$("$READELF" -h "$bin" | sed -n 's/^ *Flags: *//p')"
    case "$flags" in
        0x5000200*) : ;;
        *)
            echo "tools/userspace/build-ssh.sh: $rel has unexpected ELF Flags: $flags" >&2
            echo "  want 0x5000200 (Version5 EABI, soft-float ABI) as on every other" >&2
            echo "  binary this project ships." >&2
            exit 1
            ;;
    esac
    if strings -a "$bin" | grep -q "libnss_"; then
        echo "tools/userspace/build-ssh.sh: $rel contains glibc NSS strings -- it was built" >&2
        echo "  against glibc, not uClibc. It would link fine and then fail on the" >&2
        echo "  device at getpwnam/getpwuid time (no libnss_files.so.2 there)." >&2
        echo "  Build with this project's uClibc toolchain (tools/toolchain/build-uclibc-toolchain.sh)." >&2
        exit 1
    fi
    if "$READELF" -d "$bin" 2>/dev/null | grep -q NEEDED; then
        echo "tools/userspace/build-ssh.sh: $rel is dynamically linked (has NEEDED entries)." >&2
        "$READELF" -d "$bin" | awk '/NEEDED/{print "    " $NF}' >&2
        echo "  These must be static -- they are the recovery path when the dynamic" >&2
        echo "  loader itself is what's broken. See this script's header." >&2
        exit 1
    fi
    printf '    %-28s %8s bytes  Flags: %s  static\n' \
        "$rel" "$(wc -c < "$bin")" "$flags"
done

if ! strings -a "$STAGE_DIR/usr/sbin/dropbear" | grep -qF "$SFTPSERVER_PATH"; then
    echo "tools/userspace/build-ssh.sh: the built dropbear has no '$SFTPSERVER_PATH' string --" >&2
    echo "  DROPBEAR_SFTPSERVER did not take effect, so 'sftp zaurus' would still fail." >&2
    exit 1
fi
if ! strings -a "$STAGE_DIR/usr/sbin/dropbear" | grep -q "/dev/ptmx"; then
    echo "tools/userspace/build-ssh.sh: the built dropbear has no /dev/ptmx string -- USE_DEV_PTMX" >&2
    echo "  did not take effect, so interactive SSH would fail with 'PTY allocation" >&2
    echo "  request failed'." >&2
    exit 1
fi
if strings -a "$STAGE_DIR/usr/sbin/dropbear" | grep -q "Failed to open any /dev/pty"; then
    echo "tools/userspace/build-ssh.sh: the built dropbear still contains the legacy BSD /dev/ptyXX" >&2
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
echo "    Also picked up by tools/build-rootfs.sh."

mkdir -p "$STAGE_DIR"
printf '%s\n' "$PIKO_STATE" > "$PIKO_STAMP"
