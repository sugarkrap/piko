#!/bin/sh
set -eu

# Stage shareware Quake data to the SD card on a live Zaurus.
# Usage:
#   flash/stage-quake-data.sh [root@ip] [path/to/pak0.pak]
# Example:
#   flash/stage-quake-data.sh root@10.43.112.72 \
#       /home/makaron/Code/zaurus-refresh/.cache/quake-data/id1/pak0.pak

TARGET="${1:-root@10.43.112.72}"
PAK="${2:-/home/makaron/Code/zaurus-refresh/.cache/quake-data/id1/pak0.pak}"
KEY="${HOME}/.ssh/zaurus_ed25519"
SSH_OPTS="-i $KEY -o BatchMode=yes -o ConnectTimeout=8 -o ServerAliveInterval=5 -o ServerAliveCountMax=12 -o StrictHostKeyChecking=accept-new"
CHUNK_SIZE=4194304
CHUNK_TIMEOUT=90
STALL_RETRIES=20

if [ ! -f "$PAK" ]; then
    echo "missing pak: $PAK" >&2
    exit 1
fi

MD5_LOCAL="$(md5sum "$PAK" | awk '{print $1}')"
SIZE_LOCAL="$(wc -c < "$PAK")"

echo "host pak0.pak size: $SIZE_LOCAL"
echo "host pak0.pak md5:  $MD5_LOCAL"

ssh $SSH_OPTS "$TARGET" 'set -eu; mkdir -p /mnt/card/id1'

mount_line="$(ssh $SSH_OPTS "$TARGET" 'mount | grep "on /mnt/card " || true')"
if [ -z "$mount_line" ]; then
    echo "error: /mnt/card is not mounted on target" >&2
    exit 1
fi

case "$mount_line" in
    *"(rw,"*|*",rw,"*)
        ;;
    *)
        echo "error: /mnt/card is mounted read-only on target" >&2
        echo "       $mount_line" >&2
        exit 1
        ;;
esac

need_kb=$(( (SIZE_LOCAL + 1023) / 1024 ))
avail_kb="$(ssh $SSH_OPTS "$TARGET" 'df /mnt/card | tail -n 1 | awk "{print \$4}"' | tr -d "[:space:]")"
case "$avail_kb" in
    ''|*[!0-9]*)
        echo "error: unable to parse free space for /mnt/card (got: $avail_kb)" >&2
        exit 1
        ;;
esac

if [ "$avail_kb" -lt "$need_kb" ]; then
    echo "error: insufficient free space on /mnt/card" >&2
    echo "       need: ${need_kb}KB  have: ${avail_kb}KB" >&2
    exit 1
fi

remote_size() {
    ssh $SSH_OPTS "$TARGET" 'if [ -f /mnt/card/id1/pak0.pak ]; then wc -c < /mnt/card/id1/pak0.pak; else echo 0; fi' | tr -d "[:space:]"
}

have="$(remote_size)"
case "$have" in
    ''|*[!0-9]*)
        echo "invalid remote size: $have" >&2
        exit 1
        ;;
esac

if [ "$have" -gt "$SIZE_LOCAL" ]; then
    echo "remote file larger than source; restarting upload"
    ssh $SSH_OPTS "$TARGET" 'rm -f /mnt/card/id1/pak0.pak'
    have=0
fi

stall_count=0
while [ "$have" -lt "$SIZE_LOCAL" ]; do
    remain=$((SIZE_LOCAL - have))
    chunk="$CHUNK_SIZE"
    if [ "$remain" -lt "$chunk" ]; then
        chunk="$remain"
    fi

    echo "upload offset=$have chunk=$chunk"
    if command -v timeout >/dev/null 2>&1; then
        if ! dd if="$PAK" bs=1 skip="$have" count="$chunk" 2>/dev/null | timeout "${CHUNK_TIMEOUT}s" ssh $SSH_OPTS "$TARGET" 'cat >> /mnt/card/id1/pak0.pak'; then
            echo "chunk transfer interrupted; retrying from current remote size"
        fi
    else
        if ! dd if="$PAK" bs=1 skip="$have" count="$chunk" 2>/dev/null | ssh $SSH_OPTS "$TARGET" 'cat >> /mnt/card/id1/pak0.pak'; then
            echo "chunk transfer interrupted; retrying from current remote size"
        fi
    fi

    new_have="$(remote_size)"
    case "$new_have" in
        ''|*[!0-9]*)
            stall_count=$((stall_count + 1))
            echo "remote size check failed; retry $stall_count/$STALL_RETRIES"
            if [ "$stall_count" -ge "$STALL_RETRIES" ]; then
                echo "aborting after repeated remote size check failures" >&2
                exit 1
            fi
            continue
            ;;
    esac

    if [ "$new_have" -lt "$have" ]; then
        echo "remote size moved backwards: before=$have after=$new_have" >&2
        exit 1
    fi

    if [ "$new_have" = "$have" ]; then
        stall_count=$((stall_count + 1))
        echo "no upload progress (still $have bytes); retry $stall_count/$STALL_RETRIES"
        if [ "$stall_count" -ge "$STALL_RETRIES" ]; then
            echo "aborting after repeated stalled retries" >&2
            exit 1
        fi
        continue
    fi

    stall_count=0
    have="$new_have"
done

ssh $SSH_OPTS "$TARGET" '
set -eu
SIZE_REMOTE="$(wc -c < /mnt/card/id1/pak0.pak)"
MD5_REMOTE="$(md5sum /mnt/card/id1/pak0.pak | awk "{print \$1}")"
echo "device pak0.pak size: $SIZE_REMOTE"
echo "device pak0.pak md5:  $MD5_REMOTE"
ls -lh /mnt/card/id1/pak0.pak
'

SIZE_REMOTE="$(ssh $SSH_OPTS "$TARGET" 'wc -c < /mnt/card/id1/pak0.pak')"
MD5_REMOTE="$(ssh $SSH_OPTS "$TARGET" 'md5sum /mnt/card/id1/pak0.pak | awk "{print \$1}"')"

if [ "$SIZE_REMOTE" != "$SIZE_LOCAL" ]; then
    echo "size mismatch: host=$SIZE_LOCAL device=$SIZE_REMOTE" >&2
    exit 1
fi

if [ "$MD5_REMOTE" != "$MD5_LOCAL" ]; then
    echo "md5 mismatch: host=$MD5_LOCAL device=$MD5_REMOTE" >&2
    exit 1
fi

echo "verified: size+md5 match"

echo "done"
