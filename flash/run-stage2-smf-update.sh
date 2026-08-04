#!/bin/sh
# Host-side helper: deploy and run an in-system SMF write on stage2 over ssh.
#
# This is the developer/recovery path, for when you have the device on WiFi
# and want to push a bootstrap kernel straight at it. It is NOT how the
# device updates itself -- that is `piko-update` + `smfcommit`, which
# carries the bootstrap inside the update package and defers the NAND write
# until after a proven reboot (docs/HOWTO-OFFLINE-UPDATE.md). Use this one
# when you are iterating on the bootstrap and don't want to build a package
# for every attempt.
#
# On-device it uses the plain ARM binary writer directly:
#   /usr/sbin/piko-smf-write + /tmp/zImage.smf
#
# The old /tmp/picoupdate.sh wrapper this used to push is gone: its job
# (find the smf partition, pick the writer, print the geometry) now lives
# in piko-smf-write and piko-update. Its Cacko /sbin/nandlogical fallback
# went with it -- that was for flashing from under the Cacko ROM, and this
# script only ever talks to a running stage-2, which has piko-smf-write.
#
# Usage:
#   run-stage2-smf-update.sh [--apply] [--image /path/to/zImage] [root@host]
#
# Default is --dry-run.

set -eu

MODE="--dry-run"
TARGET="root@10.43.112.72"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/zaurus_ed25519}"
SSH_OPTS="-i $SSH_KEY -o StrictHostKeyChecking=accept-new"
IMAGE_OVERRIDE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --apply)
            MODE="--apply"
            ;;
        --image)
            shift
            if [ -z "${1:-}" ]; then
                echo "error: --image requires a file path" >&2
                exit 1
            fi
            IMAGE_OVERRIDE="$1"
            ;;
        *)
            TARGET="$1"
            ;;
    esac
    shift
done

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LOCAL_IMAGE="$SCRIPT_DIR/zImage"
LOCAL_WRITER="$SCRIPT_DIR/piko-smf-write"

# Prefer the last known size-safe D+B-test payload when available.
if [ -f "$SCRIPT_DIR/zImage-kexecboot-test" ]; then
    LOCAL_IMAGE="$SCRIPT_DIR/zImage-kexecboot-test"
fi

if [ -n "$IMAGE_OVERRIDE" ]; then
    LOCAL_IMAGE="$IMAGE_OVERRIDE"
fi

REMOTE_IMAGE="/tmp/zImage.smf"
REMOTE_WRITER="/usr/sbin/piko-smf-write"

# Logical geometry of the smf kernel slot. Fixed by the Sharp bootloader,
# not ours to pick -- see AGENTS.md and docs/DEADLETTER-MTD1-OFFSET.md.
SMF_LADDR=917504
SMF_MAX=1294336

need_file() {
    if [ ! -f "$1" ]; then
        echo "error: missing file: $1" >&2
        exit 1
    fi
}

need_file "$LOCAL_IMAGE"
need_file "$LOCAL_WRITER"

writer_file_info=$(file "$LOCAL_WRITER" 2>/dev/null || true)
case "$writer_file_info" in
    *"ARM"*|*"arm"*)
        ;;
    *)
        echo "error: $LOCAL_WRITER is not an ARM executable" >&2
        echo "       file says: $writer_file_info" >&2
        echo "       refusing to deploy writer to target" >&2
        exit 1
        ;;
esac

echo "[stage2-smf] target: $TARGET"
echo "[stage2-smf] mode: $MODE"
echo "[stage2-smf] image: $LOCAL_IMAGE"

if [ "$MODE" = "--apply" ]; then
    echo "[stage2-smf] WARNING: this will write SMF on target"
fi

# Quick connectivity check.
ssh $SSH_OPTS "$TARGET" "echo ONLINE" >/dev/null

# Stream files over ssh to avoid scp/sftp dependency on target.
cat "$LOCAL_IMAGE" | ssh $SSH_OPTS "$TARGET" "cat > '$REMOTE_IMAGE'"
cat "$LOCAL_WRITER" | ssh $SSH_OPTS "$TARGET" "cat > '$REMOTE_WRITER'"

ssh $SSH_OPTS "$TARGET" "chmod 0755 '$REMOTE_WRITER' && ls -l '$REMOTE_IMAGE' '$REMOTE_WRITER'"

# Resolve the smf partition by name, never by number: this stage-2 kernel
# calls it mtd0 while the Cacko/recovery kernel calls the same partition
# mtd1 (AGENTS.md).
#
# Parsed on the host, not on the target: the target only has to `cat`. The
# stage-2 rootfs builds busybox without `cut` (or `awk`), so the obvious
# remote pipeline dies with "ash: cut: not found" and leaves SMF_DEV empty
# -- which then looks exactly like "no smf partition", i.e. a scary
# hardware-shaped error for a missing applet. Do the text work at this end,
# where the toolset is known.
SMF_DEV=$(ssh $SSH_OPTS "$TARGET" "cat /proc/mtd" \
    | grep '"smf"' | head -n 1 | sed 's#:.*##; s#^#/dev/#')
if [ -z "$SMF_DEV" ]; then
    echo "error: no \"smf\" partition found in /proc/mtd on $TARGET" >&2
    exit 1
fi
echo "[stage2-smf] smf partition: $SMF_DEV"

# --compare is read-only and tells us whether a write is even needed.
# Exit 0 = already identical, 3 = differs, anything else = couldn't tell.
set +e
ssh $SSH_OPTS "$TARGET" \
    "$REMOTE_WRITER --compare '$SMF_DEV' '$REMOTE_IMAGE' $SMF_LADDR $SMF_MAX"
cmp_rc=$?
set -e

case "$cmp_rc" in
    0)
        echo "[stage2-smf] target already holds this exact image -- nothing to do"
        exit 0
        ;;
    3)
        echo "[stage2-smf] target differs from this image -- a write is needed"
        ;;
    *)
        echo "[stage2-smf] WARNING: could not compare (exit $cmp_rc)" >&2
        if [ "$MODE" = "--apply" ]; then
            echo "[stage2-smf] refusing to --apply without a readable comparison" >&2
            exit 1
        fi
        ;;
esac

if [ "$MODE" = "--apply" ]; then
    # Pass a backup path so the writer dumps AND verifies the partition
    # before it erases anything.
    ssh $SSH_OPTS "$TARGET" \
        "$REMOTE_WRITER '$SMF_DEV' '$REMOTE_IMAGE' $SMF_LADDR $SMF_MAX /boot/smf-backup-prewrite.bin"
    ssh $SSH_OPTS "$TARGET" "sync; sync"
    echo "[stage2-smf] apply completed (writer verified its own readback)"
    echo "[stage2-smf] the new bootstrap takes effect at the next COLD boot"
else
    echo "[stage2-smf] dry-run completed"
    echo "[stage2-smf] re-run with --apply to perform flash"
fi
