#!/bin/sh
# Host-side helper: deploy and run in-system SMF update on stage2 over ssh.
#
# This uses the plain ARM binary writer path on-device:
#   /usr/sbin/pico-smf-write + /tmp/picoupdate.sh + /tmp/zImage.smf
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
LOCAL_WRITER="$SCRIPT_DIR/pico-smf-write"
LOCAL_PICOUP="$SCRIPT_DIR/picoupdate.sh"

# Prefer the last known size-safe D+B-test payload when available.
if [ -f "$SCRIPT_DIR/zImage-kexecboot-test" ]; then
    LOCAL_IMAGE="$SCRIPT_DIR/zImage-kexecboot-test"
fi

if [ -n "$IMAGE_OVERRIDE" ]; then
    LOCAL_IMAGE="$IMAGE_OVERRIDE"
fi

REMOTE_IMAGE="/tmp/zImage.smf"
REMOTE_PICOUP="/tmp/picoupdate.sh"
REMOTE_WRITER="/usr/sbin/pico-smf-write"

need_file() {
    if [ ! -f "$1" ]; then
        echo "error: missing file: $1" >&2
        exit 1
    fi
}

need_file "$LOCAL_IMAGE"
need_file "$LOCAL_WRITER"
need_file "$LOCAL_PICOUP"

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
cat "$LOCAL_PICOUP" | ssh $SSH_OPTS "$TARGET" "cat > '$REMOTE_PICOUP'"
cat "$LOCAL_WRITER" | ssh $SSH_OPTS "$TARGET" "cat > '$REMOTE_WRITER'"

ssh $SSH_OPTS "$TARGET" "chmod 0755 '$REMOTE_PICOUP' '$REMOTE_WRITER' && ls -l '$REMOTE_IMAGE' '$REMOTE_PICOUP' '$REMOTE_WRITER'"

# Always run dry-run first so geometry and limits are printed.
ssh $SSH_OPTS "$TARGET" "$REMOTE_PICOUP --dry-run '$REMOTE_IMAGE'"

if [ "$MODE" = "--apply" ]; then
    ssh $SSH_OPTS "$TARGET" "$REMOTE_PICOUP '$REMOTE_IMAGE'"
    ssh $SSH_OPTS "$TARGET" "sync; sync"
    echo "[stage2-smf] apply completed"
else
    echo "[stage2-smf] dry-run completed"
    echo "[stage2-smf] re-run with --apply to perform flash"
fi
