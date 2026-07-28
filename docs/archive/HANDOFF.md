# Handoff — build on a faster machine, SD card required

*Written 2026-07-21, for an agent picking this up on different hardware.*

## Read this first: real-hardware findings from tonight (2026-07-21)

Distilled from parallel real-hardware testing happening alongside this
build/QEMU work — **someone/something else was actively editing
`kernel-src/` at the same time** (see the GPIO13 "EARLY BOOT MARKER"/"ZAURUS
MARKER" debug blocks in `corgi.c` and `pxa25x.c` — those are real-hardware
LED-blink debug instrumentation from that parallel effort, not leftover
cruft; don't strip them without checking whether they're still in use):

- **Machine type is Husky (543), not Corgi (423).** Despite the SL-C860
  codename "Corgi," Sharp/Cacko's own tooling reports it identically to the
  SL-C760 — Husky. **Confirmed root cause, already fixed in this repo:**
  `kernel.config-corgi-7.1.4` had `CONFIG_MACH_HUSKY` disabled even though
  `corgi.c` already has a correct `MACHINE_START(HUSKY, "SHARP Husky")`
  block (`.nr` resolves to 543 via `arch/arm/tools/mach-types`, same
  `pxa25x_map_io`/`fixup_corgi` as the Corgi entry). Without
  `CONFIG_MACH_HUSKY=y`, that block is `#ifdef`'d out entirely, so the
  kernel would never match this device's real machine ID and would hang at
  `setup_arch()`'s machine-lookup exactly like the QEMU regression described
  below (see `dump_machine_table()`). Flipped to `CONFIG_MACH_HUSKY=y` in
  both `kernel.config-corgi-7.1.4` and `kernel-src/linux-7.1.4/.config`
  tonight — **not yet rebuilt/reverified**, do that first.
- **Real NAND is a 128MiB Samsung part** (512B page, 16B OOB, 16KiB erase —
  matches the real dmesg capture in DEADLETTER.md Project 5), not the 16MiB
  chip QEMU/dev testing might assume. Check any partition table,
  `mtdparts=`/cmdline offsets, or flash-layout assumption against this real
  geometry, not a smaller dev/emulated one.
- **Cold boot vs. soft reboot behave differently on this hardware.** A
  kernel that booted fully to console on a soft reboot (from the recovery
  flasher) stayed completely dark on a true cold boot with the *identical*
  binary. If a build "works" under one test path and not the other, don't
  assume the ROM/build is broken — check which boot path is actually being
  exercised first.
- **LED blink patterns during flashing are the flasher's own default reboot
  animation, not a kernel signal.** Don't use them to infer kernel boot
  progress — this cost real time tonight. This is presumably why the GPIO13
  debug markers exist in `corgi.c`/`pxa25x.c` (driving the LED directly from
  kernel code as an unambiguous signal, since the flasher's own LED animation
  is not trustworthy for this).

Read `README.md` and `DEADLETTER.md` first — this file only covers what's
needed to reproduce the build environment elsewhere and the state of an
in-progress investigation. Don't duplicate effort re-deriving what those two
already explain.

## Why hand off

Full kernel rebuilds on this machine take ~10-15 min each from a clean tree
(longer from scratch — crosstool-ng toolchain build alone is a multi-stage
bootstrap). We're mid-bisection on a QEMU boot regression that needs several
more clean-rebuild-and-test cycles; a faster machine (more cores, mainly —
this is a single ARMv5 kernel build, not something that benefits from
distributed builds) will make that tractable instead of a 10-minute round
trip per hypothesis.

## What this machine has that a fresh one won't

`kernel-src/linux-7.1.4/` (the full patched kernel tree) and `qemu-spitz/`
(built QEMU 9.1.0) are **both gitignored** — intentionally, they're large
build trees, not source. Only the small hand-maintained patch files are
tracked in git. On a fresh machine you must reconstruct `kernel-src/` before
anything else. There is no automated apply-script yet — do this by hand,
carefully, file by file:

### 1. Get a pristine Linux 7.1.4 tree

```sh
mkdir -p kernel-src && cd kernel-src
curl -LO https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.1.4.tar.xz
tar xf linux-7.1.4.tar.xz
```

### 2. Apply the Corgi board files

```sh
cp corgi_patched.c    kernel-src/linux-7.1.4/arch/arm/mach-pxa/corgi.c
cp corgi_pm_patched.c kernel-src/linux-7.1.4/arch/arm/mach-pxa/corgi_pm.c
cp corgi.h            kernel-src/linux-7.1.4/arch/arm/mach-pxa/corgi.h
```
`Kconfig`/`Makefile` wiring for `MACH_CORGI`/`SHEPHERD`/`HUSKY`/
`PXA_SHARP_C7xx` in `arch/arm/mach-pxa/{Kconfig,Makefile}` needs to exist too
— it's not a standalone file to copy, it's small edits alongside the
existing `spitz`/`akita` entries. See README.md "Files here" section; if in
doubt, `grep -n CORGI` on this machine's `kernel-src/arch/arm/mach-pxa/{Kconfig,Makefile}`
and replicate.

### 3. Apply the W100 display driver

```sh
cp w100/w100fb_patched.c  kernel-src/linux-7.1.4/drivers/video/fbdev/w100fb.c
cp w100/w100fb_private.h  kernel-src/linux-7.1.4/drivers/video/fbdev/w100fb.h
cp w100/w100fb.h          kernel-src/linux-7.1.4/include/video/w100fb.h
```
Plus `Kconfig`/`Makefile` wiring for `FB_W100` — reference copies at
`w100/Kconfig_fbdev` / `w100/Makefile_fbdev`, same "replicate the diff"
caveat as above.

### 4. Apply the sharpsl-nand driver (includes today's -22 fix)

```sh
cp nand/sharpsl_nand_patched.c kernel-src/linux-7.1.4/drivers/mtd/nand/raw/sharpsl.c
cp nand/sharpslpart.c          kernel-src/linux-7.1.4/drivers/mtd/parsers/sharpslpart.c
cp nand/sharpsl.h               kernel-src/linux-7.1.4/include/linux/mtd/sharpsl.h
```
`sharpslpart.c` and `sharpsl.h` are untouched upstream files, just tracked
here for convenience — no diff to replicate, they're drop-in. Confirm
`drivers/mtd/nand/raw/Kconfig` / `Makefile` and `drivers/mtd/parsers/Kconfig`
/ `Makefile` already reference `sharpsl`/`sharpslpart` in a pristine
7.1.4 tree (they should — these files were never removed from mainline,
unlike the board/display/wifi files above).

### 5. Apply hostap_cs (PCMCIA WiFi) + its lib80211/michael_mic dependencies

All 20 files in `hostap-work/` go to
`kernel-src/linux-7.1.4/drivers/net/wireless/intersil/hostap/`, **except**
`lib80211*` (→ `net/wireless/`) and `michael_mic.c` (→ `crypto/`). Kconfig/
Makefile wiring needed in all three locations — see README.md's detailed
"What it is" section for the exact API changes involved (timer renames,
`header_ops.parse()` signature, `michael_mic` symbol collision, etc.) since
those aren't mechanical copies, they're actual patches against v6.7-era
source. Read that section before touching this — it's the most involved
piece of the whole port.

### 6. Config

```sh
cp kernel.config-corgi-7.1.4 kernel-src/linux-7.1.4/.config
```
Set `CONFIG_INITRAMFS_SOURCE="<absolute path to>/initramfs/rootfs"` if you
want the zsh/ash initramfs embedded (Project 3) — leave it `""` for a bare
kernel test. Then `make ARCH=arm CROSS_COMPILE=... oldconfig` (accept
defaults, non-interactively: `yes "" | make ... oldconfig`) — expect it to
report zero or near-zero new symbols; if it reports a lot, something in this
reconstruction diverged from what's tracked and needs investigating before
proceeding, not blindly accepting.

## Toolchain

Not tracked in git at all (lives at `~/x-tools/` on this machine, outside
the repo). Build fresh via crosstool-ng — see README.md "Toolchain notes"
for the two gotchas (`CT_CC_LANG_CXX=n` to dodge `libcody` against new host
GCC, `CT_DEBUG_CT_SAVE_STEPS=y` for resumability). Target:
`arm-unknown-linux-gnueabi`, armv5te/xscale/soft-float.

## QEMU

Build 9.1.0 from source (`qemu-spitz/`, gitignored) — README.md "QEMU notes"
covers the two build issues (mkvenv/distlib, libnfs API drift) and their
workarounds. This is the **last version with the `spitz` machine**
(removed 9.2) — don't accidentally grab a newer release.

## Build command

```sh
export PATH=~/x-tools/arm-unknown-linux-gnueabi/bin:$PATH
cd kernel-src/linux-7.1.4
make ARCH=arm CROSS_COMPILE=arm-unknown-linux-gnueabi- zImage -j$(nproc)
cp arch/arm/boot/zImage ../../zImage-corgi-7.1.4
```

## QEMU smoke test

```sh
qemu-spitz/build/qemu-system-arm -M spitz \
  -kernel zImage-corgi-7.1.4 \
  -append "console=ttyS0 earlyprintk" -serial stdio -nographic -monitor none
```
A working boot prints `Machine: SHARP Spitz` within the first ~10 lines and
proceeds through normal kernel init. **If it's totally silent (nothing at
all, not even the QEMU deprecation warning's followup), that's the
regression below — don't assume it's your reconstruction being wrong first,
though double-check `.config` really matches
`kernel.config-corgi-7.1.4` if it happens on a truly fresh tree.**

---

## Open investigation: QEMU silent-boot regression (inconclusive at handoff)

Hit this while rebuilding to pick up the sharpsl-nand fix (Project 5 in
DEADLETTER.md) and the zsh initramfs (Project 3). Spent a long time
bisecting it; **status at handoff is inconclusive, and given the parallel
real-hardware findings above (someone else was actively editing
`kernel-src/` concurrently), it may simply have been chasing a moving
target rather than a real, stable regression.** Full timeline, so the next
agent doesn't have to redo this work blind, but don't over-invest in it
before first just trying a clean rebuild with today's actual fixes
(`CONFIG_MACH_HUSKY=y` + the sharpsl-nand ECC fix) — it may just work now.

1. Rebuilt from `kernel.config-corgi-7.1.4` + `oldconfig` + the sharpsl-nand
   fix + embedded initramfs. Result: 9.18 MB zImage, **boots completely
   silent in QEMU** — no kernel banner, nothing. `-d in_asm` traced
   execution to `dump_machine_table()` (`arch/arm/kernel/setup.c:1119`)
   spinning forever — the path taken when the boot machine ID doesn't match
   any compiled-in `machine_desc`.
2. Checked the obvious cause for **QEMU's own `-M spitz` board** (unrelated
   to the real device's Husky finding above): QEMU passes board ID `0x2c9`
   (713, from `hw/arm/spitz.c`'s `spitzpda_class_init`), and the compiled
   `__mach_desc_SPITZ.nr` is also `713` (confirmed via `xxd` on the raw
   `.init.arch.info` section of `vmlinux`, re-checked after every rebuild
   below) — **always matched**, so it's not a QEMU machine-ID mismatch.
3. Ruled out the initramfs (rebuilt with `INITRAMFS_SOURCE=""`, ~6 MB,
   matching the known-good binary's size — still silent), ruled out the
   sharpsl-nand fix (reverted the one added line, rebuilt — still silent),
   ruled out stale incremental build state (`make clean` + full rebuild —
   still silent), ruled out `.config` drift (extracted the known-good
   binary's embedded config via `scripts/extract-ikconfig` and diffed
   against the tracked file — byte-for-byte identical), ruled out QEMU
   itself (booted the known-good binary and a fresh bad build back-to-back
   with the identical invocation — good one works every time, rebuilds
   never do).
4. Went looking for what actually changed in `kernel-src/` since the
   known-good binary was built (`find . -newermt "2026-07-19 23:06:00" ...`)
   and found active drift in files with no tracked reference copy at all:
   `pxa25x.c`, `spi-pxa2xx.c`, `spi-pxa2xx-platform.c`, `ssp.c`,
   `corgi_lcd.c`, plus `corgi.c` (which does have a tracked copy,
   `corgi_patched.c`, and had drifted from it — GPIO13 debug markers, an
   extra SPI properties struct, a missing `sharpsl_rom_device` line).
   Reverted `spitz.c` to the pristine reference snapshot (`drivers/spitz.c`)
   as the most QEMU-relevant candidate — **still silent**. Downloaded a
   pristine `linux-7.1.4` tarball and diffed the other drifted files
   against it to inspect them (`pxa25x.c`'s and `spi-pxa2xx-platform.c`'s
   changes are both in code paths — `.map_io`, platform-data setup — that
   run *after* `setup_arch()`'s machine-ID check, i.e. after the point
   where boot is already hanging, so they're very unlikely culprits by
   function-scope reasoning alone; wasn't able to test a fully-reverted
   combination before this session ended).
5. **Conclusion at handoff: not resolved.** Every change *I* made was ruled
   out individually. What's left unexplained is why a byte-identical
   `.config` + a source tree that only differs in files whose changed code
   provably can't execute before the failure point still produces a
   different boot outcome than the known-good binary. Given concurrent
   edits to the same tree were happening in parallel, the most likely
   explanation is that the tree was in a transiently-broken state at some
   point during rebuilds (from the *other* debugging effort, mid-edit) and
   this wasn't actually one stable bug to find.

**Next step: don't restart this bisection from scratch.** Just do a clean
rebuild now that `CONFIG_MACH_HUSKY=y` is set and the sharpsl-nand fix is
in, and retest in QEMU (`-M spitz`, which exercises the shared PXA2xx path
mach-ID-matching against `713`/SPITZ, independent of the real device's
`543`/Husky finding — both should now be correctly registered). If it boots
clean, this section can be deleted. If it's *still* silent, resume from
point 4 above — get a fully clean revert of `pxa25x.c`/`spi-pxa2xx.c`/
`spi-pxa2xx-platform.c`/`ssp.c`/`corgi_lcd.c` to pristine (tarball already
know-how is above) as the next concrete experiment, one file at a time
rather than all at once, so a culprit (if there is one at all) is isolated
rather than just re-confirmed absent.

**Do not attempt a real hardware flash until either this is resolved and a
QEMU boot is reconfirmed working, or the real-hardware team (doing the
parallel work referenced above) confirms their own boot independently.**
Nothing unsafe has happened from the QEMU side — `kernel-flash.sh` checks
size before writing NAND and exits cleanly if oversized, so no wasted
attempt is destructive — but there's no point flashing a kernel that won't
even pass its own machine-ID check.

## Also still open, independent of the above

- **Kernel image size budget.** The kernel-only NAND slot
  (`flash/kernel-flash.sh`'s `MTD_PART_SIZE`) is ~1.26 MB. The current
  tracked config produces ~6 MB bare / ~9 MB with the initramfs embedded —
  both need trimming (ext4, NTFS, dma-buf, and several unrelated MFD chip
  drivers/devlink got pulled in for no reason this board needs) before any
  real NAND flash can succeed. See DEADLETTER.md Project 5 for the full
  table. Not started.
- **zsh/ash initramfs** itself (Project 3 in DEADLETTER.md) is done and
  QEMU-validated — this doesn't need more work, just needs a kernel that
  actually boots to embed it into.

---

## SD card mounting

The Zaurus is flashed via Cacko's `updater.sh` mechanism reading files off a
CF/SD card (`flash/kernel-flash.sh` takes `DATAPATH` — the mounted card's
path — as its argument). Whatever machine ends up actually preparing a card
for flashing needs to mount it. Notes from doing this today:

1. **Identify the card:**
   ```sh
   lsblk
   ```
   Look for a small (sub-2GB, this project uses roughly a 1GB card)
   removable block device, e.g. `mmcblk0p1` or `sdb1` — cross-check against
   `blkid` for `TYPE="vfat"` (Cacko expects a FAT-formatted card).

2. **Mount it.** If `udisks2` is running (check `udisksctl status`), no
   root needed:
   ```sh
   udisksctl mount -b /dev/mmcblk0p1
   ```
   This lands it at `/run/media/$USER/<LABEL-or-UUID>`. If `udisksctl`
   isn't available, fall back to a plain root mount (`sudo mount
   /dev/mmcblk0p1 /mnt`), which does need a password/root.

3. **Copy the built `zImage`** (once the boot regression above is resolved
   and the config is trimmed to fit the NAND budget) onto the card root,
   named exactly `zImage` (see `flash/kernel-flash.sh`'s `TARGETFILE`
   check) — **do not** also put an `initrd.bin` on the card unless you've
   deliberately decided to go that route; see DEADLETTER.md Project 5's
   "Considered and rejected" note on why that's destructive to the live
   Cacko rootfs and doesn't actually solve anything for this project's goal.

4. **Unmount cleanly before physically removing the card:**
   ```sh
   udisksctl unmount -b /dev/mmcblk0p1
   ```

Card was mounted at `/run/media/makaron/24BE-EA96` (vfat, UUID `24BE-EA96`)
during today's session on this machine — that exact path won't carry over
to a different machine/card, re-derive it with the steps above.

## Zaurus SSH access (for later runtime testing, once something boots)

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
ED25519 not supported by the device's old OpenSSH — password auth only.
This is the device's **current, live Cacko install** — be careful with
anything that writes to it; this project's whole flashing strategy is built
around not touching the existing rootfs (see kernel-only flash notes
above and in DEADLETTER.md).
