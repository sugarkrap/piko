#!/bin/sh
set -eu

# Builds everything needed to run piko under QEMU *with a real graphical
# framebuffer*, then boots it. This is the local-development counterpart to
# tools/build-and-deploy.sh: that one puts a kernel on the last spare board,
# this one avoids touching the board at all.
#
# What makes this different from flash/qemu-smoke-test.sh, which also boots
# QEMU: that script is a serial-console CI gate on `-M spitz` (PXA270), and
# deliberately proves only that the *shared* PXA2xx boot path still works --
# it has no display, and `-M spitz` never sets machine_is_corgi()/husky(), so
# corgi.c / corgi_pm.c / w100fb.c never execute there at all. See its header
# and docs/HOWTO-QEMU-SMOKE-TEST.md.
#
# This script boots `-M husky` (or `-M corgi`), which are OUR machines, not
# upstream QEMU's -- upstream has never modelled the Corgi/Husky family. They
# are defined by tools/qemu-corgi/corgi.c together with tools/qemu-corgi/w100.c,
# a model of the ATI Imageon W100 companion chip that actually drives the panel
# on SL-C7xx/SL-C860 hardware. The unmodified in-tree w100fb driver probes it,
# registers fb0, and paints -- so this exercises w100fb.c, the real board
# geometry, and anything drawing to /dev/fb0 (otXash, otQuake, otCraft) rather
# than just the shared boot path.
#
# Those two .c files live under tools/qemu-corgi/ and are injected into the
# QEMU source tree on every run, on purpose: qemu-spitz/ is gitignored
# (.gitignore:13) because it is a 130 MB upstream tarball, so anything left
# only in there is one `rm -rf` from being gone. Keep editing them here, not
# in qemu-spitz/hw/.
#
# Usage:
#   tools/build-and-emulate.sh [--machine husky|corgi|spitz] [--console fb|serial]
#                              [--kernel PATH] [--initrd PATH]
#                              [--screenshot FILE] [--delay N] [--timeout N]
#                              [--rebuild-qemu] [--rebuild-kernel] [--build-only]
# Examples:
#   tools/build-and-emulate.sh                          # boot husky in a window
#   tools/build-and-emulate.sh --screenshot /tmp/s.png  # headless, capture, exit
#   tools/build-and-emulate.sh --machine spitz --console serial
#
# --console fb (default) boots with console=tty0, i.e. output goes to the
# emulated panel through fbcon -- what you want when the point is to LOOK at
# something. --console serial boots console=ttyS0 and dumps the kernel log to
# stdout instead, which is what you want when the point is to READ something.
# Deliberately either/or: passing both "console=ttyS0 console=tty0" together
# was observed to produce NO output on either console, so this script will not
# generate that combination.
# --screenshot FILE captures the guest's framebuffer to a .png/.ppm via QMP and
# exits, with no window and no window manager involved. This is the reliable
# way to see what the guest is drawing (and the only one available headless or
# from CI); grabbing the SDL window off the host desktop instead is at the
# mercy of whatever is stacked on top of it.
# --delay N is how long to let the guest boot before that capture (default 12).
# The guest reaches /init in ~2 s and fb0 registers at ~1.4 s, so this is
# mostly margin -- the emulated CPU is slow and a loaded host is slower.
# --build-only builds QEMU and the kernel variant and stops without booting.
#
# THE KERNEL VARIANT, AND WHY THIS SCRIPT PUTS .config BACK:
# `-append` is silently discarded unless CONFIG_CMDLINE_FORCE is off, and the
# shipped .config has it ON (it is required on real hardware, which has no
# serial port and must force console=tty0). So a QEMU-bootable kernel needs a
# *different* .config, and a tree left in that state builds a kernel that must
# never be flashed. This script therefore backs .config up, builds, and
# restores it on EXIT/INT/TERM -- including when the build fails. The variant
# lands in a clearly-named file next to the repo root, never in place of the
# real one. Same reasoning as flash/qemu-smoke-test.sh's own restore step.
#
# THE QEMU BINARY:
# Must be 9.1.0, built from source into qemu-spitz/. The spitz/akita/borzoi/
# terrier machines were removed from upstream QEMU in 9.2, and our corgi/husky
# machines are built on the same PXA2xx support, so a distro qemu-system-arm
# will not do. Two build-time snags, both handled below and both the same
# "2024 source vs. current host tooling" shape: mkvenv's bootstrap needs
# python3-distlib (installed into a throwaway venv rather than system-wide),
# and block/nfs.c no longer compiles against modern libnfs (--disable-libnfs,
# since nothing here wants an NFS disk backend).

REPO="$(cd "$(dirname "$0")/.." && pwd)"

MACHINE=husky
CONSOLE=fb
KERNEL=
INITRD=
SD=
SCREENSHOT=
DELAY=12
TIMEOUT=
REBUILD_QEMU=0
REBUILD_KERNEL=0
BUILD_ONLY=0

QEMU_VERSION=9.1.0
QEMU_DIR="$REPO/qemu-spitz"
QEMU_BIN="$QEMU_DIR/build/qemu-system-arm"
QEMU_TARBALL_URL="https://download.qemu.org/qemu-$QEMU_VERSION.tar.xz"

while [ $# -gt 0 ]; do
    case "$1" in
        --machine)      MACHINE="$2"; shift 2 ;;
        --console)      CONSOLE="$2"; shift 2 ;;
        --kernel)       KERNEL="$2"; shift 2 ;;
        --initrd)       INITRD="$2"; shift 2 ;;
        --sd)           SD="$2"; shift 2 ;;
        --screenshot)   SCREENSHOT="$2"; shift 2 ;;
        --delay)        DELAY="$2"; shift 2 ;;
        --timeout)      TIMEOUT="$2"; shift 2 ;;
        --rebuild-qemu)   REBUILD_QEMU=1; shift ;;
        --rebuild-kernel) REBUILD_KERNEL=1; shift ;;
        --build-only)   BUILD_ONLY=1; shift ;;
        -h|--help)      sed -n '3,75p' "$0"; exit 0 ;;
        *) echo "$0: unknown argument '$1' (try --help)" >&2; exit 1 ;;
    esac
done

case "$MACHINE" in
    husky|corgi|spitz) ;;
    *) echo "$0: --machine must be husky, corgi or spitz" >&2; exit 1 ;;
esac
case "$CONSOLE" in
    fb|serial) ;;
    *) echo "$0: --console must be fb or serial" >&2; exit 1 ;;
esac

# piko's own crosstool-NG toolchain if it is staged, else whatever is on PATH.
CROSS_DEFAULT="$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin/arm-unknown-linux-uclibcgnueabi-"
if [ -x "${CROSS_DEFAULT}gcc" ]; then
    CROSS_COMPILE="${CROSS_COMPILE:-$CROSS_DEFAULT}"
else
    CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabi-}"
fi

KERNEL_DIR="${KERNEL_DIR:-$(ls -d "$REPO"/kernel-src/linux-* 2>/dev/null | head -n1 || true)}"
ZIMAGE_QEMU="$REPO/zImage-qemu-testing"
[ -n "$INITRD" ] || INITRD="$REPO/initramfs/initramfs-minimal.cpio.gz"

CONFIG_BACKUP=
restore_config() {
    if [ -n "$CONFIG_BACKUP" ] && [ -f "$CONFIG_BACKUP" ]; then
        cp "$CONFIG_BACKUP" "$KERNEL_DIR/.config"
        rm -f "$CONFIG_BACKUP"
        echo "==> restored the real-hardware .config"
    fi
}
trap restore_config EXIT INT TERM

# --- QEMU -------------------------------------------------------------------

if [ ! -f "$QEMU_DIR/configure" ]; then
    echo "==> fetching QEMU $QEMU_VERSION source (the last release with PXA2xx machines)"
    mkdir -p "$QEMU_DIR"
    curl -L --progress-bar -o "$QEMU_DIR/qemu.tar.xz" "$QEMU_TARBALL_URL"
    tar xf "$QEMU_DIR/qemu.tar.xz" -C "$QEMU_DIR" --strip-components=1
    rm -f "$QEMU_DIR/qemu.tar.xz"
fi

# Inject our device model + machine, and register them in the build system.
# Idempotent: safe to re-run, and it is what makes a fresh qemu-spitz/ usable.
echo "==> injecting tools/qemu-corgi/{w100.c,corgi.c} into the QEMU tree"
# Copy only when the contents actually differ: an unconditional cp bumps the
# mtime, which makes ninja relink a 100 MB binary on every single run.
inject() {
    cmp -s "$1" "$2" || cp "$1" "$2"
}
inject "$REPO/tools/qemu-corgi/w100.c"  "$QEMU_DIR/hw/display/w100.c"
inject "$REPO/tools/qemu-corgi/corgi.c" "$QEMU_DIR/hw/arm/corgi.c"

if ! grep -q '^config W100' "$QEMU_DIR/hw/display/Kconfig"; then
    printf '\nconfig W100\n    bool\n    select FRAMEBUFFER\n' >> "$QEMU_DIR/hw/display/Kconfig"
fi
if ! grep -q "CONFIG_W100" "$QEMU_DIR/hw/display/meson.build"; then
    printf "system_ss.add(when: 'CONFIG_W100', if_true: files('w100.c'))\n" \
        >> "$QEMU_DIR/hw/display/meson.build"
fi
if ! grep -q '^config CORGI' "$QEMU_DIR/hw/arm/Kconfig"; then
    printf '\nconfig CORGI\n    bool\n    default y\n    depends on TCG && ARM\n    select W100\n    select ZAURUS  # scoop\n' \
        >> "$QEMU_DIR/hw/arm/Kconfig"
fi
if ! grep -q "CONFIG_CORGI" "$QEMU_DIR/hw/arm/meson.build"; then
    printf "system_ss.add(when: 'CONFIG_CORGI', if_true: files('corgi.c'))\n" \
        >> "$QEMU_DIR/hw/arm/meson.build"
fi

# Upstream fix, applied in place rather than shipped as a .patch so it cannot
# go stale against a different QEMU tarball: pxa2xx_mmci drives its own DMA,
# which re-enters its own MMIO region mid-transfer. QEMU's generic
# re-entrancy guard blocks that ("Blocked re-entrant IO on MemoryRegion:
# pxa2xx-mmci"), and the block CORRUPTS the transfer rather than just slowing
# it -- Linux reads back a garbage SCR and rejects the card with
# "mmc0: invalid bus width" / -EINVAL, so an SD root cannot be mounted at all.
# lsi53c895a opts out of the same guard for the same reason.
if ! grep -q "disable_reentrancy_guard" "$QEMU_DIR/hw/sd/pxa2xx_mmci.c"; then
    echo "==> patching pxa2xx_mmci re-entrancy guard (needed for SD boot)"
    python3 - "$QEMU_DIR/hw/sd/pxa2xx_mmci.c" <<'PY'
import sys
p = sys.argv[1]
src = open(p).read()
anchor = '                          "pxa2xx-mmci", 0x00100000);\n'
add = anchor + "    s->iomem.disable_reentrancy_guard = true;\n"
if anchor not in src:
    sys.exit("build-and-emulate.sh: could not find the pxa2xx-mmci region init to patch")
open(p, "w").write(src.replace(anchor, add, 1))
PY
fi

if [ ! -f "$QEMU_DIR/build/build.ninja" ] || [ "$REBUILD_QEMU" = 1 ]; then
    echo "==> configuring QEMU (arm-softmmu only; --disable-libnfs, see header)"
    ( cd "$QEMU_DIR"
      if [ ! -x pyvenv-bootstrap/bin/python3 ]; then
          python3 -m venv pyvenv-bootstrap
          pyvenv-bootstrap/bin/pip install --quiet distlib
      fi
      ./configure --python="$PWD/pyvenv-bootstrap/bin/python3" \
                  --disable-libnfs --target-list=arm-softmmu >/dev/null )
fi

echo "==> building qemu-system-arm"
( cd "$QEMU_DIR/build" && ninja qemu-system-arm >/dev/null )

if ! "$QEMU_BIN" -M help | grep -q "^$MACHINE "; then
    echo "$0: built QEMU has no '-M $MACHINE' machine" >&2
    exit 1
fi

# --- kernel -----------------------------------------------------------------

if [ ! -f "$ZIMAGE_QEMU" ] || [ "$REBUILD_KERNEL" = 1 ]; then
    [ -n "$KERNEL_DIR" ] && [ -f "$KERNEL_DIR/.config" ] || {
        echo "$0: no configured kernel tree under $REPO/kernel-src (run flash/setup-kernel-src.sh)" >&2
        exit 1
    }
    echo "==> building the QEMU kernel variant (CMDLINE_FORCE off, MACH_SPITZ on)"
    CONFIG_BACKUP="$KERNEL_DIR/.config.real-hardware-backup"
    cp "$KERNEL_DIR/.config" "$CONFIG_BACKUP"
    ( cd "$KERNEL_DIR"
      # MACH_SPITZ is only needed for -M spitz, but costs nothing to leave in
      # and keeps one variant image usable for all three machines.
      ./scripts/config --enable MACH_SPITZ \
                       --set-str CMDLINE "console=ttyS0 earlyprintk" \
                       --disable CMDLINE_FORCE
      make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" olddefconfig >/dev/null
      make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)" zImage >/dev/null )
    cp "$KERNEL_DIR/arch/arm/boot/zImage" "$ZIMAGE_QEMU"
    restore_config
    CONFIG_BACKUP=
    # arch/arm/boot/zImage is now the QEMU variant and no longer matches the
    # restored .config; say so rather than leave a booby-trapped artifact.
    echo "==> NOTE: $KERNEL_DIR/arch/arm/boot/zImage is the QEMU variant."
    echo "    Re-run 'make zImage' before flashing anything to real hardware."
fi

[ -n "$KERNEL" ] || KERNEL="$ZIMAGE_QEMU"

if [ "$BUILD_ONLY" = 1 ]; then
    echo "==> built: $QEMU_BIN"
    echo "==> built: $KERNEL"
    exit 0
fi

# --- boot -------------------------------------------------------------------

[ -f "$KERNEL" ] || { echo "$0: no kernel at $KERNEL" >&2; exit 1; }

if [ "$CONSOLE" = serial ]; then
    APPEND="console=ttyS0 earlyprintk"
else
    APPEND="console=tty0"
fi

# Root comes either from an SD image (tools/build-emulator-image.sh) or, by
# default, from the throwaway initramfs. QEMU rejects an SD image whose size
# is not a power of two, so say so plainly rather than letting it fail deep in
# block-layer setup.
BOOTMEDIA=""
if [ -n "$SD" ]; then
    [ -f "$SD" ] || { echo "$0: no SD image at $SD" >&2; exit 1; }
    sd_bytes=$(stat -c %s "$SD")
    if [ $(( sd_bytes & (sd_bytes - 1) )) -ne 0 ]; then
        echo "$0: SD image size must be a power of two (is $sd_bytes bytes);" >&2
        echo "    fix with: truncate -s 512M $SD" >&2
        exit 1
    fi
    BOOTMEDIA="sd"
    APPEND="root=/dev/mmcblk0 rw rootfstype=ext2 init=/sbin/init $APPEND"
else
    [ -f "$INITRD" ] || { echo "$0: no initramfs at $INITRD (tools/build-initramfs.sh)" >&2; exit 1; }
    BOOTMEDIA="initrd"
fi

echo "==> booting -M $MACHINE (console=$CONSOLE, root=$BOOTMEDIA)"

# The media arguments, shared by every launch path below.
if [ "$BOOTMEDIA" = sd ]; then
    # snapshot=on is not an optimisation, it is what makes repeat runs mean
    # anything: the guest mounts this root read-write, and killing the
    # emulator (timeout, Ctrl-C, CI cancelling the job) leaves a non-journalled
    # ext2 with half-written metadata. Observed first-hand -- an image that had
    # booted fine came back with every file mode reset, so init could no longer
    # exec /etc/init.d/xsession. With snapshot=on every write lands in a
    # throwaway overlay and the image on disk is never touched.
    set -- -drive "if=sd,format=raw,file=$SD,snapshot=on"
else
    set -- -initrd "$INITRD"
fi
# A fixed RTC keeps anything clock-driven in the guest reproducible, which is
# what makes screenshot comparison viable at all.
set -- "$@" -rtc base=2020-01-01T00:00:00
MEDIA_ARGS="$*"

if [ -n "$SCREENSHOT" ]; then
    # Headless capture. -display none still lets QMP screendump work, because
    # screendump asks the console to refresh itself rather than relying on a
    # display backend's timer.
    QMP_SOCK="$(mktemp -u /tmp/piko-emulate-qmp.XXXXXX)"
    [ -n "$TIMEOUT" ] || TIMEOUT=$((DELAY + 20))
    # shellcheck disable=SC2086  # MEDIA_ARGS is deliberately word-split
    timeout "$TIMEOUT" "$QEMU_BIN" -M "$MACHINE" \
        -kernel "$KERNEL" $MEDIA_ARGS -append "$APPEND" \
        -display none -qmp "unix:$QMP_SOCK,server,nowait" &
    QEMU_PID=$!
    python3 - "$QMP_SOCK" "$SCREENSHOT" "$DELAY" <<'PY'
import json, socket, sys, time
sock_path, out, delay = sys.argv[1], sys.argv[2], float(sys.argv[3])
for _ in range(100):                       # wait for the socket to appear
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect(sock_path); break
    except OSError:
        time.sleep(0.1)
else:
    sys.exit("could not reach the QEMU monitor")
f = s.makefile("rwb")
def reply():
    while True:
        line = f.readline()
        if not line:
            return None
        msg = json.loads(line)
        if "event" not in msg:
            return msg
reply()
f.write(b'{"execute":"qmp_capabilities"}\n'); f.flush(); reply()
time.sleep(delay)
fmt = "png" if out.lower().endswith(".png") else "ppm"
f.write(json.dumps({"execute": "screendump", "arguments": {
    "filename": out, "device": "w100", "format": fmt}}).encode() + b"\n")
f.flush()
r = reply()
if r is None or "error" in (r or {}):
    sys.exit("screendump failed: %s" % r)
f.write(b'{"execute":"quit"}\n'); f.flush()
PY
    wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$QMP_SOCK"
    echo "==> wrote $SCREENSHOT"
    exit 0
fi

# shellcheck disable=SC2086  # MEDIA_ARGS is deliberately word-split
set -- -M "$MACHINE" -kernel "$KERNEL" $MEDIA_ARGS -append "$APPEND"
if [ "$CONSOLE" = serial ]; then
    set -- "$@" -serial stdio -display none -monitor none
else
    set -- "$@" -display sdl
fi

if [ -n "$TIMEOUT" ]; then
    timeout "$TIMEOUT" "$QEMU_BIN" "$@" || true
else
    "$QEMU_BIN" "$@"
fi
