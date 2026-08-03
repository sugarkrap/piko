#!/bin/sh
set -eu

# Robust wrapper around tools/build-uclibc-toolchain.sh for building this
# project's cross-toolchain directly on a dev machine (not CI).
#
# Two problems showed up building this by hand that CI never hits:
#
# 1. THE MACHINE SLEEPS MID-BUILD. A crosstool-NG build is tens of
#    minutes of unattended CPU time; a laptop that suspends on idle (or
#    when the lid closes) silently pauses it. On resume, the build's own
#    process can come back wedged or simply be gone -- the failure looks
#    like a hang or a crash with no error message, and hours of wall time
#    buy nothing. `systemd-inhibit` blocks sleep/idle/lid-switch for
#    exactly the lifetime of this script, released automatically on exit
#    (success, failure, or being killed) -- no separate cleanup step to
#    forget.
#
# 2. TWO SESSIONS BUILDING THE SAME toolchain/ AT ONCE. This project has
#    had more than one dev machine session working from the same checkout
#    at a time, and tools/build-uclibc-toolchain.sh does `rm -rf` on its
#    build directory before every run -- a second invocation stomps the
#    first one's progress instead of erroring out. A simple PID lockfile
#    makes the second caller fail fast with a clear message instead of
#    racing silently.
#
# 3. THE HOST'S OWN gcc IS TOO NEW. crosstool-NG's first stage builds a
#    "core" cross-gcc using the BUILD machine's plain `gcc`/`g++` on PATH
#    -- it does not isolate itself from the host toolchain the way the
#    final cross-compiler is isolated from the target's. On a
#    rolling-release box that plain `gcc` can be far newer than GCC
#    13.4.0's own build scripts were ever tested against; concretely,
#    GCC 16's reworked libstdc++ <bits/locale_facets.h> conflicts with
#    GCC 13's fixincludes/build process (toupper/isgraph/etc. macro vs.
#    template redeclaration errors), and the "core C gcc compiler" stage
#    dies. CI never hits this because GitHub's runner image ships a much
#    older stock gcc. The fix is not a crosstool-NG option -- it is
#    simply "don't let the too-new gcc be the one found on PATH": if an
#    older versioned gcc package (gcc14, gcc13, ...) is installed
#    alongside the system one, a tiny shim directory of cc/gcc/c++/g++/
#    cpp symlinks pointing at it is prepended to PATH for the build only.
#
# Usage:
#   tools/build-toolchain.sh [--force]
#
# --force is passed straight through to build-uclibc-toolchain.sh (rebuild
# even if a working compiler is already present).
#
# Deliberately does NOT duplicate any crosstool-NG/config logic -- that
# all still lives in build-uclibc-toolchain.sh so there is exactly one
# place that knows how to build this toolchain. This script only adds the
# two things above around it.

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TOOLCHAIN_DIR="${PIKO_TOOLCHAIN_DIR:-$REPO/toolchain}"
LOCK="$TOOLCHAIN_DIR/.build-toolchain.lock"
LOG="${BUILD_TOOLCHAIN_LOG:-$TOOLCHAIN_DIR/build-toolchain.log}"

mkdir -p "$TOOLCHAIN_DIR"

# Stale lock from a session that died without cleaning up (SIGKILL, a crash,
# the machine itself losing power) is taken over rather than left stuck
# forever -- same reasoning as matchbox-fbrun's own lock in this repo.
if [ -f "$LOCK" ]; then
    owner="$(cat "$LOCK" 2>/dev/null || true)"
    if [ -n "$owner" ] && kill -0 "$owner" 2>/dev/null; then
        echo "FAILED: another build-toolchain.sh is already running (pid $owner, lock: $LOCK)" >&2
        echo "        If that is stale (process really gone), remove the lock and retry." >&2
        exit 1
    fi
    echo "==> stale lock from pid ${owner:-unknown}, taking it over"
fi
echo "$$" > "$LOCK"
SHIM_DIR="$TOOLCHAIN_DIR/.build-host-cc-shim"
trap 'rm -f "$LOCK"; rm -rf "$SHIM_DIR"' EXIT INT TERM

# Prefer the newest versioned gcc package that is NOT the system default --
# gcc14 first (known-good against this project's crosstool-NG/GCC-13.4.0
# combination), falling back through older ones if that is not installed.
# HOST_CC_VERSION overrides the search entirely for whoever hits a
# different bad combination later.
if [ -n "${HOST_CC_VERSION:-}" ]; then
    try_versions="$HOST_CC_VERSION"
else
    try_versions="14 13 12 11"
fi

# crosstool-NG's own bootstrap does NOT just look for bare `gcc`/`g++` --
# it generates its internal "buildtools" wrapper by preferring the
# build-machine-triplet-prefixed name (autoconf convention: on this Arch
# box, `x86_64-pc-linux-gnu-gcc`), which exists here as its own symlink to
# the same too-new system compiler and is found before a bare `gcc` shim
# ever comes into play. Both forms have to be shimmed, or the triplet one
# silently wins and the whole point of this is defeated -- which is
# exactly what happened the first time this was written.
MACHINE="$(gcc -dumpmachine 2>/dev/null || echo unknown)"

rm -rf "$SHIM_DIR"
original_gcc_version="$(gcc --version 2>/dev/null | head -1 || echo unknown)"
for v in $try_versions; do
    if command -v "gcc-$v" >/dev/null 2>&1 && command -v "g++-$v" >/dev/null 2>&1; then
        mkdir -p "$SHIM_DIR"
        for tool in cc:gcc gcc:gcc c++:g++ g++:g++ cpp:cpp \
                    "$MACHINE-gcc:gcc" "$MACHINE-g++:g++"; do
            link="${tool%%:*}"
            real="${tool#*:}-$v"
            # Prefer the triplet-versioned binary itself if it exists
            # (e.g. x86_64-pc-linux-gnu-gcc-14); otherwise the plain
            # versioned one works identically under any invoked name.
            target="$(command -v "$MACHINE-$real" || command -v "$real")"
            ln -sf "$target" "$SHIM_DIR/$link"
        done
        echo "==> host gcc is $original_gcc_version, shimming gcc-$v ($("$SHIM_DIR/gcc" --version | head -1)) onto PATH for the build (bare and $MACHINE-prefixed)"
        PATH="$SHIM_DIR:$PATH"
        export PATH
        break
    fi
done
if [ ! -d "$SHIM_DIR" ]; then
    echo "==> no older gcc[0-9]+ package found to shim (tried: $try_versions)" >&2
    echo "    building with the system gcc ($(gcc --version | head -1)) as-is -- if it is" >&2
    echo "    too new for crosstool-NG's core-gcc stage, install e.g. gcc14 first." >&2
fi

if ! command -v systemd-inhibit >/dev/null 2>&1; then
    echo "==> systemd-inhibit not found -- proceeding WITHOUT a sleep inhibitor" >&2
    echo "    (disable suspend/idle yourself for the duration, or this can silently stall)" >&2
    exec "$REPO/tools/build-uclibc-toolchain.sh" "$@" > "$LOG" 2>&1
fi

echo "==> building with systemd-inhibit held (sleep/idle/lid-switch blocked)"
echo "==> log: $LOG"
exec systemd-inhibit \
    --what=sleep:idle:handle-lid-switch \
    --why="piko cross-toolchain build (tools/build-toolchain.sh)" \
    --mode=block \
    -- "$REPO/tools/build-uclibc-toolchain.sh" "$@" > "$LOG" 2>&1
