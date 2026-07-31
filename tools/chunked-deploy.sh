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
#   tools/chunked-deploy.sh [--adapter IFACE] [--target user@host] [--kernel-only] [--no-userspace] [user@host]
# Example:
#   tools/chunked-deploy.sh --adapter wlan0 root@10.43.112.72
#
# --adapter IFACE binds the SSH connection to a specific local network
# interface (ssh -B), useful when the build machine has multiple network
# adapters and the Zaurus is only reachable via one of them.
# --target user@host sets the SSH destination explicitly. A positional
# user@host is also accepted for backwards compatibility.
# --no-userspace skips the MPlayer + ALSA-runtime + SDL payload (section 8).
# That payload is also skipped automatically when the staged trees do not
# exist or when the device lacks free space -- see section 8 for why the
# space check refuses rather than half-deploying.
# --kernel-only skips every module/script/helper deployment step below and
# only ships /boot/zImage-full (still preceded by the md5sum bootstrap).
# Useful when iterating on a kernel-only change where redeploying modules
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
NO_USERSPACE=0
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
        --no-userspace)
            NO_USERSPACE=1
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
# Staged userspace payloads produced by tools/build-userspace.sh (absent on
# a clean checkout that has not built them yet -- section 8 skips silently).
MPLAYER_STAGE="${MPLAYER_STAGE:-$REPO/userspace/stage-mplayer}"
ALSA_STAGE="${ALSA_STAGE:-$REPO/userspace/stage-alsa-runtime}"
SDL_STAGE="${SDL_STAGE:-$REPO/userspace/stage-sdl-runtime}"
# Toolchain sysroot copy of the uClibc dynamic linker -- only needed for
# SDL (see tools/build-sdl.sh's header for why it alone, unlike the rest
# of this static userland, is a shared library).
HOST_TRIPLET="${CROSS_HOST:-arm-unknown-linux-uclibcgnueabi}"
TCROOT="${TCROOT:-$REPO/toolchain/x-tools/$HOST_TRIPLET/$HOST_TRIPLET/sysroot}"
# Heavy software lives on the SD card, not on the ~68 MiB root jffs2.
# /mnt/card/.zaurus is the hidden overlay described in
# rootfs/etc/zaurus-card.sh; its usr/bin is already on PATH (unconditionally,
# so it costs nothing when no card is inserted). MPlayer alone is ~16 MiB,
# which is a quarter of the root filesystem.
#
# Override MPLAYER_DEST to put it somewhere else (e.g. /usr/bin/mplayer to
# force it onto flash anyway). A destination under /mnt/ is not charged
# against the root filesystem budget.
CARD_ROOT="${CARD_ROOT:-/mnt/card/.zaurus}"
MPLAYER_DEST="${MPLAYER_DEST:-$CARD_ROOT/usr/bin/mplayer}"
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

# 6a. Graphical session: xsession brings up Xfbdev -> Zaurus keymap ->
# Matchbox, and inittab runs it on tty1 in place of the getty, so the
# device boots to a desktop rather than an ash prompt. xsession falls
# back to a getty itself on every failure path, so shipping these cannot
# strand the machine at a blank screen -- and dropbear starts earlier in
# rcS, so SSH is up either way.
#
# Deployed BEFORE the X11 payload below on purpose: if the payload
# transfer dies partway over this flaky link, xsession finds no Xfbdev
# and drops to a console instead of init respawning into a broken state.
send_file "$REPO/rootfs/etc/init.d/xsession" "/etc/init.d/xsession"
ssh_do "chmod 0755 /etc/init.d/xsession"
send_file "$REPO/rootfs/etc/inittab" "/etc/inittab"

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
# constraint). These are TRACKED FILES under rootfs/usr/sbin/ -- send them
# verbatim rather than regenerating them from heredocs here.
#
# They used to be inline heredocs in this script, which quietly created a
# second source of truth: rootfs/usr/sbin/audioon and the heredoc drifted
# apart (the heredoc grew the /dev/dsp + /dev/mixer mknod lines, the tracked
# file grew mixer documentation), and an edit committed to the tracked file
# was silently overwritten on the next full deploy by the stale heredoc.
# Found 2026-07-30. Single source of truth now: rootfs/usr/sbin/.
send_file "$REPO/rootfs/usr/sbin/audioon" "/usr/sbin/audioon"
send_file "$REPO/rootfs/usr/sbin/audinfo" "/usr/sbin/audinfo"
ssh_do "chmod 0755 /usr/sbin/audioon /usr/sbin/audinfo"

# 7b. SD-card software overlay. /etc/zaurus-card.sh puts
# /mnt/card/.zaurus/usr/bin on PATH (unconditionally -- a PATH element that
# does not exist is simply skipped, so this costs nothing with no card in,
# and a shell started before insertion still finds card software after).
# profile/zshrc source it so ash and zsh behave identically; sdapps
# reports what a card provides.
send_file "$REPO/rootfs/etc/zaurus-card.sh" "/etc/zaurus-card.sh"
send_file "$REPO/rootfs/etc/profile"        "/etc/profile"
send_file "$REPO/rootfs/etc/zshrc"          "/etc/zshrc"
send_file "$REPO/rootfs/usr/sbin/sdapps"  "/usr/sbin/sdapps"
ssh_do "chmod 0644 /etc/zaurus-card.sh /etc/profile /etc/zshrc"
ssh_do "chmod 0755 /usr/sbin/sdapps"

# 8. Userspace media payload: MPlayer + the ALSA runtime config tree + SDL.
#
# Skipped entirely with --no-userspace (or when the staged trees are absent,
# e.g. a clean checkout that has not run tools/build-userspace.sh yet).
#
# What actually has to ship, and why it is so short:
#   * mplayer          -- fully static (no NEEDED entries at all), so there
#                         are no libraries to ship alongside it.
#   * /usr/share/alsa  -- REQUIRED even though libasound is linked
#                         statically into mplayer/aplay: alsa-lib opens
#                         alsa.conf at runtime by absolute path. Without it
#                         every PCM open fails with
#                         "Unknown PCM cards.pcm.default".
#   * aplay/amixer/alsactl -- also static; small, and the only way to test
#                         or adjust the audio path on the device.
#   * libSDL-1.2.so.0 + sdltest + pikalibrate -- SDL is dynamically linked
#                         (the one exception to this section's static
#                         convention -- see tools/build-sdl.sh's header for
#                         why), so shipping it also bootstraps
#                         /lib/ld-uClibc*.so + /lib/libc.so onto the device
#                         if not already there (see the SDL block below for
#                         the executable-bit gotcha that bootstrap has to
#                         get right, found on real hardware). pikalibrate
#                         also gets /etc/piko/touchscreen.cfg pushed, but
#                         ONLY if the device doesn't already have one --
#                         it holds real calibration state once run, not a
#                         file this script should ever clobber.
# Only the config files this board can actually use are sent (the upstream
# tree also carries ~70 cards/*.conf for hardware that does not exist here);
# each send_file is several SSH round trips over a slow, flaky link.
if [ "$NO_USERSPACE" -eq 0 ] && [ -d "$MPLAYER_STAGE" -o -d "$ALSA_STAGE" -o -d "$SDL_STAGE" ]; then
    # Preflight: JFFS2 needs free space to garbage-collect. Filling the root
    # filesystem on this board is not a recoverable mistake over SSH, so
    # refuse up front instead of half-deploying and wedging it.
    need=0
    # Only count MPlayer against the root filesystem if that is where it is
    # actually going; an SD-card destination costs the root nothing.
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
        # Explicit if, not `[ -f ] && ...`: under `set -e` a false test at
        # the end of a loop body makes the loop return non-zero and aborts
        # the whole deploy.
        if [ -f "$f" ]; then
            need=$((need + $(wc -c < "$f")))
        fi
    done
    need_kb=$(((need / 1024) + 512))          # + slack for the config tree
    avail_kb="$(ssh_do "df /usr | tail -n 1" | awk '{print $4}')"
    case "$avail_kb" in ''|*[!0-9]*) avail_kb=0 ;; esac

    echo "==> userspace payload: needs ~${need_kb} KiB, device has ${avail_kb} KiB free on /"
    if [ "$avail_kb" -gt 0 ] && [ "$need_kb" -gt "$((avail_kb - 4096))" ]; then
        echo "SKIPPING userspace payload: not enough free space." >&2
        echo "  Leaving at least 4 MiB headroom on the root jffs2 is deliberate --" >&2
        echo "  it needs room to garbage-collect, and a full root is not something" >&2
        echo "  you can recover from over SSH on this board." >&2
        echo "  Free space first, or stage MPlayer on the SD card instead:" >&2
        echo "    ssh $TARGET 'mount /mnt/card'" >&2
        echo "    ssh $TARGET 'cat > /mnt/card/mplayer' < $MPLAYER_STAGE/usr/bin/mplayer" >&2
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
            for b in bin/aplay bin/amixer sbin/alsactl; do
                [ -f "$ALSA_STAGE/usr/$b" ] || continue
                send_file "$ALSA_STAGE/usr/$b" "/usr/$b"
                ssh_do "chmod 0755 /usr/$b"
            done
        fi
        # SDL: small (a few hundred KiB), always goes to the root
        # filesystem like ALSA above, never card-gated like MPlayer.
        if [ -n "$SDL_SO_REAL" ]; then
            if [ -f "$TCROOT/lib/ld-uClibc-1.0.54.so" ] && [ -f "$TCROOT/lib/libuClibc-1.0.54.so" ]; then
                send_file "$TCROOT/lib/ld-uClibc-1.0.54.so" "/lib/ld-uClibc-1.0.54.so"
                send_file "$TCROOT/lib/libuClibc-1.0.54.so" "/lib/libuClibc-1.0.54.so"
                # `cat > file` (send_file's transfer mechanism) creates the
                # remote file with the default umask, NOT the source's
                # executable bit -- confirmed on real hardware to land as
                # 644. That's silently fatal for ld-uClibc-1.0.54.so
                # specifically: the kernel's ELF loader opens the
                # PT_INTERP target via the same open_exec() path (and
                # therefore the same MAY_EXEC permission check) it uses
                # for the top-level binary, so a non-executable dynamic
                # linker makes EVERY dynamically-linked binary on the
                # device fail execve() with EACCES ("Permission denied"),
                # not just SDL's -- this silently broke
                # matchbox-remote/Xfbdev too until caught here.
                ssh_do "chmod 0755 /lib/ld-uClibc-1.0.54.so /lib/libuClibc-1.0.54.so"
                ssh_do "
                    set -e
                    ln -sf ld-uClibc-1.0.54.so /lib/ld-uClibc.so.1
                    ln -sf ld-uClibc.so.1 /lib/ld-uClibc.so.0
                    ln -sf libuClibc-1.0.54.so /lib/libc.so.0
                    ln -sf libuClibc-1.0.54.so /lib/libc.so.1
                "
            else
                echo "==> WARNING: toolchain sysroot runtime libs missing under $TCROOT/lib" >&2
                echo "    -- cannot bootstrap dynamic linking; SDL will only run if a" >&2
                echo "    previous deploy already put ld-uClibc/libc.so on the device." >&2
            fi
            send_file "$SDL_STAGE/usr/lib/$SDL_SO_REAL" "/usr/lib/$SDL_SO_REAL"
            ssh_do "ln -sf '$SDL_SO_REAL' /usr/lib/libSDL-1.2.so.0"
            if [ -f "$SDL_STAGE/usr/bin/sdltest" ]; then
                send_file "$SDL_STAGE/usr/bin/sdltest" "/usr/bin/sdltest"
                ssh_do "chmod 0755 /usr/bin/sdltest"
            fi
            if [ -f "$SDL_STAGE/usr/bin/pikalibrate" ]; then
                send_file "$SDL_STAGE/usr/bin/pikalibrate" "/usr/bin/pikalibrate"
                ssh_do "chmod 0755 /usr/bin/pikalibrate"
            fi
            # touchscreen.cfg holds USER-CALIBRATED STATE once pikalibrate has
            # been run -- unlike every other rootfs/etc/* file in this script,
            # it must NOT be unconditionally overwritten on a routine
            # redeploy, or a real calibration would get silently reset back
            # to the tracked defaults.
            if [ -f "$REPO/rootfs/etc/piko/touchscreen.cfg" ]; then
                if [ "$(ssh_do "test -f /etc/piko/touchscreen.cfg && echo yes || echo no")" = "no" ]; then
                    ssh_do "mkdir -p /etc/piko"
                    send_file "$REPO/rootfs/etc/piko/touchscreen.cfg" "/etc/piko/touchscreen.cfg"
                else
                    echo "==> /etc/piko/touchscreen.cfg already exists on device, leaving it alone"
                fi
            fi
        fi
        # Heavy apps: card-only. If there is no card, skip them rather than
        # falling back to flash -- that is the whole point of the split.
        if [ -f "$MPLAYER_STAGE/usr/bin/mplayer" ]; then
            case "$MPLAYER_DEST" in
            /mnt/card/*)
                # Corgi's pxamci card-detect does not reliably signal
                # insertion, so /mnt/card is frequently NOT mounted even
                # with a card physically present. Writing to an unmounted
                # /mnt/card silently lands on the root jffs2 -- a 16 MiB
                # file in exactly the place we are trying to protect -- so
                # mount first and then VERIFY, rather than trusting it.
                ssh_do "mount /mnt/card 2>/dev/null || true"
                if ssh_do "grep -q ' /mnt/card ' /proc/mounts && echo yes || echo no" | grep -q yes; then
                    ssh_do "mkdir -p '$(dirname "$MPLAYER_DEST")'"
                    send_file "$MPLAYER_STAGE/usr/bin/mplayer" "$MPLAYER_DEST"
                    ssh_do "chmod 0755 '$MPLAYER_DEST'"
                else
                    echo "==> no SD card mounted -- SKIPPING heavy apps (MPlayer)."
                    echo "    Heavy software is card-only by design; the root jffs2"
                    echo "    has no room for it. Insert a card and re-run, or set"
                    echo "    MPLAYER_DEST=/usr/bin/mplayer to force it onto flash."
                fi
                ;;
            *)
                send_file "$MPLAYER_STAGE/usr/bin/mplayer" "$MPLAYER_DEST"
                ssh_do "chmod 0755 '$MPLAYER_DEST'"
                ;;
            esac
        fi
    fi
fi

ssh_do "rm -rf '$REMOTE_STAGE'"

# 9. X11 + Matchbox desktop payload.
#
# Shipped as ONE tar and unpacked on the device with our own untar
# (userspace/src/untar.c): the stack is ~400 files, and that many
# individual send_file round trips over this link would be brutal. The
# device's busybox has no tar at all, which is precisely why untar exists.
#
# Built by tools/build-matchbox-payload.sh, which also verifies every
# DT_NEEDED is satisfied and every ELF is really ARM before packing.
# Skipped (with a note, not an error) when the tar has not been built --
# a kernel-only redeploy should not have to build all of X first.
X11_PAYLOAD="${X11_PAYLOAD:-/tmp/matchbox-payload.tar}"
if [ "$KERNEL_ONLY" -eq 0 ] && [ -f "$X11_PAYLOAD" ]; then
    echo "==> X11/Matchbox payload ($(wc -c < "$X11_PAYLOAD") bytes)"
    if [ "$(ssh_do "if [ -x /usr/local/bin/untar ]; then echo 1; else echo 0; fi")" != "1" ]; then
        # untar is a small static binary with no dependencies, so it can
        # always be (re)built and pushed even on a bare device.
        echo "FAILED: /usr/local/bin/untar missing on the device." >&2
        echo "Build it and push it first:" >&2
        echo "  \$GCC -march=armv5te -O2 -static -o untar userspace/src/untar.c" >&2
        exit 1
    fi
    send_file "$X11_PAYLOAD" "/tmp/x11-payload.tar"
    # A running X server holds its own binary open, so unpacking over it
    # fails with "Text file busy" and the payload only half-lands. Stop
    # the session first -- we are replacing exactly those binaries.
    # Clients before the server, so the WM is not killed out from under
    # its display. No pkill/awk here (busybox is minimal), so PIDs are
    # read out of ps with the shell; `kill` is our own static one from
    # userspace/src/kill.c, since this busybox has no kill applet either.
    echo "==> stopping any running graphical session"
    ssh_do "for p in \$(ps | grep -E 'matchbox|xev' | grep -v grep | while read a b; do echo \$a; done); do /usr/local/bin/kill -15 \$p 2>/dev/null; done; sleep 2" || true
    ssh_do "for p in \$(ps | grep Xfbdev | grep -v grep | while read a b; do echo \$a; done); do /usr/local/bin/kill -15 \$p 2>/dev/null; done; sleep 2" || true
    ssh_do "/usr/local/bin/untar /tmp/x11-payload.tar / && rm -f /tmp/x11-payload.tar"
    echo "==> X11/Matchbox stack unpacked"
    echo "    (session stopped for the update; it restarts on reboot,"
    echo "     or run: DISPLAY=:0 matchbox-session &)"
elif [ "$KERNEL_ONLY" -eq 0 ]; then
    echo "==> no X11 payload at $X11_PAYLOAD -- skipping"
    echo "    (build it with tools/build-matchbox-payload.sh)"
fi

echo ""
echo "All files deployed and size-verified. NOT rebooted yet."
echo "Kernel panic fix + sound modules are staged at:"
echo "  /boot/zImage-full        (old copy at /boot/zImage-full.bak)"
echo "  /lib/modules/$KVER_LOCAL/zaurus-audio/*.ko"
echo "  /usr/sbin/audioon, /usr/sbin/audinfo"
if [ "$NO_USERSPACE" -eq 0 ] && [ -f "$MPLAYER_STAGE/usr/bin/mplayer" ]; then
    echo "  $MPLAYER_DEST + /usr/share/alsa + aplay/amixer/alsactl (if space allowed)"
fi
if [ "$NO_USERSPACE" -eq 0 ] && [ -d "$SDL_STAGE/usr/lib" ]; then
    echo "  /usr/lib/libSDL-1.2.so.0 + /usr/bin/sdltest + /usr/bin/pikalibrate (if staged)"
fi
echo ""
echo "Reboot manually when ready: ssh -i $KEY $TARGET reboot"
