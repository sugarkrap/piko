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
trap 'rm -f "$LOCK"' EXIT INT TERM

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
