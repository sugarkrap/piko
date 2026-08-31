#!/bin/sh
set -eu

ADAPTER=""
TARGET=""
KERNEL_ONLY=0
ONLY=""
NO_USERSPACE=0
CREATE_BACKUP_FILES=0
REPLACE_DROPBEAR=0
VERIFY=0
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
        --kernel-only)
            KERNEL_ONLY=1
            shift
            ;;
        --only)
            if [ $# -lt 2 ]; then
                echo "FAILED: --only requires a name or glob" >&2
                exit 1
            fi
            ONLY="$ONLY $2"
            shift 2
            ;;
        --no-userspace)
            NO_USERSPACE=1
            shift
            ;;
        --create-backup-files)
            CREATE_BACKUP_FILES=1
            shift
            ;;
        --replace-dropbear)
            REPLACE_DROPBEAR=1
            shift
            ;;
        --verify)
            VERIFY=1
            shift
            ;;
        --help|-h)
            echo "Usage: tools/chunked-deploy.sh [OPTIONS] [user@host]"
            echo ""
            echo "  --adapter IFACE"
            echo "  --target user@host"
            echo "  --kernel-only"
            echo "  --only NAME|GLOB         deploy just the matching files (repeatable)"
            echo "  --no-userspace"
            echo "  --create-backup-files"
            echo "  --replace-dropbear"
            echo "  --verify                re-hash on the device, ignore /var/lib/piko/deploy-manifest"
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
TARGET="${TARGET:-root@10.43.112.72}"
KEY="${HOME}/.ssh/zaurus_ed25519"
SSH_CONTROL="${TMPDIR:-/tmp}/piko-deploy-%r@%h:%p"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o StrictHostKeyChecking=accept-new -o ControlMaster=auto -o ControlPath=$SSH_CONTROL -o ControlPersist=120"
if [ -n "$ADAPTER" ]; then
    SSH_OPTS="$SSH_OPTS -B $ADAPTER"
fi
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
DEFAULT_REPO="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
REPO="${REPO:-$DEFAULT_REPO}"
MPLAYER_STAGE="${MPLAYER_STAGE:-$REPO/build/stage-mplayer}"
ALSA_STAGE="${ALSA_STAGE:-$REPO/build/stage-alsa-runtime}"
SDL_STAGE="${SDL_STAGE:-$REPO/build/stage-sdl-runtime}"
HOST_TRIPLET="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TCROOT="${TCROOT:-$REPO/toolchain/x-tools/$HOST_TRIPLET/$HOST_TRIPLET/sysroot}"
KERNEL_DIR="${KERNEL_DIR:-$REPO/build/kernel/src/linux-7.1.4}"

if [ ! -d "$REPO" ]; then
    echo "FAILED: REPO does not exist: $REPO" >&2
    exit 1
fi
if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
    echo "FAILED: expected built kernel image missing: $KERNEL_DIR/arch/arm/boot/zImage" >&2
    echo "Run tools/build-and-deploy.sh first, or set KERNEL_DIR to the tree that contains your latest build." >&2
    exit 1
fi
CHUNK_SIZE=524288
MAX_ATTEMPTS=8
RETRY_DELAY=2
STAGE="$(mktemp -d /tmp/zaurus-chunked-deploy.XXXXXX)"
REMOTE_STAGE="/tmp/chunked-deploy-stage"

LOCKFILE="/tmp/zaurus-chunked-deploy.lock"
exec 9>"$LOCKFILE"
if ! flock -n 9; then
    echo "FAILED: another deploy holds $LOCKFILE" >&2
    exit 1
fi

cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

ssh_do() {
    ssh_do_attempt=1
    while :; do
        if ssh $SSH_OPTS -i "$KEY" "$TARGET" "$1" > "$STAGE/.ssh_do.out"; then
            cat "$STAGE/.ssh_do.out"
            return 0
        fi
        if [ "$ssh_do_attempt" -ge "$MAX_ATTEMPTS" ]; then
            echo "FAILED: ssh command did not succeed after $MAX_ATTEMPTS attempts: $1" >&2
            return 1
        fi
        echo "  (ssh_do: connection problem, retrying in ${RETRY_DELAY}s: $1)" >&2
        ssh_do_attempt=$((ssh_do_attempt + 1))
        sleep "$RETRY_DELAY"
    done
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

DEPLOY_MANIFEST="${DEPLOY_MANIFEST:-/var/lib/piko/deploy-manifest}"
MANIFEST_LOCAL="$STAGE/deploy-manifest"
MANIFEST_DIRTY=0
: > "$MANIFEST_LOCAL"

manifest_lookup() {
    awk -v p="$1" '$2 == p { print $1; exit }' "$MANIFEST_LOCAL" 2>/dev/null
}

manifest_record() {
    awk -v p="$1" '$2 != p' "$MANIFEST_LOCAL" > "$MANIFEST_LOCAL.tmp" 2>/dev/null || : > "$MANIFEST_LOCAL.tmp"
    mv "$MANIFEST_LOCAL.tmp" "$MANIFEST_LOCAL"
    echo "$2 $1" >> "$MANIFEST_LOCAL"
    MANIFEST_DIRTY=1
}

manifest_push() {
    [ "$MANIFEST_DIRTY" -eq 1 ] || return 0
    ssh_do "mkdir -p '$(dirname "$DEPLOY_MANIFEST")'"
    if cat "$MANIFEST_LOCAL" | ssh $SSH_OPTS -i "$KEY" "$TARGET" "cat > '$DEPLOY_MANIFEST'"; then
        echo "==> recorded $(wc -l < "$MANIFEST_LOCAL") hashes in $DEPLOY_MANIFEST"
        MANIFEST_DIRTY=0
    else
        echo "==> could not write $DEPLOY_MANIFEST -- the next run will re-hash everything" >&2
    fi
}

DEFERRED=""

defer() {
    DEFERRED="$DEFERRED
$1"
}

flush_deferred() {
    if [ -n "$DEFERRED" ]; then
        echo "==> applying deferred permissions in one pass"
        ssh_do "$(printf '%s\n' "$DEFERRED" | sed '/^$/d; s/$/ 2>\/dev\/null || true/')"
        DEFERRED=""
    fi
}

wants() {
    if [ -z "$ONLY" ]; then
        return 0
    fi
    for pattern in $ONLY; do
        case "$1" in $pattern) return 0 ;; esac
        case "$2" in $pattern) return 0 ;; esac
    done
    return 1
}

send_file() {
    local_path="$1"
    remote_path="$2"
    name="$(basename "$remote_path")"

    if [ "$remote_path" != "/usr/bin/md5sum" ] && ! wants "$name" "$remote_path"; then
        return
    fi

    remote_dir="$(dirname "$remote_path")"
    if [ "$remote_dir" != "/" ]; then
        ssh_do "mkdir -p '$remote_dir'"
    fi

    local_size=$(wc -c < "$local_path")
    local_mode="$(stat -c %a "$local_path")"
    chunk_dir="$STAGE/$name.chunks"
    remote_chunk_dir="$REMOTE_STAGE/$name.chunks"
    file_attempt=1
    wanted=""

    already="$(remote_size "$remote_path")"
    if [ "$already" = "$local_size" ]; then
        if [ -n "$MD5SUM_LOCAL" ]; then
            wanted="$(local_md5 "$local_path")"
            if [ "$VERIFY" -eq 0 ] && [ "$(manifest_lookup "$remote_path")" = "$wanted" ]; then
                echo "==> $name: unchanged since the last deploy ($local_size bytes), skipping"
                return
            fi
        fi
        if [ "$HAVE_REMOTE_MD5" = 1 ] && [ -n "$MD5SUM_LOCAL" ]; then
            if [ "$(remote_md5 "$remote_path")" = "$wanted" ]; then
                echo "==> $name: already deployed ($local_size bytes, md5 verified), skipping"
                manifest_record "$remote_path" "$wanted"
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

    if [ "$CREATE_BACKUP_FILES" -eq 1 ]; then
        ssh_do "
            set -e
            chmod '$local_mode' '$remote_new'
            rm -rf '$remote_chunk_dir'
            if [ -f '$remote_path' ]; then
                cp '$remote_path' '$remote_path.bak'
            fi
            mv '$remote_new' '$remote_path'
        "
        echo "==> $name: deployed to $remote_path (previous copy at $remote_path.bak)"
    else
        ssh_do "
            set -e
            chmod '$local_mode' '$remote_new'
            rm -rf '$remote_chunk_dir'
            mv '$remote_new' '$remote_path'
        "
        echo "==> $name: deployed to $remote_path"
    fi
    [ -n "$MD5SUM_LOCAL" ] && manifest_record "$remote_path" "$(local_md5 "$local_path")"
    return 0
}

echo "Target: $TARGET"

DEV_MEDIA="$(ssh_do '. /etc/piko-media 2>/dev/null
echo "${PIKO_KERNEL:-/boot/zImage-full}"
echo "${PIKO_CARD_ROOT:-/mnt/card/.zaurus}"
echo "${PIKO_CARD_MNT:-/mnt/card}"')"
KERNEL_DEST="${KERNEL_DEST:-$(echo "$DEV_MEDIA" | sed -n 1p)}"
CARD_ROOT="${CARD_ROOT:-$(echo "$DEV_MEDIA" | sed -n 2p)}"
CARD_MNT="${CARD_MNT:-$(echo "$DEV_MEDIA" | sed -n 3p)}"
CARD_TMP="${CARD_TMP:-$CARD_ROOT/tmp}"
MPLAYER_DEST="${MPLAYER_DEST:-$CARD_ROOT/usr/bin/mplayer}"
echo "==> this device boots $KERNEL_DEST and keeps its card payload in $CARD_ROOT"

ssh_do "mount '$CARD_MNT' 2>/dev/null || true"
if ssh_do "grep -q ' $CARD_MNT ' /proc/mounts && echo yes || echo no" | grep -q yes; then
    REMOTE_STAGE="$CARD_TMP"
    X11_PAYLOAD_REMOTE="${X11_PAYLOAD_REMOTE:-$CARD_TMP/x11-payload.tar}"
    if [ -n "$ONLY" ]; then
    echo "==> filtered deploy:$ONLY"
fi
echo "==> staging transfers on the card ($REMOTE_STAGE)"
else
    X11_PAYLOAD_REMOTE="${X11_PAYLOAD_REMOTE:-/tmp/x11-payload.tar}"
    echo "==> no card at $CARD_MNT -- staging on the root ($REMOTE_STAGE)" >&2
fi

ssh_do "mkdir -p '$REMOTE_STAGE'"

if [ "$VERIFY" -eq 1 ]; then
    echo "==> --verify: re-hashing every destination, ignoring $DEPLOY_MANIFEST"
else
    ssh_do "cat '$DEPLOY_MANIFEST' 2>/dev/null || true" > "$MANIFEST_LOCAL"
    manifest_lines="$(wc -l < "$MANIFEST_LOCAL")"
    if [ "$manifest_lines" -gt 0 ]; then
        echo "==> $DEPLOY_MANIFEST has $manifest_lines hashes from the last deploy"
    else
        echo "==> no $DEPLOY_MANIFEST yet -- this run hashes on the device and writes one"
    fi
fi

MD5SUM_BIN="$REPO/build/target/bin/md5sum"
if [ -f "$MD5SUM_BIN" ] && [ -n "$MD5SUM_LOCAL" ]; then
    send_file "$MD5SUM_BIN" "/usr/bin/md5sum"
    defer "chmod 0755 /usr/bin/md5sum" || true
fi
if [ "$(ssh_do "if [ -x /usr/bin/md5sum ]; then echo 1; else echo 0; fi")" = "1" ]; then
    HAVE_REMOTE_MD5=1
    echo "==> remote md5sum available -- content-verifying all subsequent transfers"
else
    echo "==> remote md5sum NOT available -- falling back to size-only verification"
fi

ssh_do "mkdir -p '$(dirname "$KERNEL_DEST")'"
send_file "$KERNEL_DIR/arch/arm/boot/zImage" "$KERNEL_DEST"

if [ "$KERNEL_ONLY" -eq 1 ]; then
    echo "==> --kernel-only: skipping module/script/helper deployment"
    ssh_do "rm -rf '$REMOTE_STAGE'"
    manifest_push
    echo "==> done (kernel only)"
    exit 0
fi

KVER_LOCAL=""
if [ -f "$KERNEL_DIR/include/config/kernel.release" ]; then
    KVER_LOCAL="$(cat "$KERNEL_DIR/include/config/kernel.release")"
fi
KVER_REMOTE="$(ssh_do 'uname -r')"
echo "==> remote kernel release currently running: $KVER_REMOTE (will change after reboot to the newly deployed kernel)"

. "$REPO/tools/kernel/kernel-modules.sh"
MODULES=""
for m in $AUDIO_MODULES; do
    MODULES="$MODULES
$KERNEL_DIR/$m"
done

for m in $MODULES; do
    if [ ! -f "$m" ]; then
        echo "missing module: $m" >&2
        exit 1
    fi
    b="$(basename "$m")"
    send_file "$m" "/lib/modules/$KVER_LOCAL/zaurus-audio/$b"
done

for relpath in $WIFI_MODULES; do
    local_path="$KERNEL_DIR/$(echo "$relpath" | sed 's#^kernel/##')"
    if [ ! -f "$local_path" ]; then
        echo "missing module: $local_path" >&2
        exit 1
    fi
    remote_path="/lib/modules/$KVER_LOCAL/$relpath"
    send_file "$local_path" "$remote_path"
done

for relpath in $SD_MODULES $NAND_MODULES; do
    local_path="$KERNEL_DIR/$(echo "$relpath" | sed 's#^kernel/##')"
    if [ ! -f "$local_path" ]; then
        echo "missing module: $local_path" >&2
        exit 1
    fi
    remote_path="/lib/modules/$KVER_LOCAL/$relpath"
    send_file "$local_path" "$remote_path"
done

KEEP_EXISTING="etc/TZ
etc/piko/touchscreen.cfg
etc/piko/phoneme.cfg
etc/wpa_supplicant/wpa_supplicant.conf"

echo "==> rootfs overlay ($(cd "$REPO/rootfs" && find . -type f | wc -l) tracked files)"
ssh_do "mkdir -p $(cd "$REPO/rootfs" && find . -mindepth 1 -type d | sed 's#^\.##' | tr '\n' ' ')"
for rel in $(cd "$REPO/rootfs" && find . -type f | sed 's#^\./##' | sort); do
    keep=0
    for k in $KEEP_EXISTING; do
        [ "$rel" = "$k" ] && keep=1
    done
    if [ "$keep" -eq 1 ] && [ "$(ssh_do "if [ -e /$rel ]; then echo yes; else echo no; fi")" = "yes" ]; then
        echo "==> /$rel: keeping the device's own copy"
        continue
    fi
    send_file "$REPO/rootfs/$rel" "/$rel"
done
defer "chmod 0755 $(cd "$REPO/rootfs" && find . -type f -perm -100 | sed 's#^\.##' | tr '\n' ' ')"
defer "chmod 0644 $(cd "$REPO/rootfs" && find . -type f ! -perm -100 | sed 's#^\.##' | tr '\n' ' ')"

if [ -f "$REPO/build/target/bin/brightd" ]; then
    send_file "$REPO/build/target/bin/brightd" "/usr/sbin/brightd"
    defer "chmod 0755 /usr/sbin/brightd"
else
    echo "==> skipping brightd (not built -- run tools/userspace/build-userspace.sh)"
fi

if [ -f "$REPO/build/target/bin/piko-splash" ]; then
    send_file "$REPO/build/target/bin/piko-splash" "/usr/sbin/piko-splash"
    defer "chmod 0755 /usr/sbin/piko-splash"
else
    echo "==> skipping piko-splash (not built -- run tools/userspace/build-userspace.sh)"
fi

if [ -f "$REPO/build/target/bin/mhz" ]; then
    send_file "$REPO/build/target/bin/mhz" "/usr/sbin/mhz"
    defer "chmod 0755 /usr/sbin/mhz"
else
    echo "==> skipping mhz (not built -- run tools/userspace/build-userspace.sh)"
fi
if [ -f "$REPO/build/target/bin/flipd" ]; then
    send_file "$REPO/build/target/bin/flipd" "/usr/sbin/flipd"
    defer "chmod 0755 /usr/sbin/flipd"
else
    echo "==> skipping flipd (not built -- run tools/userspace/build-userspace.sh)"
fi
for clock_tool in hwclock ntpsync; do
    if [ -f "$REPO/build/target/bin/$clock_tool" ]; then
        send_file "$REPO/build/target/bin/$clock_tool" "/usr/sbin/$clock_tool"
        defer "chmod 0755 /usr/sbin/$clock_tool"
    else
        echo "==> skipping $clock_tool (not built -- run tools/userspace/build-userspace.sh)"
    fi
done
if [ -x "$REPO/build/target/bin/cardswap" ]; then
    send_file "$REPO/build/target/bin/cardswap" "/usr/sbin/cardswap"
    defer "chmod 0755 /usr/sbin/cardswap"
else
    echo "==> no built cardswap -- skipping (run tools/userspace/build-userspace.sh)"
    echo "    without it an inserted card mounts, but gets no swap area"
fi

if [ -x "$REPO/build/target/bin/zramswap" ]; then
    send_file "$REPO/build/target/bin/zramswap" "/usr/sbin/zramswap"
    defer "chmod 0755 /usr/sbin/zramswap"
else
    echo "==> no built zramswap -- skipping (run tools/userspace/build-userspace.sh)"
    echo "    without it the machine falls back to card-only swap"
fi

SSH_STAGE="${SSH_STAGE:-$REPO/build/stage-ssh}"
if [ "$KERNEL_ONLY" -eq 0 ] && [ -d "$SSH_STAGE" ]; then
    echo "==> SSH file transfer payload (scp + sftp-server)"
    SSH_PAYLOAD_FILES="usr/bin/scp:usr/bin/scp:755
usr/libexec/sftp-server:usr/libexec/sftp-server:755
usr/bin/dbclient:usr/bin/dbclient:755
usr/bin/dropbearkey:usr/bin/dropbearkey:755"
    SSH_PAYLOAD_SERVER="usr/sbin/dropbear:usr/sbin/dropbear:755"
    for entry in $SSH_PAYLOAD_FILES; do
        src="$SSH_STAGE/${entry%%:*}"
        rest="${entry#*:}"
        dest="/${rest%%:*}"
        mode="${rest#*:}"
        ssh_do "mkdir -p '$(dirname "$dest")'"
        send_file "$src" "$dest"
        defer "chmod 0$mode '$dest'"
    done

    if [ "$REPLACE_DROPBEAR" -eq 1 ]; then
        srv_src="$SSH_STAGE/${SSH_PAYLOAD_SERVER%%:*}"
        srv_rest="${SSH_PAYLOAD_SERVER#*:}"
        srv_dest="/${srv_rest%%:*}"
        echo "==> replacing $srv_dest (rename-aside, effective next boot)"
        ssh_do "if [ -f '$srv_dest' ]; then mv -f '$srv_dest' '$srv_dest.prev'; fi"
        send_file "$srv_src" "$srv_dest"
        defer "chmod 0${srv_rest#*:} '$srv_dest'"
        echo "    old server at /usr/sbin/dropbear.prev, effective next boot"
    fi
elif [ "$KERNEL_ONLY" -eq 0 ]; then
    echo "==> no SSH payload at $SSH_STAGE -- skipping (run tools/userspace/build-ssh.sh)"
fi

if [ -x "$REPO/build/target/usr/bin/opkg" ]; then
    ssh_do "mkdir -p /etc/opkg /var/lib/opkg/info /var/cache/opkg"
    send_file "$REPO/build/target/usr/bin/opkg" "/usr/bin/opkg"
    defer "chmod 0755 /usr/bin/opkg"
else
    echo "==> no staged opkg -- skipping (build it with tools/userspace/build-opkg.sh)"
fi

if [ -x "$REPO/build/target/bin/kill" ]; then
    send_file "$REPO/build/target/bin/kill" "/usr/bin/kill"
    defer "chmod 0755 /usr/bin/kill"
    ssh_do "mkdir -p /usr/local/bin && ln -sf /usr/bin/kill /usr/local/bin/kill"
fi

for lib in libpikovideo.so.1 libpikorom.so.1; do
    if [ -f "$REPO/build/target/usr/lib/$lib" ]; then
        send_file "$REPO/build/target/usr/lib/$lib" "/lib/$lib"
        defer "chmod 0755 /lib/$lib"
    else
        echo "==> no built $lib -- skipping (run tools/userspace/build-pikoemu.sh and build-libpikorom.sh)"
        echo "    without it pikoemu and piko-sync-server will not start"
    fi
done

if [ -x "$REPO/build/target/bin/pikoemu" ]; then
    send_file "$REPO/build/target/bin/pikoemu" "/usr/local/bin/pikoemu"
    defer "chmod 0755 /usr/local/bin/pikoemu"
else
    echo "==> no built pikoemu -- skipping (run tools/userspace/build-pikoemu.sh)"
fi

UI_STAGE="$REPO/build/stage-ui/usr/local/share/piko/ui"
if [ -f "$UI_STAGE/notify.pkui" ]; then
    ssh_do "mkdir -p /usr/local/share/piko/ui"
    for f in "$UI_STAGE"/*; do
        send_file "$f" "/usr/local/share/piko/ui/$(basename "$f")"
    done
    defer "chmod 0644 /usr/local/share/piko/ui/*"
else
    echo "==> no baked pikoemu ui -- skipping (tools/scripts/ui-bake.js, font-to-pkfn.js)"
    echo "    without it pikoemu draws no notifications"
fi

BEZEL_STAGE="$REPO/build/stage-bezels/usr/local/.zaurus/bezels"
if [ -d "$BEZEL_STAGE" ]; then
    ssh_do "mkdir -p /usr/local/.zaurus/bezels"
    for f in "$BEZEL_STAGE"/*.pkbz; do
        [ -f "$f" ] || continue
        send_file "$f" "/usr/local/.zaurus/bezels/$(basename "$f")"
    done
    defer "chmod 0644 /usr/local/.zaurus/bezels/*.pkbz"
else
    echo "==> no baked bezels -- skipping (run tools/userspace/build-bezels.sh)"
fi

if [ -x "$REPO/build/target/bin/piko-sync-server" ]; then
    echo "==> piko-sync-server (stopping it first -- it cannot replace itself)"
    ssh_do "for p in \$(ps | grep '[p]iko-sync-server' | while read a b; do echo \$a; done); do /usr/local/bin/kill -15 \$p 2>/dev/null; done; sleep 1" || true
    send_file "$REPO/build/target/bin/piko-sync-server" "/usr/bin/piko-sync-server"
    defer "chmod 0755 /usr/bin/piko-sync-server"
    if [ -f "$REPO/userspace/desktop/piko-sync-server.desktop" ]; then
        send_file "$REPO/userspace/desktop/piko-sync-server.desktop" "/usr/share/applications/piko-sync-server.desktop"
    fi
    if [ -f "$REPO/userspace/desktop/piko-sync-server.png" ]; then
        send_file "$REPO/userspace/desktop/piko-sync-server.png" "/usr/share/pixmaps/piko-sync-server.png"
    fi
else
    echo "==> no built piko-sync-server -- skipping (run tools/userspace/build-piko-sync.sh)"
fi

if [ -x "$REPO/build/target/bin/pkillx" ]; then
    send_file "$REPO/build/target/bin/pkillx" "/usr/sbin/pkillx"
    defer "chmod 0755 /usr/sbin/pkillx"
else
    echo "==> no built pkillx -- skipping (run tools/userspace/build-userspace.sh)"
    echo "    without it /usr/sbin/gototty is a no-op: 'pkillx: not found'"
fi

if [ -x "$REPO/build/target/bin/vol" ]; then
    send_file "$REPO/build/target/bin/vol" "/usr/sbin/vol"
    defer "chmod 0755 /usr/sbin/vol"
else
    echo "==> no built vol -- skipping (run tools/userspace/build-userspace.sh)"
fi

if [ -x "$REPO/build/target/bin/fbtext" ]; then
    send_file "$REPO/build/target/bin/fbtext" "/usr/sbin/fbtext"
    defer "chmod 0755 /usr/sbin/fbtext"
else
    echo "==> no built fbtext -- skipping (run tools/userspace/build-userspace.sh)"
fi

if [ "$NO_USERSPACE" -eq 0 ] && [ -d "$MPLAYER_STAGE" -o -d "$ALSA_STAGE" -o -d "$SDL_STAGE" ]; then
    need=0
    MPLAYER_ON_ROOT=1
    case "$MPLAYER_DEST" in /mnt/*) MPLAYER_ON_ROOT=0 ;; esac
    MPLAYER_SIZED=""
    [ "$MPLAYER_ON_ROOT" -eq 1 ] && MPLAYER_SIZED="$MPLAYER_STAGE/usr/bin/mplayer"
    SDL_SO_REAL=""
    if [ -d "$SDL_STAGE/usr/lib" ]; then
        SDL_SO_REAL="$(cd "$SDL_STAGE/usr/lib" && ls libSDL-1.2.so.0.* 2>/dev/null | head -1 || true)"
    fi
    SDL_SIZED=""
    [ -n "$SDL_SO_REAL" ] && SDL_SIZED="$SDL_STAGE/usr/lib/$SDL_SO_REAL"
    for f in $MPLAYER_SIZED $SDL_SIZED "$SDL_STAGE/usr/bin/sdltest" "$SDL_STAGE/usr/bin/pikalibrate" \
             "$ALSA_STAGE/usr/bin/aplay" "$ALSA_STAGE/usr/bin/amixer" \
             "$ALSA_STAGE/usr/sbin/alsactl"; do
        if [ -f "$f" ]; then
            need=$((need + $(wc -c < "$f")))
        fi
    done
    need_kb=$(((need / 1024) + 512))
    avail_kb="$(ssh_do "df /usr | tail -n 1" | awk '{print $4}')"
    case "$avail_kb" in ''|*[!0-9]*) avail_kb=0 ;; esac

    echo "==> userspace payload: needs ~${need_kb} KiB, device has ${avail_kb} KiB free on /"
    if [ "$avail_kb" -gt 0 ] && [ "$need_kb" -gt "$((avail_kb - 4096))" ]; then
        echo "SKIPPING userspace payload: needs ${need_kb} KiB, ${avail_kb} KiB free, 4 MiB reserved" >&2
    else
        if [ -d "$ALSA_STAGE" ]; then
            ssh_do "mkdir -p /usr/share/alsa/cards /usr/share/alsa/pcm /usr/share/alsa/ctl /var/lib/alsa"
            for rel in share/alsa/alsa.conf \
                       share/alsa/cards/aliases.conf \
                       share/alsa/ctl/default.conf; do
                if [ -f "$ALSA_STAGE/usr/$rel" ]; then
                    send_file "$ALSA_STAGE/usr/$rel" "/usr/$rel"
                fi
            done
            for f in "$ALSA_STAGE"/usr/share/alsa/pcm/*.conf; do
                [ -f "$f" ] || continue
                send_file "$f" "/usr/share/alsa/pcm/$(basename "$f")"
            done
            for f in "$ALSA_STAGE"/usr/share/alsa/cards/*.conf; do
                [ -f "$f" ] || continue
                send_file "$f" "/usr/share/alsa/cards/$(basename "$f")"
            done
            for b in bin/aplay bin/amixer sbin/alsactl; do
                [ -f "$ALSA_STAGE/usr/$b" ] || continue
                send_file "$ALSA_STAGE/usr/$b" "/usr/$b"
                defer "chmod 0755 /usr/$b"
            done
        fi
        if [ -n "$SDL_SO_REAL" ]; then
            if [ -f "$TCROOT/lib/ld-uClibc-1.0.54.so" ] && [ -f "$TCROOT/lib/libuClibc-1.0.54.so" ]; then
                send_file "$TCROOT/lib/ld-uClibc-1.0.54.so" "/lib/ld-uClibc-1.0.54.so"
                send_file "$TCROOT/lib/libuClibc-1.0.54.so" "/lib/libuClibc-1.0.54.so"
                defer "chmod 0755 /lib/ld-uClibc-1.0.54.so /lib/libuClibc-1.0.54.so"
                ssh_do "
                    set -e
                    ln -sf ld-uClibc-1.0.54.so /lib/ld-uClibc.so.1
                    ln -sf ld-uClibc.so.1 /lib/ld-uClibc.so.0
                    ln -sf libuClibc-1.0.54.so /lib/libc.so.0
                    ln -sf libuClibc-1.0.54.so /lib/libc.so.1
                "
            else
                echo "==> WARNING: no ld-uClibc/libc.so under $TCROOT/lib" >&2
            fi
            send_file "$SDL_STAGE/usr/lib/$SDL_SO_REAL" "/lib/$SDL_SO_REAL"
            ssh_do "ln -sf '$SDL_SO_REAL' /lib/libSDL-1.2.so.0"
            ssh_do "rm -f /usr/lib/libSDL-1.2.so.0 /usr/lib/$SDL_SO_REAL"
            for extra in libSDL_image-1.2.so.0 libSDL_mixer-1.2.so.0; do
                [ -e "$SDL_STAGE/usr/lib/$extra" ] || continue
                extra_real="$(basename "$(readlink -f "$SDL_STAGE/usr/lib/$extra")")"
                send_file "$SDL_STAGE/usr/lib/$extra_real" "/lib/$extra_real"
                ssh_do "ln -sf '$extra_real' /lib/$extra"
                ssh_do "rm -f /usr/lib/$extra /usr/lib/$extra_real"
            done

            PHONEME_HOME="$REPO/build/stage-phoneme/usr/local/lib/phoneme"
            if [ -f "$PHONEME_HOME/bin/runMidlet" ]; then
                ssh_do "mkdir -p /usr/local/lib/phoneme/bin"
                send_file "$PHONEME_HOME/bin/runMidlet" "/usr/local/lib/phoneme/bin/runMidlet"
                defer "chmod 0755 /usr/local/lib/phoneme/bin/runMidlet"
                if wants "phoneme-data.tar" "$CARD_TMP/phoneme-data.tar"; then
                    tar -C "$PHONEME_HOME" -cf "$STAGE/phoneme-data.tar" lib appdb
                    send_file "$STAGE/phoneme-data.tar" "$CARD_TMP/phoneme-data.tar"
                    ssh_do "/usr/local/bin/untar '$CARD_TMP/phoneme-data.tar' /usr/local/lib/phoneme && rm -f '$CARD_TMP/phoneme-data.tar'"
                fi
                send_file "$REPO/userspace/src/phoneme-run" "/usr/local/bin/phoneme-run"
                defer "chmod 0755 /usr/local/bin/phoneme-run"
            fi

            if [ -f "$SDL_STAGE/usr/bin/sdltest" ]; then
                send_file "$SDL_STAGE/usr/bin/sdltest" "/usr/bin/sdltest"
                defer "chmod 0755 /usr/bin/sdltest"
            fi
            if [ -f "$SDL_STAGE/usr/bin/pikalibrate" ]; then
                send_file "$SDL_STAGE/usr/bin/pikalibrate" "/usr/bin/pikalibrate"
                defer "chmod 0755 /usr/bin/pikalibrate"
            fi
        fi
        if [ -f "$MPLAYER_STAGE/usr/bin/mplayer" ]; then
            case "$MPLAYER_DEST" in
            "$CARD_MNT"/*)
                ssh_do "mount '$CARD_MNT' 2>/dev/null || true"
                if ssh_do "grep -q ' $CARD_MNT ' /proc/mounts && echo yes || echo no" | grep -q yes; then
                    ssh_do "mkdir -p '$(dirname "$MPLAYER_DEST")'"
                    send_file "$MPLAYER_STAGE/usr/bin/mplayer" "$MPLAYER_DEST"
                    defer "chmod 0755 '$MPLAYER_DEST'"
                else
                    echo "==> no card at $CARD_MNT -- SKIPPING MPlayer (set MPLAYER_DEST to force)"
                fi
                ;;
            *)
                send_file "$MPLAYER_STAGE/usr/bin/mplayer" "$MPLAYER_DEST"
                defer "chmod 0755 '$MPLAYER_DEST'"
                ;;
            esac
        fi
    fi
fi

ssh_do "rm -rf '$REMOTE_STAGE'"

X11_PAYLOAD="${X11_PAYLOAD:-/tmp/matchbox-payload.tar}"
if [ "$KERNEL_ONLY" -eq 0 ] && [ -f "$X11_PAYLOAD" ] \
   && wants "x11-payload.tar" "$X11_PAYLOAD_REMOTE"; then
    echo "==> X11/Matchbox payload ($(wc -c < "$X11_PAYLOAD") bytes)"
    if [ -f "$REPO/build/target/bin/untar" ]; then
        send_file "$REPO/build/target/bin/untar" "/usr/local/bin/untar"
        defer "chmod 0755 /usr/local/bin/untar"
    fi

    if [ "$(ssh_do "if [ -x /usr/local/bin/untar ]; then echo 1; else echo 0; fi")" != "1" ]; then
        echo "FAILED: no /usr/local/bin/untar on the device" >&2
        exit 1
    fi
    send_file "$X11_PAYLOAD" "$X11_PAYLOAD_REMOTE"
    echo "==> stopping any running graphical session"
    ssh_do "for p in \$(ps | grep -E 'matchbox|xev|toasters' | grep -v grep | while read a b; do echo \$a; done); do /usr/local/bin/kill -15 \$p 2>/dev/null; done; sleep 2" || true
    ssh_do "for p in \$(ps | grep Xfbdev | grep -v grep | while read a b; do echo \$a; done); do /usr/local/bin/kill -15 \$p 2>/dev/null; done; sleep 2" || true
    ssh_do "/usr/local/bin/untar '$X11_PAYLOAD_REMOTE' / && rm -f '$X11_PAYLOAD_REMOTE'"
    echo "==> X11/Matchbox stack unpacked, session restarts on reboot"
elif [ "$KERNEL_ONLY" -eq 0 ]; then
    echo "==> no X11 payload at $X11_PAYLOAD -- skipping"
fi

manifest_push

echo ""
flush_deferred
echo "deployed, not rebooted: ssh -i $KEY $TARGET reboot"
