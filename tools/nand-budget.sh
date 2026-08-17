#!/bin/sh
set -eu

REPO="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TREE="${1:-}"
IMAGE="${IMAGE:-$REPO/flash/mtd3.jffs2}"
BASE_IMAGE="${BASE_IMAGE:-$REPO/flash/base.jffs2}"
NAND_KB="${NAND_KB:-69632}"
TOP="${TOP:-15}"

human() {
    awk -v kb="$1" 'BEGIN {
        if (kb >= 1024) printf "%.1f MiB", kb / 1024;
        else printf "%d KiB", kb;
    }'
}

bar() {
    awk -v pct="$1" 'BEGIN {
        n = int(pct / 4);
        if (n > 25) n = 25;
        s = "";
        for (i = 0; i < n; i++) s = s "#";
        for (i = n; i < 25; i++) s = s ".";
        printf "%s", s;
    }'
}

if [ -n "$TREE" ]; then
    if [ ! -d "$TREE" ]; then
        echo "nand-budget: not a directory: $TREE" >&2
        exit 1
    fi

    echo "==> uncompressed contents of $TREE"
    echo ""
    printf "    %-34s %10s\n" "COMPONENT" "SIZE"
    printf "    %-34s %10s\n" "---------" "----"

    total_kb=0
    for d in $(cd "$TREE" && find . -mindepth 1 -maxdepth 2 -type d \
                   | sed 's|^\./||' | sort); do
        case "$d" in
            */*) ;;
            *) continue ;;
        esac
        kb="$(du -sk "$TREE/$d" 2>/dev/null | while read -r n _; do echo "$n"; break; done)"
        [ -n "$kb" ] || continue
        [ "$kb" -lt 64 ] && continue
        printf "    %-34s %10s\n" "$d" "$(human "$kb")"
    done | sort -k2 -h -r 2>/dev/null || true

    total_kb="$(du -sk "$TREE" | while read -r n _; do echo "$n"; break; done)"
    echo ""
    printf "    %-34s %10s\n" "TOTAL (uncompressed)" "$(human "$total_kb")"

    echo ""
    echo "==> largest individual files"
    find "$TREE" -type f -printf '%s %p\n' 2>/dev/null \
        | sort -nr | head -n "$TOP" \
        | while read -r bytes path; do
            printf "    %10s  %s\n" "$(human $((bytes / 1024)))" "${path#$TREE/}"
        done
    echo ""
fi

if [ -f "$IMAGE" ]; then
    img_kb=$(( $(stat -c '%s' "$IMAGE") / 1024 ))
    pct=$(( img_kb * 100 / NAND_KB ))
    free_kb=$(( NAND_KB - img_kb ))
    echo "==> flashed image against the NAND ceiling"
    echo ""
    printf "    image      %s  (%s)\n" "$(human "$img_kb")" "$(basename "$IMAGE")"
    if [ -f "$BASE_IMAGE" ]; then
        base_kb=$(( $(stat -c '%s' "$BASE_IMAGE") / 1024 ))
        printf "    base       %s  (piko adds %s)\n" \
            "$(human "$base_kb")" "$(human $(( img_kb - base_kb )))"
    fi
    printf "    partition  %s\n" "$(human "$NAND_KB")"
    printf "    headroom   %s\n" "$(human "$free_kb")"
    echo ""
    printf "    [%s] %d%% of NAND, compressed\n" "$(bar "$pct")" "$pct"
    echo ""
    echo "    JFFS2 compresses, so the mounted filesystem uses more than this."
    echo "    Pass DEVICE=user@host for what the board actually reports."
    echo ""

    if [ -n "${DEVICE:-}" ]; then
        key="${SSH_KEY:-$HOME/.ssh/zaurus_ed25519}"
        df_line="$(ssh -o BatchMode=yes -o ConnectTimeout=15 -i "$key" "$DEVICE" \
                       'df / | tail -n 1' 2>/dev/null || true)"
        if [ -n "$df_line" ]; then
            set -- $df_line
            dev_total="$2"; dev_used="$3"; dev_free="$4"
            dev_pct=$(( dev_used * 100 / dev_total ))
            echo "==> as mounted on $DEVICE"
            echo ""
            printf "    used       %s of %s\n" "$(human "$dev_used")" "$(human "$dev_total")"
            printf "    free       %s\n" "$(human "$dev_free")"
            echo ""
            printf "    [%s] %d%% in use\n" "$(bar "$dev_pct")" "$dev_pct"
            echo ""
            pct="$dev_pct"
        else
            echo "    (could not reach $DEVICE)"
            echo ""
        fi
    fi

    if [ "$pct" -ge 90 ]; then
        echo "    WARNING: over 90% of the NAND partition is used." >&2
        echo "    Move something onto SD or CF as a Piko Sync software part." >&2
    fi
else
    echo "==> no image at $IMAGE yet -- build one with tools/build-mtd3-jffs2.sh"
fi
