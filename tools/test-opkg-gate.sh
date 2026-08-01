#!/bin/sh
set -eu

# Checks that the device's opkg accepts packages built for this system and
# refuses Sharp-era ones -- on the build host, by running the real ARM
# binary under qemu-arm.
#
# Usage:
#   tools/test-opkg-gate.sh
#
# Requires: qemu-arm (Debian/Ubuntu: qemu-user; Arch: qemu-user-static),
# and opkg already staged by tools/build-opkg.sh.
#
#
# WHY THIS TEST EXISTS
# ====================
# The retro-compatibility gate is not code -- it is the absence of one
# line. /etc/opkg/opkg.conf lists the acceptable architectures, and it
# deliberately does not list "arm". Delete those lines, or build opkg
# without --sysconfdir=/etc so it never finds the file, and opkg does not
# fail closed: it falls back to a built-in list of {all, noarch,
# HOST_CPU_STR}, where HOST_CPU_STR is the configure --host CPU -- the
# literal string "arm". Sharp packages then install cleanly and silently.
#
# So the thing that keeps them out is invisible in a diff and produces no
# error when it breaks. Case 3 below is the one that matters: it removes
# the arch lines on purpose and asserts that the package WOULD have been
# accepted, which is what proves the other cases are testing something
# real rather than passing for an unrelated reason.

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OPKG="$REPO/userspace/stage-target/usr/bin/opkg"
CONF="$REPO/rootfs/etc/opkg/opkg.conf"

command -v qemu-arm >/dev/null 2>&1 || {
    echo "SKIP: qemu-arm not installed -- cannot run the ARM binary here." >&2
    exit 0
}
[ -x "$OPKG" ] || {
    echo "FAILED: $OPKG not built. Run tools/build-opkg.sh first." >&2
    exit 1
}
command -v ar >/dev/null 2>&1 || { echo "FAILED: 'ar' not found" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fails=0

# make_ipk NAME ARCH STYLE OUT
#   ARCH  -- architecture to declare, or "" to omit the field entirely
#            (plenty of Sharp-era packages have no Architecture at all)
#   STYLE -- ar  = modern layout
#            tar = the pre-2005 layout, a plain tar.gz of the same members,
#                  which is what most original Zaurus .ipk files are
make_ipk() {
    _n="$1"; _a="$2"; _s="$3"; _o="$4"
    _b="$WORK/build"
    rm -rf "$_b"; mkdir -p "$_b/CONTROL" "$_b/usr/bin"
    echo "#!/bin/sh" > "$_b/usr/bin/$_n"; chmod 755 "$_b/usr/bin/$_n"
    {
        echo "Package: $_n"
        echo "Version: 1.0"
        [ -n "$_a" ] && echo "Architecture: $_a"
        echo "Maintainer: test"
        echo "Description: gate test"
    } > "$_b/CONTROL/control"
    ( cd "$_b/CONTROL" && tar -czf "$WORK/control.tar.gz" ./control )
    ( cd "$_b"         && tar -czf "$WORK/data.tar.gz" ./usr )
    echo "2.0" > "$WORK/debian-binary"
    rm -f "$_o"
    if [ "$_s" = ar ]; then
        ( cd "$WORK" && ar rc "$_o" debian-binary control.tar.gz data.tar.gz )
    else
        ( cd "$WORK" && tar -czf "$_o" ./debian-binary ./control.tar.gz ./data.tar.gz )
    fi
    rm -f "$WORK/control.tar.gz" "$WORK/data.tar.gz" "$WORK/debian-binary"
}

# fresh_root CONFFILE -> an offline root preloaded with a config
fresh_root() {
    _r="$WORK/root"
    rm -rf "$_r"; mkdir -p "$_r/etc/opkg"
    cp "$1" "$_r/etc/opkg/opkg.conf"
    echo "$_r"
}

# installs ROOT IPK -> 0 if the package ended up installed
installs() {
    qemu-arm "$OPKG" -o "$1" --conf "$1/etc/opkg/opkg.conf" install "$2" \
        >"$WORK/out.log" 2>&1 || true
    qemu-arm "$OPKG" -o "$1" --conf "$1/etc/opkg/opkg.conf" list-installed \
        2>/dev/null | grep -q .
}

check() { # DESCRIPTION EXPECTED ACTUAL
    if [ "$2" = "$3" ]; then
        echo "  PASS  $1"
    else
        echo "  FAIL  $1 (expected $2, got $3)"
        echo "        --- opkg said ---"
        sed 's/^/        /' "$WORK/out.log"
        fails=$((fails + 1))
    fi
}

echo "==> opkg architecture gate"
echo "    binary: $OPKG"
echo "    config: $CONF"
echo

# --- 1. a package built for this system, modern format -> ACCEPTED ----
make_ipk modernapp piko ar "$WORK/modern.ipk"
R="$(fresh_root "$CONF")"
if installs "$R" "$WORK/modern.ipk"; then r=installed; else r=refused; fi
check "piko package installs" installed "$r"

# --- 2. Sharp-era packages -> REFUSED --------------------------------
make_ipk sharparm arm tar "$WORK/sharp-arm.ipk"
R="$(fresh_root "$CONF")"
if installs "$R" "$WORK/sharp-arm.ipk"; then r=installed; else r=refused; fi
check "Sharp 'arm' package refused" refused "$r"

# No Architecture field at all. Worth testing separately: opkg's
# pkg_arch_supported() returns TRUE for a NULL architecture, so it is only
# the earlier solver check that keeps these out.
make_ipk noarch "" tar "$WORK/sharp-noarch.ipk"
R="$(fresh_root "$CONF")"
if installs "$R" "$WORK/sharp-noarch.ipk"; then r=installed; else r=refused; fi
check "package with no Architecture refused" refused "$r"

# --- 3. the gate is really the config, not luck ----------------------
# Strip the arch lines and confirm the SAME Sharp package sails through.
# If this ever reports "refused", something else is doing the rejecting
# and cases 2's guarantee is not what this file claims it is.
grep -v '^arch ' "$CONF" > "$WORK/noarch.conf"
R="$(fresh_root "$WORK/noarch.conf")"
if installs "$R" "$WORK/sharp-arm.ipk"; then r=installed; else r=refused; fi
check "without arch lines, Sharp package WOULD install" installed "$r"

echo
if [ "$fails" -eq 0 ]; then
    echo "==> all checks passed"
    exit 0
fi
echo "==> $fails check(s) FAILED"
exit 1
