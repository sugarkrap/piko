#!/bin/sh
set -eu

# Chunked, retry-tolerant deploy of the cross-compiled X11 stack (Xfbdev +
# matchbox-window-manager + matchbox-remote and their shared libs) onto the
# Zaurus, over the same known-flaky WiFi link tools/chunked-deploy.sh deals
# with. Reuses its chunk/reassemble/md5-verify transfer approach because the
# device shell has no scp, tar, gzip, or rsync -- only cat/cp/mkdir/ln over a
# plain `ssh host cmd` pipe.
#
# IMPORTANT: this device's rootfs is a from-scratch busybox/zsh/dropbear
# userland that is entirely STATICALLY linked -- there was no dynamic linker
# or libc.so anywhere on it before this script runs (verified via
# `find / -xdev -iname '*.so*'` returning nothing). Our cross-built
# Xfbdev/libX11/etc are dynamically linked against uClibc, so this script
# also bootstraps /lib/ld-uClibc*.so + /lib/libc.so onto the device from the
# same toolchain sysroot the binaries were built against -- skipping that
# would leave the kernel unable to find the ELF interpreter at exec time.
#
# Usage:
#   tools/deploy-x11.sh [--adapter IFACE] [--target user@host] [user@host]
#
# Only ONE instance of this script (and none of the other deploy scripts)
# may run against the device at a time -- see tools/chunked-deploy.sh for
# why (JFFS2 write races). This script uses its own lockfile.

ADAPTER=""
TARGET=""
while [ $# -gt 0 ]; do
    case "$1" in
        --adapter)
            if [ $# -lt 2 ]; then
                echo "FAILED: --adapter requires an interface name" >&2
                exit 1
            fi
            ADAPTER="$2"
            shift 2
            ;;
        --target)
            if [ $# -lt 2 ]; then
                echo "FAILED: --target requires user@host" >&2
                exit 1
            fi
            TARGET="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: tools/deploy-x11.sh [--adapter IFACE] [--target user@host] [user@host]"
            exit 0
            ;;
        --*)
            echo "FAILED: unknown option: $1" >&2
            exit 1
            ;;
        *)
            if [ -n "$TARGET" ]; then
                echo "FAILED: multiple targets specified ($TARGET and $1)" >&2
                exit 1
            fi
            TARGET="$1"
            shift
            ;;
    esac
done
TARGET="${TARGET:-root@10.208.47.72}"
KEY="${HOME}/.ssh/zaurus_ed25519"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o StrictHostKeyChecking=accept-new"
if [ -n "$ADAPTER" ]; then
    SSH_OPTS="$SSH_OPTS -B $ADAPTER"
fi

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
STAGE_TARGET="$REPO/userspace/stage-target/usr"
TCROOT="$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/arm-unknown-linux-uclibcgnueabi/sysroot"
CROSS_STRIP="$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin/arm-unknown-linux-uclibcgnueabi-strip"

for need in "$STAGE_TARGET/bin/Xfbdev" "$STAGE_TARGET/bin/matchbox-window-manager" "$STAGE_TARGET/bin/matchbox-remote"; do
    if [ ! -f "$need" ]; then
        echo "FAILED: expected built binary missing: $need" >&2
        echo "Cross-compile+stage the X11/matchbox stack into userspace/stage-target first." >&2
        exit 1
    fi
done
for need in "$TCROOT/lib/ld-uClibc-1.0.54.so" "$TCROOT/lib/libuClibc-1.0.54.so"; do
    if [ ! -f "$need" ]; then
        echo "FAILED: expected toolchain sysroot runtime lib missing: $need" >&2
        exit 1
    fi
done

CHUNK_SIZE=524288   # 512 KiB
MAX_ATTEMPTS=8
RETRY_DELAY=2
STAGE="$(mktemp -d /tmp/zaurus-deploy-x11.XXXXXX)"
DEPLOY_TREE="$STAGE/tree"
REMOTE_STAGE="/tmp/deploy-x11-stage"

LOCKFILE="/tmp/zaurus-deploy-x11.lock"
exec 9>"$LOCKFILE"
if ! flock -n 9; then
    echo "FAILED: another tools/deploy-x11.sh appears to already be running" >&2
    echo "        (lock held on $LOCKFILE) -- refusing to start a second," >&2
    echo "        overlapping deploy against the same device." >&2
    exit 1
fi

cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

ssh_do() {
    # shellcheck disable=SC2029
    ssh $SSH_OPTS -i "$KEY" "$TARGET" "$1"
}

remote_size() {
    ssh_do "if [ -f '$1' ]; then wc -c < '$1'; else echo MISSING; fi"
}

MD5SUM_LOCAL=""
if command -v md5sum >/dev/null 2>&1; then
    MD5SUM_LOCAL=md5sum
fi
HAVE_REMOTE_MD5=0

local_md5() {
    set -- $($MD5SUM_LOCAL "$1")
    echo "$1"
}

remote_md5() {
    ssh_do "if [ -f '$1' ]; then set -- \$(/usr/bin/md5sum '$1'); echo \"\$1\"; else echo MISSING; fi"
}

# send_file LOCAL_PATH REMOTE_PATH -- see tools/chunked-deploy.sh for the
# full rationale (chunking + per-chunk retry + md5 verification).
send_file() {
    local_path="$1"
    remote_path="$2"
    name="$(basename "$remote_path")"
    local_size=$(wc -c < "$local_path")
    chunk_dir="$STAGE/$name.chunks"
    remote_chunk_dir="$REMOTE_STAGE/$name.chunks"
    file_attempt=1

    already="$(remote_size "$remote_path")"
    if [ "$already" = "$local_size" ]; then
        if [ "$HAVE_REMOTE_MD5" = 1 ] && [ -n "$MD5SUM_LOCAL" ]; then
            if [ "$(remote_md5 "$remote_path")" = "$(local_md5 "$local_path")" ]; then
                echo "==> $name: already deployed ($local_size bytes, md5 verified), skipping"
                return
            fi
            echo "==> $name: size matches but md5 differs, redeploying"
        else
            echo "==> $name: already deployed ($local_size bytes), skipping"
            return
        fi
    fi

    while :; do
        if [ "$file_attempt" -gt "$MAX_ATTEMPTS" ]; then
            echo "FAILED: $name would not reassemble correctly after $MAX_ATTEMPTS full-file attempts" >&2
            exit 1
        fi

        echo "==> $name: $local_size bytes, chunking at $CHUNK_SIZE (file attempt $file_attempt/$MAX_ATTEMPTS)"
        rm -rf "$chunk_dir"
        mkdir -p "$chunk_dir"
        split -b "$CHUNK_SIZE" -d -a 5 "$local_path" "$chunk_dir/part."

        ssh_do "rm -rf '$remote_chunk_dir' && mkdir -p '$remote_chunk_dir'"

        for part in "$chunk_dir"/part.*; do
            part_name="$(basename "$part")"
            part_local_size=$(wc -c < "$part")
            remote_part="$remote_chunk_dir/$part_name"

            attempt=1
            while :; do
                existing=$(remote_size "$remote_part")
                if [ "$existing" = "$part_local_size" ]; then
                    break
                fi
                if [ "$attempt" -gt "$MAX_ATTEMPTS" ]; then
                    echo "FAILED: $part_name would not transfer intact after $MAX_ATTEMPTS attempts" >&2
                    exit 1
                fi
                echo "  $part_name: attempt $attempt/$MAX_ATTEMPTS"
                if ! cat "$part" | ssh $SSH_OPTS -i "$KEY" "$TARGET" "cat > '$remote_part'"; then
                    echo "  (connection dropped, retrying in ${RETRY_DELAY}s)"
                fi
                attempt=$((attempt + 1))
                sleep "$RETRY_DELAY"
            done
        done

        echo "==> $name: all chunks present, reassembling remotely"
        remote_new="$remote_path.new"
        ssh_do "cat '$remote_chunk_dir'/part.* > '$remote_new' && wc -c < '$remote_new'" > "$STAGE/$name.remote_size"
        remote_total=$(cat "$STAGE/$name.remote_size")
        if [ "$remote_total" != "$local_size" ]; then
            echo "FAILED: $name reassembled to $remote_total bytes, expected $local_size" >&2
            ssh_do "rm -rf '$remote_chunk_dir' '$remote_new'" || true
            file_attempt=$((file_attempt + 1))
            sleep "$RETRY_DELAY"
            continue
        fi

        if [ "$HAVE_REMOTE_MD5" = 1 ] && [ -n "$MD5SUM_LOCAL" ]; then
            remote_hash="$(remote_md5 "$remote_new")"
            local_hash="$(local_md5 "$local_path")"
            if [ "$remote_hash" != "$local_hash" ]; then
                echo "FAILED: $name md5 mismatch after reassembly (local $local_hash, remote $remote_hash)" >&2
                ssh_do "rm -rf '$remote_chunk_dir' '$remote_new'" || true
                file_attempt=$((file_attempt + 1))
                sleep "$RETRY_DELAY"
                continue
            fi
            echo "==> $name: md5 verified ($local_hash)"
        fi

        break
    done

    ssh_do "
        set -e
        mkdir -p '$(dirname "$remote_path")'
        mv '$remote_new' '$remote_path'
        rm -rf '$remote_chunk_dir'
    "
    echo "==> $name: deployed to $remote_path"
}

echo "Target: $TARGET"
ssh_do "mkdir -p '$REMOTE_STAGE'"

if [ "$(ssh_do "if [ -x /usr/bin/md5sum ]; then echo 1; else echo 0; fi")" = "1" ]; then
    HAVE_REMOTE_MD5=1
    echo "==> remote md5sum available -- content-verifying all transfers"
else
    echo "==> remote md5sum NOT available -- falling back to size-only verification"
fi

# --- Assemble a stripped local staging tree (originals in
# userspace/stage-target are left untouched, unstripped, for future
# relinking/debugging) ---
mkdir -p "$DEPLOY_TREE/lib" "$DEPLOY_TREE/usr/bin" "$DEPLOY_TREE/usr/lib"

cp "$TCROOT/lib/ld-uClibc-1.0.54.so" "$TCROOT/lib/libuClibc-1.0.54.so" "$DEPLOY_TREE/lib/"
cp "$STAGE_TARGET/bin/Xfbdev" "$STAGE_TARGET/bin/matchbox-window-manager" "$STAGE_TARGET/bin/matchbox-remote" "$DEPLOY_TREE/usr/bin/"
cp \
    "$STAGE_TARGET/lib/libX11.so.6.3.0" \
    "$STAGE_TARGET/lib/libX11-xcb.so.1.0.0" \
    "$STAGE_TARGET/lib/libXau.so.6.0.0" \
    "$STAGE_TARGET/lib/libXdmcp.so.6.0.0" \
    "$STAGE_TARGET/lib/libXext.so.6.4.0" \
    "$STAGE_TARGET/lib/libXfont.so.1.4.1" \
    "$STAGE_TARGET/lib/libfontenc.so.1.0.0" \
    "$STAGE_TARGET/lib/libmd.so.1.0.0" \
    "$STAGE_TARGET/lib/libpixman-1.so.0.42.2" \
    "$STAGE_TARGET/lib/libxkbfile.so.1.0.2" \
    "$STAGE_TARGET/lib/libz.so.1.3.1" \
    "$STAGE_TARGET/lib/libxcb.so.1.1.0" \
    "$DEPLOY_TREE/usr/lib/"

chmod u+w "$DEPLOY_TREE/lib"/*.so "$DEPLOY_TREE/usr/bin"/* "$DEPLOY_TREE/usr/lib"/*.so.*
find "$DEPLOY_TREE" -type f -exec "$CROSS_STRIP" --strip-unneeded {} \;

echo "==> local deploy tree assembled and stripped: $(du -sh "$DEPLOY_TREE" | cut -f1)"

# --- Core uClibc runtime (bootstraps dynamic linking on this device for
# the first time -- see header comment) ---
send_file "$DEPLOY_TREE/lib/ld-uClibc-1.0.54.so" "/lib/ld-uClibc-1.0.54.so"
send_file "$DEPLOY_TREE/lib/libuClibc-1.0.54.so" "/lib/libuClibc-1.0.54.so"
# `cat > file` over ssh (send_file's transfer mechanism) creates the
# remote file with the default umask, NOT the source's executable bit --
# confirmed on real hardware to land as 644. That's silently fatal for
# ld-uClibc-1.0.54.so specifically: the kernel's ELF loader opens the
# PT_INTERP target via the same open_exec() path (and therefore the same
# MAY_EXEC permission check) it uses for the top-level binary, so a
# non-executable dynamic linker makes EVERY dynamically-linked binary on
# the device fail execve() with EACCES ("Permission denied") -- this was
# silently breaking matchbox-remote/Xfbdev until caught here.
ssh_do "chmod 0755 /lib/ld-uClibc-1.0.54.so /lib/libuClibc-1.0.54.so"
ssh_do "
    set -e
    ln -sf ld-uClibc-1.0.54.so /lib/ld-uClibc.so.1
    ln -sf ld-uClibc.so.1 /lib/ld-uClibc.so.0
    ln -sf libuClibc-1.0.54.so /lib/libc.so.0
    ln -sf libuClibc-1.0.54.so /lib/libc.so.1
"
echo "==> /lib symlinks (ld-uClibc.so.0/.1, libc.so.0/.1) in place"

# --- X11 stack shared libs, with SONAME symlinks matching what the
# binaries actually request at NEEDED-resolution time ---
send_file "$DEPLOY_TREE/usr/lib/libX11.so.6.3.0" "/usr/lib/libX11.so.6.3.0"
send_file "$DEPLOY_TREE/usr/lib/libX11-xcb.so.1.0.0" "/usr/lib/libX11-xcb.so.1.0.0"
send_file "$DEPLOY_TREE/usr/lib/libXau.so.6.0.0" "/usr/lib/libXau.so.6.0.0"
send_file "$DEPLOY_TREE/usr/lib/libXdmcp.so.6.0.0" "/usr/lib/libXdmcp.so.6.0.0"
send_file "$DEPLOY_TREE/usr/lib/libXext.so.6.4.0" "/usr/lib/libXext.so.6.4.0"
send_file "$DEPLOY_TREE/usr/lib/libXfont.so.1.4.1" "/usr/lib/libXfont.so.1.4.1"
send_file "$DEPLOY_TREE/usr/lib/libfontenc.so.1.0.0" "/usr/lib/libfontenc.so.1.0.0"
send_file "$DEPLOY_TREE/usr/lib/libmd.so.1.0.0" "/usr/lib/libmd.so.1.0.0"
send_file "$DEPLOY_TREE/usr/lib/libpixman-1.so.0.42.2" "/usr/lib/libpixman-1.so.0.42.2"
send_file "$DEPLOY_TREE/usr/lib/libxkbfile.so.1.0.2" "/usr/lib/libxkbfile.so.1.0.2"
send_file "$DEPLOY_TREE/usr/lib/libz.so.1.3.1" "/usr/lib/libz.so.1.3.1"
send_file "$DEPLOY_TREE/usr/lib/libxcb.so.1.1.0" "/usr/lib/libxcb.so.1.1.0"

ssh_do "
    set -e
    ln -sf libX11.so.6.3.0 /usr/lib/libX11.so.6
    ln -sf libX11-xcb.so.1.0.0 /usr/lib/libX11-xcb.so.1
    ln -sf libXau.so.6.0.0 /usr/lib/libXau.so.6
    ln -sf libXdmcp.so.6.0.0 /usr/lib/libXdmcp.so.6
    ln -sf libXext.so.6.4.0 /usr/lib/libXext.so.6
    ln -sf libXfont.so.1.4.1 /usr/lib/libXfont.so.1
    ln -sf libfontenc.so.1.0.0 /usr/lib/libfontenc.so.1
    ln -sf libmd.so.1.0.0 /usr/lib/libmd.so.1
    ln -sf libpixman-1.so.0.42.2 /usr/lib/libpixman-1.so.0
    ln -sf libxkbfile.so.1.0.2 /usr/lib/libxkbfile.so.1
    ln -sf libz.so.1.3.1 /usr/lib/libz.so.1
    ln -sf libxcb.so.1.1.0 /usr/lib/libxcb.so.1
"
echo "==> /usr/lib SONAME symlinks in place"

# --- Binaries ---
send_file "$DEPLOY_TREE/usr/bin/Xfbdev" "/usr/bin/Xfbdev"
send_file "$DEPLOY_TREE/usr/bin/matchbox-window-manager" "/usr/bin/matchbox-window-manager"
send_file "$DEPLOY_TREE/usr/bin/matchbox-remote" "/usr/bin/matchbox-remote"
ssh_do "chmod 0755 /usr/bin/Xfbdev /usr/bin/matchbox-window-manager /usr/bin/matchbox-remote"

ssh_do "rm -rf '$REMOTE_STAGE'"

echo "==> done. Quick sanity check:"
ssh_do "ls -la /lib/ld-uClibc.so.0 /lib/libc.so.0 /usr/lib/libX11.so.6 /usr/bin/Xfbdev"
