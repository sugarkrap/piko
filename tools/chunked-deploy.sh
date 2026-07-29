#!/bin/sh
set -eu

# Chunked, retry-tolerant deploy of the stage-2 zImage + sound modules over
# a known-flaky WiFi link. Splits each file into small pieces and re-sends
# any piece whose remote size doesn't match after transfer, instead of
# re-sending the whole (multi-MB) file when the SSH link drops mid-copy.
#
# Only ONE instance of this script may run against the device at a time
# (enforced via a local flock below) -- two overlapping runs racing to
# write the same remote file has corrupted the on-device JFFS2 filesystem
# before (bad `mv`-in-place race on /boot/zImage-full).
#
# Usage:
#   tools/chunked-deploy.sh [--adapter IFACE] [--target user@host] [--kernel-only] [user@host]
# Example:
#   tools/chunked-deploy.sh --adapter wlan0 root@10.43.112.72
#
# --adapter IFACE binds the SSH connection to a specific local network
# interface (ssh -B), useful when the build machine has multiple network
# adapters and the Zaurus is only reachable via one of them.
# --target user@host sets the SSH destination explicitly. A positional
# user@host is also accepted for backwards compatibility.
# --kernel-only skips every module/script/helper deployment step below and
# only ships /boot/zImage-full (still preceded by the md5sum bootstrap and
# the compr=zlib remount preflight). Useful when iterating on a kernel-only
# change (e.g. the JFFS2 compressor fix itself) where redeploying modules
# that haven't changed just adds time and extra risk on the flaky link.
#
# Device shell is a stripped-down busybox ash: no md5sum/sha1sum/cksum/cmp,
# no `command` builtin, no printf. Per-chunk/per-file integrity is verified
# by byte count (wc -c) during transfer -- sufficient to catch truncation
# from a dropped connection, the actual failure mode on this link. On top
# of that, if userspace/src/md5sum (a small custom ARM/uclibc static
# binary, see userspace/src/md5sum.c) is deployed to /usr/bin/md5sum on
# the device, this script also does a real MD5 content comparison against
# the host's own `md5sum` for the final reassembled file -- catching the
# residual risk that byte-count alone can't (e.g. flash-write corruption
# after a correct transfer). Falls back to size-only if either side is
# missing md5sum.

ADAPTER=""
TARGET=""
KERNEL_ONLY=0
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
        --help|-h)
            echo "Usage: tools/chunked-deploy.sh [--adapter IFACE] [--target user@host] [--kernel-only] [user@host]"
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
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=30 -o ServerAliveInterval=15 -o ServerAliveCountMax=8 -o StrictHostKeyChecking=accept-new"
if [ -n "$ADAPTER" ]; then
    SSH_OPTS="$SSH_OPTS -B $ADAPTER"
fi
SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
DEFAULT_REPO="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
REPO="${REPO:-$DEFAULT_REPO}"
KERNEL_DIR="${KERNEL_DIR:-$REPO/kernel-src/linux-7.1.4}"

if [ ! -d "$REPO" ]; then
    echo "FAILED: REPO does not exist: $REPO" >&2
    exit 1
fi
if [ ! -f "$KERNEL_DIR/arch/arm/boot/zImage" ]; then
    echo "FAILED: expected built kernel image missing: $KERNEL_DIR/arch/arm/boot/zImage" >&2
    echo "Run tools/build-and-deploy.sh first, or set KERNEL_DIR to the tree that contains your latest build." >&2
    exit 1
fi
CHUNK_SIZE=524288   # 512 KiB
MAX_ATTEMPTS=8
RETRY_DELAY=2
STAGE="$(mktemp -d /tmp/zaurus-chunked-deploy.XXXXXX)"
REMOTE_STAGE="/tmp/chunked-deploy-stage"

# Hard guard against two overlapping copies of this script hitting the same
# device at once: two concurrent `mv .../zImage-full.new .../zImage-full`
# (or any other target file) racing on the device's JFFS2 filesystem can
# corrupt the destination inode (seen 2026-07-26: "jffs2: compression type
# 0x07 not available" / jffs2_decompress returned -5, unbootable until the
# .bak was restored via physical console). A terminal-reuse mixup that
# left an old background run alive while a new one was started is enough
# to trigger this -- so refuse to proceed at all if another instance
# already holds this lock, rather than relying on "it looked finished".
LOCKFILE="/tmp/zaurus-chunked-deploy.lock"
exec 9>"$LOCKFILE"
if ! flock -n 9; then
    echo "FAILED: another tools/chunked-deploy.sh appears to already be running" >&2
    echo "        (lock held on $LOCKFILE) -- refusing to start a second," >&2
    echo "        overlapping deploy against the same device. Confirm no" >&2
    echo "        other instance is active before retrying." >&2
    exit 1
fi


cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

ssh_do() {
    # shellcheck disable=SC2029
    ssh $SSH_OPTS -i "$KEY" "$TARGET" "$1"
}

remote_size() {
    # Prints remote file size in bytes, or "MISSING" if it doesn't exist.
    # (Check with `test -f` first rather than relying on stderr redirection
    # ordering to hide the "can't open" error -- ash applies the `<`
    # redirection before `2>/dev/null` takes effect, so the error would
    # print anyway.)
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
    # Prints the remote file's md5 hex digest, or "MISSING" if absent.
    # No `cut` on the device -- use `set --` to split md5sum's
    # "<hash>  <filename>" output on whitespace and take the first field.
    ssh_do "if [ -f '$1' ]; then set -- \$(/usr/bin/md5sum '$1'); echo \"\$1\"; else echo MISSING; fi"
}

# send_file LOCAL_PATH REMOTE_PATH
# Splits LOCAL_PATH into CHUNK_SIZE pieces, transfers each into a scratch
# dir under REMOTE_STAGE, retrying only the pieces that fail/mismatch, then
# concatenates remotely and verifies total size before replacing the
# destination (with a .bak of whatever was there before).
send_file() {
    local_path="$1"
    remote_path="$2"
    name="$(basename "$remote_path")"
    local_size=$(wc -c < "$local_path")
    chunk_dir="$STAGE/$name.chunks"
    remote_chunk_dir="$REMOTE_STAGE/$name.chunks"
    file_attempt=1

    # Skip entirely if the remote target already has the right byte count
    # (and, when md5sum is available on both ends, the right content too) --
    # lets a retry after a mid-run SSH drop resume near where it left off
    # instead of re-chunking/re-uploading every file from the start again.
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
        if [ -f '$remote_path' ]; then
            cp '$remote_path' '$remote_path.bak'
        fi
        mv '$remote_new' '$remote_path'
        rm -rf '$remote_chunk_dir'
    "
    echo "==> $name: deployed to $remote_path (previous copy at $remote_path.bak)"
}

echo "Target: $TARGET"
ssh_do "mkdir -p '$REMOTE_STAGE'"

# 0. Bootstrap: deploy our own md5sum tool (device busybox has none built
#    in) so every subsequent send_file below can verify actual content,
#    not just byte count. Size-only fallback if it's missing/not built.
MD5SUM_BIN="$REPO/userspace/src/md5sum"
if [ -f "$MD5SUM_BIN" ] && [ -n "$MD5SUM_LOCAL" ]; then
    send_file "$MD5SUM_BIN" "/usr/bin/md5sum"
    ssh_do "chmod 0755 /usr/bin/md5sum" || true
fi
if [ "$(ssh_do "if [ -x /usr/bin/md5sum ]; then echo 1; else echo 0; fi")" = "1" ]; then
    HAVE_REMOTE_MD5=1
    echo "==> remote md5sum available -- content-verifying all subsequent transfers"
else
    echo "==> remote md5sum NOT available -- falling back to size-only verification"
fi

# 0b. Force zlib compression for this deploy's writes, regardless of
# whatever JFFS2 compressor mode the CURRENTLY-RUNNING kernel defaults to.
# A `rootflags=compr=zlib` baked into the kernel we're about to *ship* only
# takes effect once that kernel is the one running -- it does nothing for
# the write we are about to perform right now, which happens under
# whichever kernel is already live. Left alone, a live kernel using the
# default CMODE_PRIORITY compressor selection can pick LZO for
# /boot/zImage-full, and the bootstrap kernel (CONFIG_JFFS2_LZO not set,
# see kernel.config-corgi-7.1.4-minimal) can't decompress it --
# "jffs2: compression type 0x07 not available", unbootable until the .bak
# is restored (see docs/DEADLETTER-* for the history of this failure).
if remount_out="$(ssh_do "mount -o remount,compr=zlib / 2>&1")"; then
    echo "==> remounted / with compr=zlib (forces zlib for this deploy's writes)"
    [ -n "$remount_out" ] && echo "    $remount_out"
else
    echo "WARNING: remount compr=zlib on / failed -- the currently-running" >&2
    echo "         kernel may not support the compr= mount option. Writes" >&2
    echo "         below may still pick LZO under CMODE_PRIORITY, which the" >&2
    echo "         bootstrap kernel cannot decompress. Proceeding anyway." >&2
    echo "         remote error: $remount_out" >&2
fi

# 1. Stage-2 kernel (kernel-panic fix)
send_file "$KERNEL_DIR/arch/arm/boot/zImage" "/boot/zImage-full"

if [ "$KERNEL_ONLY" -eq 1 ]; then
    echo "==> --kernel-only: skipping module/script/helper deployment"
    ssh_do "rm -rf '$REMOTE_STAGE'"
    echo "==> done (kernel only)"
    exit 0
fi

# 2. Sound stack modules + helper scripts (matching the kernel just sent)
KVER_LOCAL=""
if [ -f "$KERNEL_DIR/include/config/kernel.release" ]; then
    KVER_LOCAL="$(cat "$KERNEL_DIR/include/config/kernel.release")"
fi
KVER_REMOTE="$(ssh_do 'uname -r')"
echo "==> remote kernel release currently running: $KVER_REMOTE (will change after reboot to the newly deployed kernel)"

. "$REPO/tools/kernel-modules.sh"
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
    ssh_do "mkdir -p '/lib/modules/$KVER_LOCAL/zaurus-audio'"
    send_file "$m" "/lib/modules/$KVER_LOCAL/zaurus-audio/$b"
done

# 3. WiFi/PCMCIA stack modules -- MUST be redeployed in lockstep with every
# kernel rebuild. List + full rationale now lives in tools/kernel-modules.sh
# (shared with flash/build-update-package.sh and flash/build-mtd3-jffs2.sh).
for relpath in $WIFI_MODULES; do
    local_path="$KERNEL_DIR/$(echo "$relpath" | sed 's#^kernel/##')"
    if [ ! -f "$local_path" ]; then
        echo "missing module: $local_path" >&2
        exit 1
    fi
    remote_path="/lib/modules/$KVER_LOCAL/$relpath"
    ssh_do "mkdir -p '$(dirname "$remote_path")'"
    send_file "$local_path" "$remote_path"
done

# 4. SPI stack modules -- needed for the MAX1111 ADC and the touchscreen's
# SPI1 bus. ORDER MATTERS (ssp.ko before spi-pxa2xx-platform.ko). List +
# full rationale now lives in tools/kernel-modules.sh.
for relpath in $SPI_MODULES; do
    local_path="$KERNEL_DIR/$(echo "$relpath" | sed 's#^kernel/##')"
    if [ ! -f "$local_path" ]; then
        echo "missing module: $local_path" >&2
        exit 1
    fi
    remote_path="/lib/modules/$KVER_LOCAL/$relpath"
    ssh_do "mkdir -p '$(dirname "$remote_path")'"
    send_file "$local_path" "$remote_path"
done

# 5. MMC/SD + VFAT stack. List + full rationale now lives in
# tools/kernel-modules.sh.
for relpath in $SD_MODULES; do
    local_path="$KERNEL_DIR/$(echo "$relpath" | sed 's#^kernel/##')"
    if [ ! -f "$local_path" ]; then
        echo "missing module: $local_path" >&2
        exit 1
    fi
    remote_path="/lib/modules/$KVER_LOCAL/$relpath"
    ssh_do "mkdir -p '$(dirname "$remote_path")'"
    send_file "$local_path" "$remote_path"
done

# 6. rcS itself -- carries the insmod calls for the SPI/MMC modules above (and
# is just a plain file on the live, writable jffs2 root, so it can be
# pushed the same way as everything else here; no NAND reflash needed).
send_file "$REPO/rootfs/etc/init.d/rcS" "/etc/init.d/rcS"
ssh_do "chmod 0755 /etc/init.d/rcS"

# 6b. hostap.conf -- sets iw_mode=2 on the correct module (hostap_cs, not
# hostap; iw_mode is declared in hostap_hw.c which hostap_cs.c #includes
# directly). Was previously only ever deployed by hand, never tracked by
# this script -- add it now so future redeploys don't silently regress it.
send_file "$REPO/rootfs/etc/modprobe.d/hostap.conf" "/etc/modprobe.d/hostap.conf"

# 6c. wifi-up.sh -- mdev's $wlan0 rule runs this on card bring-up. Also
# previously only ever deployed by hand; now DHCP-first with a static
# fallback (see the file itself for why). Track it here so it can't
# silently drift from the repo copy.
send_file "$REPO/rootfs/etc/wifi-up.sh" "/etc/wifi-up.sh"
ssh_do "chmod 0755 /etc/wifi-up.sh"

# 7. audioon / audinfo helper scripts (single-word, per AGENTS.md typing
# constraint -- generated inline here as self-contained heredocs; the
# deploy-audio-stack.sh script that originally staged them is gone, so
# this is now the only place they're produced).
cat > "$STAGE/audioon" << 'EOF_AUDIOON'
#!/bin/sh
set -eu
KVER="$(uname -r)"
MD="/lib/modules/${KVER}/zaurus-audio"

load_ko() {
    ko="$1"
    # module names in lsmod always use underscores even when the .ko file
    # on disk uses dashes (e.g. snd-timer.ko -> "snd_timer" in lsmod) --
    # comparing against the raw dashed name here always failed to match,
    # so an already-loaded module would get re-inserted and insmod would
    # fail with "File exists" (found 2026-07-26 while bringing up audio).
    name="$(echo "${ko%.ko}" | tr '-' '_')"
    if lsmod 2>/dev/null | grep -q "^${name} "; then
        return 0
    fi
    insmod "$MD/$ko"
}

load_ko soundcore.ko
load_ko snd.ko
load_ko snd-timer.ko
load_ko snd-pcm.ko
load_ko snd-pcm-dmaengine.ko
load_ko snd-pxa2xx-lib.ko
# snd-soc-core is built with CONFIG_SND_SOC_AC97_BUS=y, so it references
# ac97_bus_type/snd_ac97_reset unconditionally even though this device's
# codec (WM8731) is I2S, not AC97 -- those symbols live in ac97_bus.ko and
# snd-ac97-codec.ko, which must be loaded first or snd-soc-core.ko fails
# with "unknown symbol in module" (found 2026-07-26).
load_ko ac97_bus.ko
load_ko snd-ac97-codec.ko
load_ko snd-soc-core.ko
load_ko snd-soc-pxa2xx.ko
load_ko snd-soc-pxa2xx-i2s.ko
load_ko snd-soc-wm8731.ko
load_ko snd-soc-wm8731-i2c.ko
load_ko snd-soc-corgi.ko
load_ko snd-mixer-oss.ko
load_ko snd-pcm-oss.ko

[ -e /dev/mixer ] || mknod /dev/mixer c 14 0
[ -e /dev/dsp ] || mknod /dev/dsp c 14 3

echo "audio stack loaded"
ls -l /dev/dsp /dev/mixer
EOF_AUDIOON

cat > "$STAGE/audinfo" << 'EOF_AUDINFO'
#!/bin/sh
set -eu
uname -a
echo "-- lsmod (audio) --"
lsmod | grep -E 'snd|wm8731|corgi|regmap|i2c' || true
echo "-- /proc/asound --"
ls -la /proc/asound || true
cat /proc/asound/cards 2>/dev/null || true
echo "-- device nodes --"
ls -l /dev/dsp /dev/mixer 2>/dev/null || true
EOF_AUDINFO

send_file "$STAGE/audioon" "/usr/sbin/audioon"
send_file "$STAGE/audinfo" "/usr/sbin/audinfo"
ssh_do "chmod 0755 /usr/sbin/audioon /usr/sbin/audinfo"

ssh_do "rm -rf '$REMOTE_STAGE'"

echo ""
echo "All files deployed and size-verified. NOT rebooted yet."
echo "Kernel panic fix + sound modules are staged at:"
echo "  /boot/zImage-full        (old copy at /boot/zImage-full.bak)"
echo "  /lib/modules/$KVER_LOCAL/zaurus-audio/*.ko"
echo "  /usr/sbin/audioon, /usr/sbin/audinfo"
echo ""
echo "Reboot manually when ready: ssh -i $KEY $TARGET reboot"
