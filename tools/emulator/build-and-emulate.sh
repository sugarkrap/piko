#!/bin/sh
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"

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

CROSS_DEFAULT="$REPO/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin/arm-unknown-linux-uclibcgnueabi-"
if [ -x "${CROSS_DEFAULT}gcc" ]; then
    CROSS_COMPILE="${CROSS_COMPILE:-$CROSS_DEFAULT}"
else
    CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabi-}"
fi

KERNEL_DIR="${KERNEL_DIR:-$(ls -d "$REPO"/build/kernel/src/linux-* 2>/dev/null | head -n1 || true)}"
ZIMAGE_QEMU="$REPO/zImage-qemu-testing"
[ -n "$INITRD" ] || INITRD="$REPO/build/initramfs/initramfs-minimal.cpio.gz"

CONFIG_BACKUP=
restore_config() {
    if [ -n "$CONFIG_BACKUP" ] && [ -f "$CONFIG_BACKUP" ]; then
        cp "$CONFIG_BACKUP" "$KERNEL_DIR/.config"
        rm -f "$CONFIG_BACKUP"
        echo "==> restored the real-hardware .config"
    fi
}
trap restore_config EXIT INT TERM

if [ ! -f "$QEMU_DIR/configure" ]; then
    echo "==> fetching QEMU $QEMU_VERSION source (the last release with PXA2xx machines)"
    mkdir -p "$QEMU_DIR"
    curl -L --progress-bar -o "$QEMU_DIR/qemu.tar.xz" "$QEMU_TARBALL_URL"
    tar xf "$QEMU_DIR/qemu.tar.xz" -C "$QEMU_DIR" --strip-components=1
    rm -f "$QEMU_DIR/qemu.tar.xz"
fi

echo "==> injecting tools/emulator/qemu-corgi/{w100.c,corgi.c} into the QEMU tree"
inject() {
    cmp -s "$1" "$2" || cp "$1" "$2"
}
inject "$REPO/tools/emulator/qemu-corgi/w100.c"  "$QEMU_DIR/hw/display/w100.c"
inject "$REPO/tools/emulator/qemu-corgi/corgi.c" "$QEMU_DIR/hw/arm/corgi.c"

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

if [ ! -f "$ZIMAGE_QEMU" ] || [ "$REBUILD_KERNEL" = 1 ]; then
    [ -n "$KERNEL_DIR" ] && [ -f "$KERNEL_DIR/.config" ] || {
        echo "$0: no configured kernel tree under $REPO/build/kernel/src (run tools/kernel/setup-kernel-src.sh)" >&2
        exit 1
    }
    echo "==> building the QEMU kernel variant (CMDLINE_FORCE off, MACH_SPITZ on)"
    CONFIG_BACKUP="$KERNEL_DIR/.config.real-hardware-backup"
    cp "$KERNEL_DIR/.config" "$CONFIG_BACKUP"
    ( cd "$KERNEL_DIR"
      ./scripts/config --enable MACH_SPITZ \
                       --set-str CMDLINE "console=ttyS0 earlyprintk" \
                       --disable CMDLINE_FORCE
      make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" olddefconfig >/dev/null
      make ARCH=arm CROSS_COMPILE="$CROSS_COMPILE" -j"$(nproc)" zImage >/dev/null )
    cp "$KERNEL_DIR/arch/arm/boot/zImage" "$ZIMAGE_QEMU"
    restore_config
    CONFIG_BACKUP=
    echo "==> NOTE: $KERNEL_DIR/arch/arm/boot/zImage is the QEMU variant."
    echo "    Re-run 'make zImage' before flashing anything to real hardware."
fi

[ -n "$KERNEL" ] || KERNEL="$ZIMAGE_QEMU"

if [ "$BUILD_ONLY" = 1 ]; then
    echo "==> built: $QEMU_BIN"
    echo "==> built: $KERNEL"
    exit 0
fi

[ -f "$KERNEL" ] || { echo "$0: no kernel at $KERNEL" >&2; exit 1; }

if [ "$CONSOLE" = serial ]; then
    APPEND="console=ttyS0 earlyprintk"
else
    APPEND="console=tty0"
fi

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
    [ -f "$INITRD" ] || { echo "$0: no initramfs at $INITRD (tools/kernel/build-initramfs.sh)" >&2; exit 1; }
    BOOTMEDIA="initrd"
fi

echo "==> booting -M $MACHINE (console=$CONSOLE, root=$BOOTMEDIA)"

if [ "$BOOTMEDIA" = sd ]; then
    set -- -drive "if=sd,format=raw,file=$SD,snapshot=on"
else
    set -- -initrd "$INITRD"
fi
set -- "$@" -rtc base=2020-01-01T00:00:00
MEDIA_ARGS="$*"

if [ -n "$SCREENSHOT" ]; then
    QMP_SOCK="$(mktemp -u /tmp/piko-emulate-qmp.XXXXXX)"
    [ -n "$TIMEOUT" ] || TIMEOUT=$((DELAY + 20))
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
