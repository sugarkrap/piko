# Dead Letter — Sharp Zaurus SL-C860 Revival

*Everything you need to pick this up cold. Written 2026-07-21.*

---

## Hardware

**Sharp Zaurus SL-C860** (codename: Corgi)

| Component | Detail |
|---|---|
| CPU | Intel PXA255 (XScale ARMv5TE) @ 400 MHz, no FPU |
| RAM | 64 MB |
| Display | ATI Imageon W100 GPU → 640×480 LCD (physically portrait: 480×640 fb) |
| Storage | NAND flash (Cacko 1.23 ROM), CF slot, SD slot |
| Connectivity | USB gadget (client), 802.11b via PCMCIA (Prism2/2.5/3) |
| Kernel (stock) | Linux 2.4.18-rmk7-pxa3-embedix, OABI |
| Kernel (new) | Linux 7.1.4, in-tree + patches here, OABI+EABI compat |

The device runs **Cacko 1.23** (a Qtopia 1.x ROM). The display is physically portrait but the W100 GPU performs hardware rotation — the kernel driver `w100fb.c` transparently presents a landscape 640×480 framebuffer when landscape resolution is requested.

---

## Repository layout

```
zaurus-refresh/
├── corgi_patched.c / corgi_pm_patched.c   ← board files, compile-verified vs 7.1.4
├── corgi_v6.0.c / corgi_pm_v6.0.c        ← originals (v6.0, before mainline removal)
├── corgi.h                                 ← GPIO/hardware constants header
├── kernel.config-corgi-7.1.4              ← .config that builds the working zImage
├── zImage-corgi-7.1.4                     ← built kernel (5.99 MB), ready to flash
├── kernel-src/linux-7.1.4/               ← full patched kernel source (gitignored)
├── w100/
│   ├── w100fb_patched.c                   ← display driver, compile-verified
│   ├── w100fb.h (public)                  ← include/video/w100fb.h (from v6.0)
│   └── w100fb_private.h                   ← drivers/video/fbdev/w100fb.h (from v6.0)
├── hostap-work/                            ← PCMCIA WiFi driver (20 files)
├── flash/
│   ├── zImage                             ← kernel currently flashed on device
│   ├── kernel-flash.sh                    ← flashing script (via Cacko's updater)
│   └── modules/                           ← WiFi/crypto KOs (gitignored)
├── initramfs/
│   ├── rootfs/                            ← initramfs tree (busybox + zsh WIP)
│   └── initramfs-minimal.cpio.gz         ← last built cpio (gitignored)
├── userspace/bin/                          ← static ARM binaries (iwconfig etc.)
└── qemu-spitz/                            ← QEMU 9.1.0 build (gitignored)
```

---

## Project 1 — Mainline kernel for Corgi (this repo)

### What it is

Corgi support was **removed from mainline in Jan 2023** (Linux 6.1) as part of
a "remove unused ARM board files" cleanup. The board files weren't broken; they
just weren't being maintained. This project re-ports them to Linux 7.1.4.

### Current state (2026-07-21): **builds clean, unbooted on real hardware**

Everything compiles with zero warnings. The kernel image `zImage-corgi-7.1.4`
(5.99 MB) boots successfully in QEMU (`-M spitz`, PXA270) with the busybox
initramfs. QEMU's spitz machine is PXA270, not our PXA255, has no W100, and
never triggers `machine_is_corgi()` — so the QEMU test validates the shared
PXA2xx/sharpsl_pm boot path only. Real hardware validation is still pending.

**What was ported and compile-verified:**
- `arch/arm/mach-pxa/corgi.c` + `corgi_pm.c` — board init, power management
- `drivers/video/fbdev/w100fb.c` — ATI Imageon W100 display driver (3 fixes)
- `net/wireless/lib80211*` + `crypto/michael_mic.c` — removed in 6.8, re-added
- `drivers/net/wireless/prism54/hostap_cs` + friends — PCMCIA WiFi (from 6.7)

**Key API changes hit along the way (for reference):**
- `.handle_irq` removed from `struct machine_desc` — just drop it
- `FBINFO_DEFAULT` is gone (was always 0x0) — drop the flag
- `platform_driver.remove()` returns `void` now (was `int`) everywhere
- `matrix_keypad_platform_data` + `ads7846_platform_data` migrated to
  `software_node`/`PROPERTY_ENTRY_*` — ported to match `spitz.c`
- UDC platform_data removed — replaced with `gpiod_lookup_table`
- Timer renames: `del_timer_sync`→`timer_delete_sync`, `from_timer`→`timer_container_of`
- `lib80211_crypt_tkip.c` local `michael_mic()` collided with new ieee80211.h public — renamed

### Kernel config highlights

```
CONFIG_ARCH_PXA=y
CONFIG_MACH_CORGI=y
CONFIG_OABI_COMPAT=y        ← lets old OABI Cacko userland run alongside EABI binaries
CONFIG_FB_W100=y
CONFIG_KEYBOARD_MATRIX=y    ← required (was missing from pxa_defconfig — keyboard would be silent)
CONFIG_KEYBOARD_GPIO_POLLED=y
CONFIG_HOSTAP_CS=m
BLK_DEV_INITRD=y
INITRAMFS_SOURCE="path/to/rootfs"
```

See `kernel.config-corgi-7.1.4` for the full working config.

### Toolchain for kernel

`arm-unknown-linux-gnueabi` built with crosstool-ng (armv5te/xscale/soft-float,
**C++ disabled** — `CT_CC_LANG_CXX=n` to skip `libcody` which fails against
host GCC 16+). Installed to `~/x-tools/`.

### QEMU smoke test

```sh
qemu-spitz/build/qemu-system-arm -M spitz \
  -kernel zImage-corgi-7.1.4 \
  -initrd initramfs/initramfs-minimal.cpio.gz \
  -append "console=ttyS0 earlyprintk" -serial stdio -nographic -monitor none
```

QEMU 9.1.0 is the **last version with `spitz` machine** (removed in 9.2). Built
from source in `qemu-spitz/`.

### Flashing

Flash via Cacko's `updater.sh` mechanism over CF/SD. See `flash/kernel-flash.sh`.
Flash kernel-only (zImage, no initrd.bin initially) to preserve existing Cacko
rootfs on NAND. Non-destructive.

**VFS panic at boot:** The new kernel panics at `VFS: Unable to mount root fs on
'/dev/mtdblock1'` because Cacko's NAND layout uses MTD partition 1 as root but
the new kernel's MTD wiring for Corgi doesn't map it the same way. **Solution:
use initramfs as final root** (see Project 3 below).

---

## Project 2 — DOSBox for Zaurus (sibling repo: `dosbox-armv5-zaurus`)

### What it is

DOSBox 0.74-3 cross-compiled for the SL-C860, running from the Qtopia launcher
in fullscreen landscape. The binary is statically linked so it carries SDL 1.2,
ncurses, etc. with it.

### Current state (2026-07-21): **binary runs, Qtopia launch needs work**

- Binary (ELF32, OABI, statically linked): `buildroot/output/build/dosbox-0.74-3/src/dosbox`
- `dosbox --version` exits 0 on device ✓
- `dosbox` (no args) over SSH: "Can't init SDL Unable to open a console terminal" — expected
- From direct SSH + vtlaunch: reaches `GUI_StartUp` (framebuffer opens) ✓
- From Qtopia launcher (`dosbox-run` script): **current state unclear** — last
  confirmed working test was binary `217851e7` reaching `GUI_StartUp`. Display
  output not yet confirmed (whether the 640×480 landscape image appears correctly).

### Critical OABI constraints

The stock 2.4.18 kernel is **OABI-only**. EABI binaries segfault at first syscall
(`swi #0` not understood). Building for OABI requires:

| Setting | Value |
|---|---|
| uClibc `CONFIG_ARM_EABI` | **NOT SET** |
| GCC ABI | `apcs-gnu` (`-mabi=apcs-gnu`) |
| Syscall convention | `swi #N` (OABI), not `swi #0` + r7 |
| LinuxThreads | OLD (not NPTL) — futexes unavailable on 2.4 |

The buildroot is in `dosbox-armv5-zaurus/buildroot/`, defconfig
`zaurus-dosbox_defconfig`. Non-trivial patches were needed in GCC and uClibc to
get OABI clean — see the sibling repo's README for the full patch list.

### The display pipeline (stock Cacko kernel)

```
DOSBox renders → 640×480 SDL surface
  → SDL fbcon blits to /dev/fb0
  → /dev/fb0 is physically 480×640 portrait (w100fb)
  → w100fb hardware-rotates → 640×480 landscape on LCD
```

SDL `fbcon` needs to be told to **not** apply software rotation. The w100fb
driver handles it in hardware when landscape resolution is requested. DOSBox
should write directly into a 640×480 logical fb.

Current issue: there may be a 40px letterbox (480 physical height / 640 logical
width — need to verify whether SDL's fbcon applies any centering). Likely needs
direct `mmap` of the framebuffer bypassing SDL's rotation logic entirely.

### `dosbox-run` launch script

Located at `/home/QtPalmtop/bin/dosbox-run` on the Zaurus:
1. First invocation forks background, exits (Qtopia sees clean return)
2. Background invocation: kills QPE, runs `vtreset`/`fbpan0`, runs
   `vtlaunch` (redirects stdin/stdout/stderr to /dev/tty1) → exec dosbox

**vtlaunch** is critical: it moves fd 0/1/2 to /dev/tty1 before execing dosbox,
giving dosbox a real console terminal for SDL fbcon.

### SSH deployment

```sh
sshpass -p 'zer0care' scp \
  -o KexAlgorithms=+diffie-hellman-group1-sha1 \
  -o HostKeyAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no \
  -o PubkeyAuthentication=no \
  buildroot/output/build/dosbox-0.74-3/src/dosbox \
  zaurus@10.43.112.72:/home/zaurus/dosbox.new
# then on device:
sshpass -p 'zer0care' ssh ... zaurus@10.43.112.72 \
  "su root -c 'cp /home/zaurus/dosbox.new /home/QtPalmtop/bin/dosbox'"
```

**WARNING:** NEVER run `dosbox` from SSH without a backgrounded kill timer.
`SDL_VIDEODRIVER=dummy` makes it spin at 100% CPU and render the device
completely unresponsive to SSH. Always: `dosbox & PID=$!; sleep 5; kill $PID`.

### Open DOSBox items

- Verify fullscreen landscape display (640×480 image, no rotation artifacts)
- Remove ZDB debug logging (marker files, early init, SIGSEGV handler, Config::Init logging)
- Remove shadow_fb if present — direct fb mmap instead
- Add vsync gate via `FBIO_WAITFORVSYNC` ioctl (needs w100fb patch below)
- Implement 40px vertical letterbox if needed (480 vs 640 height mismatch)
- Shift+Backspace to quit

---

## Project 5 — sharpsl-nand real-hardware bring-up (2026-07-21)

### The bug

First real-hardware boot hit:
```
nand: device found, Manufacturer ID: 0xec, Chip ID: 0x79 (Samsung NAND 128MiB 3,3V 8-bit)
nand: 128 MiB, SLC, erase size: 16 KiB, page size: 512, OOB size: 16
sharpsl-nand sharpsl-nand: probe with driver sharpsl-nand failed with error -22
MTD: Couldn't look up '/dev/mtdblock1': -2
Kernel panic - not syncing: VFS: Unable to mount root fs on "/dev/mtdblock1" or unknown-block(0,0)
```

NAND chip ID detection succeeds (page/OOB/erase geometry all correct), so the
failure is past chip identification, inside `nand_scan()`'s hardware-ECC setup.
QEMU's `spitz` machine has no real Corgi NAND controller, so this driver had
**never been exercised on real hardware before** — this was the first real test.

### Root cause (found, fixed)

`drivers/mtd/nand/raw/sharpsl.c`'s `sharpsl_attach_chip()` only wires up
`ecc.hwctl`/`ecc.calculate`/`ecc.correct` when
`chip->ecc.engine_type == NAND_ECC_ENGINE_TYPE_ON_HOST` — but **nothing in the
file ever sets `engine_type` to `ON_HOST`** before `nand_scan()` runs. Compare
to `omap2.c`/`sunxi_nand.c`/`fsmc_nand.c`, which all set this explicitly in
`probe()`. Without it, `attach_chip` no-ops, the three ECC callbacks stay NULL,
and `nand_scan_tail`'s hardware-ECC sanity check
(`nand_base.c` ~line 5779: `if ((!ecc->calculate || !ecc->correct || !ecc->hwctl) && ...)`)
hits `WARN(1, "No ECC functions supplied..."); return -EINVAL;` — exactly the
observed -22. A genuine porting bug, not a hardware/wiring issue.

**Fix:** in `sharpsl_nand_probe()`, before the `nand_scan(this, 1)` call, add:
```c
this->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;
```
Applied in `kernel-src/linux-7.1.4/drivers/mtd/nand/raw/sharpsl.c` and mirrored
to the tracked copy at `nand/sharpsl_nand_patched.c` (kernel-src/ is
gitignored). `nand/sharpslpart.c` and `nand/sharpsl.h` also copied for
reference — untouched, just tracked now.

### MTD partition numbering (separate, resolved finding)

The `/dev/mtdblock1` VFS panic in the log above is **not a board/wiring bug**
either. `sharpslpart.c` documents real `/proc/mtd` output sampled from an
SL-C860:
```
mtd0: "Filesystem"   (boot ROM / physmap)
mtd1: "smf"          (kernel slot — what kernel-flash.sh writes to)
mtd2: "root"          (Cacko rootfs)
mtd3: "home"
```
`root=/dev/mtdblock1` was simply the wrong index — that's `smf` (kernel), not
`root`. Root is `mtd2`. Not needed for the initramfs-as-root plan (Project 3)
since that path never mounts an external root at all, but worth knowing if a
persistent NAND-backed rootfs is wanted later.

### Kernel image size budget — real blocker for NAND flashing

`flash/kernel-flash.sh`'s kernel-only NAND slot (`smf` partition, `mtd1`) is
capped at `MTD_PART_SIZE=1294336` bytes (~1.26 MB). Sizes as of 2026-07-21:

| Image | Size |
|---|---|
| Currently flashed (`flash/zImage`) | 1.12 MB |
| `zImage-corgi-7.1.4` (no initramfs) | 5.99 MB — **4.7x over budget** |
| + embedded zsh initramfs (~3 MB compressed) | ~9 MB — **~7x over budget** |

The bloat is from unrelated subsystems pulled into `kernel.config-corgi-7.1.4`
(ext4, NTFS, dma-buf, unrelated MFD chip drivers, devlink) — this board needs
none of it. **Not yet trimmed** — next real step before a NAND flash attempt
can succeed. `kernel-flash.sh` checks size before writing and exits cleanly if
oversized, so attempting anyway is non-destructive, just pointless until this
is fixed.

Considered and rejected: flashing the initramfs separately as `initrd.bin` via
`flash/updater.sh`. That mechanism writes into the **live Cacko rootfs
partition** (`FSRO`/`mtd2`), not a RAM-loaded companion image — nothing copies
it into RAM and hands it to the kernel via ATAG, so it wouldn't actually solve
anything and would destroy the current Cacko install in the process. Embedding
via `INITRAMFS_SOURCE` and trimming the kernel config is the right path.

---

## Project 3 — Initramfs-as-root with zsh (active)

### Why

The new 7.1.4 kernel panics trying to mount `/dev/mtdblock1` as root — the NAND
layout mismatch is non-trivial to fix, and we don't need the Cacko userland
anyway (we're replacing it). **Solution: embed a complete initramfs in the kernel
and use it as the permanent root.** No `root=` kernel parameter; `init=/init` stays
as PID 1 forever.

The goal is a self-contained environment that:
1. Boots to a **zsh shell with tab completion** (immediate goal)
2. Eventually launches DOSBox fullscreen (end goal)

### Current state (2026-07-21): **zsh integrated, ash fallback added, QEMU-validated; kernel rebuild in progress**

`initramfs/rootfs/init` now tries `/bin/zsh -l` first (with a quick
`zsh -c 'exit 0'` smoke test) and falls back to `/bin/ash -l` (busybox) if zsh
is missing or fails. `/etc/zshrc`, `/etc/passwd`, `/etc/group` added (minimal,
just enough for `compinit`). `compinit -u` is used deliberately — this is a
single-user embedded device, so the interactive insecure-directory prompt
(`compaudit`) would otherwise hang an unattended boot waiting for y/n on stdin.

Confirmed working in QEMU (`spitz` machine, PXA270, shared boot path only —
still not real-hardware-validated for the shell itself): kernel boots, `/init`
execs zsh, `redhat` prompt renders, tab-completion autoload is wired via
`FPATH`. Repacked into `initramfs/initramfs-minimal.cpio.gz`.

Kernel-side: `kernel-src/linux-7.1.4/.config` had drifted (`BLK_DEV_INITRD`
wasn't even set) from the tracked `kernel.config-corgi-7.1.4`. Re-synced,
`INITRAMFS_SOURCE` pointed at `initramfs/rootfs`, `oldconfig` re-run. Rebuild
with the sharpsl-nand fix (Project 5) is in progress — **still needs the
config trimmed down to fit the 1.26 MB NAND slot before this is flashable**
(see Project 5's size-budget note); QEMU testing doesn't need that trim.

### Old status notes (superseded by above, kept for the build recipe)

- `initramfs/rootfs/` — the initramfs tree; currently has busybox + one `/init` script
- `initramfs/rootfs/init` — mounts proc/sys/devtmpfs, drops to `/bin/sh` (busybox ash)
- **zsh 5.9** cross-compiled and available at:
  `dosbox-armv5-zaurus/buildroot/output/target/bin/zsh` (ARM EABI, statically linked, 1.3 MB stripped)
- **zsh functions** (1153 files, 7.2 MB → 1.4 MB compressed):
  `dosbox-armv5-zaurus/buildroot/output/target/usr/share/zsh/5.9/functions/`
- **terminfo** (linux, vt100, xterm, etc.):
  `dosbox-armv5-zaurus/buildroot/output/target/usr/share/terminfo/`

**zsh was built using the EABI toolchain** (same buildroot as DOSBox,
`arm-buildroot-linux-uclibcgnueabi`). This is correct — the new 7.1.4 kernel
has `CONFIG_OABI_COMPAT=y` so EABI binaries work fine.

### Next steps to complete initramfs

1. **Copy binaries into rootfs:**
   ```sh
   ROOTFS=~/Code/zaurus-refresh/initramfs/rootfs
   BR=~/Code/dosbox-armv5-zaurus/buildroot/output/target
   STRIP=~/Code/dosbox-armv5-zaurus/buildroot/output/host/bin/arm-buildroot-linux-uclibcgnueabi-strip

   cp $BR/bin/zsh $ROOTFS/bin/zsh && $STRIP $ROOTFS/bin/zsh
   mkdir -p $ROOTFS/usr/share/zsh/5.9
   cp -r $BR/usr/share/zsh/5.9/functions $ROOTFS/usr/share/zsh/5.9/
   mkdir -p $ROOTFS/usr/share/terminfo
   cp -r $BR/usr/share/terminfo/l $ROOTFS/usr/share/terminfo/  # linux
   cp -r $BR/usr/share/terminfo/x $ROOTFS/usr/share/terminfo/  # xterm
   cp -r $BR/usr/share/terminfo/v $ROOTFS/usr/share/terminfo/  # vt100
   ```

2. **Write `/etc/zshrc`:**
   ```sh
   mkdir -p $ROOTFS/etc
   cat > $ROOTFS/etc/zshrc << 'EOF'
   export TERM=linux
   export HOME=/root
   export PATH=/bin:/sbin:/usr/bin:/usr/sbin
   FPATH=/usr/share/zsh/5.9/functions:$FPATH
   autoload -Uz compinit && compinit
   autoload -Uz promptinit && promptinit
   prompt redhat
   HISTFILE=/root/.zsh_history
   HISTSIZE=500
   setopt hist_ignore_dups autocd
   EOF
   mkdir -p $ROOTFS/root
   ```

3. **Update `/init` to exec zsh:**
   ```sh
   #!/bin/sh
   mount -t proc proc /proc
   mount -t sysfs sysfs /sys
   mount -t devtmpfs devtmpfs /dev 2>/dev/null

   # Give the console a moment, set terminal
   export TERM=linux

   exec /bin/zsh -l
   ```

4. **Repack the cpio.gz:**
   ```sh
   cd $ROOTFS
   find . | cpio -H newc -o | gzip -9 > ../initramfs-minimal.cpio.gz
   ```

5. **Rebuild the kernel** (it embeds the initramfs at build time via `INITRAMFS_SOURCE`):
   ```sh
   cd ~/Code/zaurus-refresh/kernel-src/linux-7.1.4
   make ARCH=arm CROSS_COMPILE=arm-unknown-linux-gnueabi- zImage -j$(nproc)
   cp arch/arm/boot/zImage ../../zImage-corgi-7.1.4
   ```
   Or pass as `initrd` at boot time (simpler for iteration):
   ```sh
   qemu-system-arm -M spitz -kernel zImage -initrd initramfs-minimal.cpio.gz \
     -append "console=ttyS0 earlyprintk" -serial stdio -nographic
   ```

### zsh function loading is lazy (autoload)

All 1153 zsh function files sit in tmpfs (consuming ~7.2 MB RAM), but zsh only
reads a file into memory when the function is first called. `autoload -Uz compinit`
marks the function for lazy load — the completion system initializes only when the
first Tab is pressed. This is zsh's standard autoload mechanism; it's what
"lazy loading" means in this context. No special setup needed beyond `FPATH` and
`autoload`.

---

## Project 4 — w100fb enhancements (pending)

### Add FBIO_WAITFORVSYNC ioctl

`w100_vsync()` exists in `w100fb_patched.c` (lines 1590-1627, polls
`GEN_INT_STATUS` bit 1) but is not exposed to userspace — `w100fb_ops` has no
`.fb_ioctl`. To add:

```c
static int w100fb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
    if (cmd == FBIO_WAITFORVSYNC) {
        w100_vsync();
        return 0;
    }
    return -EINVAL;
}

// In w100fb_ops:
.fb_ioctl = w100fb_ioctl,
```

DOSBox can then gate each frame on vsync via:
```c
ioctl(fb_fd, FBIO_WAITFORVSYNC, 0);
```

---

## Quake (incidental)

Handheld Quake (`qpe-quake_1.5.0-2`, OABI dynamically linked) runs on Cacko.
Works landscape from Games tab with `-width 640 -height 480` in `quake_script`.
No custom build needed; the stock OABI binary + correct flags is sufficient.
Data at `/mnt/card/id1/pak0.pak` (172 MB).

---

## WiFi / PCMCIA

`hostap_cs` driver is ported and builds as a module. Userspace tools (static
ARM EABI binaries) are in `userspace/bin/`: `iwconfig`, `iwlist`, `iwpriv`,
`pccardctl`, `wpa_supplicant`, `wpa_cli`. These are the **only** tools that
work with this driver — it speaks Wireless Extensions (WEXT) only, not
`cfg80211`/`nl80211`. Modern `iw` / `nmcli` cannot control this card.

---

## Zaurus SSH access

```
IP:       10.43.112.72
User:     zaurus
Password: zer0care
Flags:    -o KexAlgorithms=+diffie-hellman-group1-sha1
          -o HostKeyAlgorithms=+ssh-rsa
          -o StrictHostKeyChecking=no
          -o PubkeyAuthentication=no
Root:     su root -c "..."  (no password from zaurus user)
```

Use `sshpass -p 'zer0care'` to script it. ED25519 keys are not supported by
the device's old OpenSSH — password auth only.

---

## What's NOT done yet (honest accounting)

- [ ] Boot new 7.1.4 kernel on real Zaurus hardware (zero hardware tests so far)
- [ ] Verify w100fb actually rotates and displays correctly on real hardware
- [ ] Initramfs: copy zsh + functions + repack + flash
- [ ] DOSBox: confirm full landscape display works end-to-end
- [ ] DOSBox: add vsync ioctl to w100fb
- [ ] DOSBox: strip all debug logging (ZDB markers, early init, Config::Init spam)
- [ ] DOSBox: direct fb mmap instead of SDL's fbcon (remove shadow_fb, remove rotation code)
- [ ] WiFi: any runtime test of hostap_cs module
- [ ] UDC: runtime test of USB gadget
