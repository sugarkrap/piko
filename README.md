# Corgi (SL-C860) mainline revival — prep notes

> **I AM NOT RESPONSIBLE FOR ANY BRICKED DEVICE. USE IT AT YOUR OWN RISK.
> PLEASE BEWARE THAT THIS IS INCOMPLETE.** See `DEADLETTER-MTD2-MTD3.md`
> for a real account of a device this project actually bricked, and why.

Scoped against current mainline (`torvalds/linux` master, checked 2026-07-19)
by diffing the last kernel to carry Corgi support (v6.0, before the Jan 2023
"remove unused board files" cleanup) against the actively-maintained sibling
board file `spitz.c`.

## Status (2026-07-19)

**`corgi.c`/`corgi_pm.c` now build clean (zero warnings) against Linux
7.1.4**, and a full kernel build produces a working `zImage`:
`zImage-corgi-7.1.4` (5.98 MB), config in `kernel.config-corgi-7.1.4`
(based on `pxa_defconfig` + `MACH_CORGI` + `CONFIG_OABI_COMPAT=y` so the
existing Cacko OABI userland keeps working under this EABI-built kernel).
Toolchain: `arm-unknown-linux-gnueabi`, built locally via crosstool-ng
(armv5te/xscale/soft-float, C++ disabled — see "Toolchain notes" below).

**QEMU smoke test passed.** Built QEMU 9.1.0 locally (last version with the
`spitz` machine — see "QEMU notes" below) and booted `zImage-corgi-7.1.4`
under `-M spitz` with a minimal busybox initramfs (built to a local scratch
path, e.g. `/tmp/initramfs-minimal.cpio.gz`, 1.1 MB — not tracked in git;
see "QEMU notes" below for how it's assembled): full boot to an
interactive shell, `spitz_misc_charging: Charging on.` confirms
`sharpsl_pm`'s runtime path (the same family `corgi_pm.c` uses) actually
executes correctly, not just compiles. Reuse:

```sh
qemu-spitz/build/qemu-system-arm -M spitz \
  -kernel zImage-corgi-7.1.4 \
  -initrd /tmp/initramfs-minimal.cpio.gz \
  -append "console=ttyS0 earlyprintk" -serial stdio -nographic -monitor none
```

Caveat: this only exercises the *shared* PXA2xx/sharpsl_pm boot path.
`-M spitz` is PXA270, not our PXA255 Corgi target, has no W100 chip, and
never sets `machine_is_corgi()` true — `corgi.c`/`corgi_pm.c` themselves
never execute here. Real validation of those still needs actual hardware.

**`w100fb.c` is now wired in and builds clean (zero warnings).** Turned out
to need far less than the original "decade of API drift, multi-week" scoping
worried about — three mechanical fixes total:

1. **A second, separate `w100fb.h` was needed** — the driver's own private
   register-definitions header (`drivers/video/fbdev/w100fb.h`), distinct
   from the public `include/video/w100fb.h` pulled earlier. Same filename,
   different file, both from v6.0.
2. **`FBINFO_DEFAULT` is gone from `fb.h`** — it was always just `0x0`, a
   no-op flag. Dropped from the `info->flags` assignment.
3. **`platform_driver.remove()` returns `void` now** (was `int`) — the
   exact same signature change already hit in `sharpsl_pm.c` and
   `pxa25x_udc.c`; this is clearly a blanket kernel-wide API change, not a
   subsystem-specific one. Fixed `w100fb_remove()` the same way.

`Kconfig`/`Makefile` wiring for `FB_W100` (re-adding what was stripped
alongside the board files) is done; `select FB_CFB_FILLRECT/COPYAREA/
IMAGEBLIT` still resolve fine — they're defined in `core/Kconfig`, not the
top-level file, nothing actually changed there despite first appearances.

Full kernel build with `CONFIG_FB_W100=y` succeeds, zero warnings,
`zImage-corgi-7.1.4` updated (5.99 MB — the ~6 KB delta is the driver).
**Still genuinely untested**: no real Corgi hardware, and QEMU's `spitz`
machine has no W100 chip to even attempt exercising this against.

**`hostap_cs` (PCMCIA WiFi, Prism2/2.5/3) ported and building clean.**
Removed from mainline in **Linux 6.8** (March 2024, last present at v6.7)
as "obsolete... marked obsolete in 2016... highly unlikely to still have
any users." Same pattern as `corgi.c`/`w100fb.c`: old, unmaintained, pruned
as dead weight rather than actually broken. This pull was bigger than the
board/display work — `HOSTAP` drags in a small dependency chain that had
*also* been removed or restructured since v6.7, all pulled from that same
snapshot for mutual consistency:

- **`lib80211`** (the WEP/TKIP/CCMP crypto helper library, 4 files) — not
  simply deleted; merged into `libipw` in Oct 2024, after our v6.7
  snapshot predates it. Re-added as a standalone subsystem (`Kconfig`/
  `Makefile` wiring in `net/wireless/`).
- **`CRYPTO_MICHAEL_MIC`** (`crypto/michael_mic.c`) — gone from
  `crypto/Kconfig`/`Makefile`, re-added the same way.
- **`WEXT_SPY`** — the Kconfig toggle is gone, but *not* because the
  feature was deleted: `wext-core.c` already has the `SIOCGIWSPY`/
  `SIOCSIWSPY` ioctls unconditionally built in now. Just dropped `select
  WEXT_SPY` from `hostap`'s own Kconfig — nothing to resurrect.
- **The generic iwspy plumbing `hostap` actually used *is* gone**, though:
  `struct iw_public_data`, `net_device.wireless_data`, and
  `wireless_spy_update()` were removed entirely (the surviving
  `iw_public_data` at v6.7 also references `struct libipw_device`,
  ipw2x00-specific — not something to drag in for hostap, which only ever
  touched the `spy_data` field). Rather than patching a core, widely-used
  `netdevice.h` struct back in, cleanly stripped the per-station
  signal-quality monitoring feature (`iwspy`) from `hostap_80211_rx.c`,
  `hostap_main.c`, and the `SIOCxIWSPY`/`THRSPY` ioctl table entries in
  `hostap_ioctl.c` — optional feature, not needed for basic association.
- **Timer API renames** (`del_timer_sync`→`timer_delete_sync`,
  `del_timer`→`timer_delete`, `from_timer`→`timer_container_of`) across
  `lib80211.c`, `hostap_hw.c`, `hostap_ap.c`, `hostap_ioctl.c` — same
  blanket kernel-wide rename already seen in `sharpsl_pm.c`.
- **`michael_mic` symbol collision** — `lib80211_crypt_tkip.c`'s own
  local helper function shared a name with a new public `michael_mic()`
  declared in `include/linux/ieee80211.h` (added post-v6.7, for mac80211's
  internal use). Renamed the local one to `lib80211_tkip_michael_mic`.
- **`asm/unaligned.h` → `linux/unaligned.h`** in `crypto/michael_mic.c`.
- **`header_ops.parse()` gained a `const struct net_device *dev`
  parameter** — updated `hostap_80211_header_parse()`'s signature in
  `hostap_main.c`.

All files live in `modules/hostap/` at the project root (20 driver files +
`lib80211*` + `michael_mic.c`). Full kernel build with `CONFIG_HOSTAP_CS=m`
succeeds, zero warnings. Completely untested at runtime — no CF/PCMCIA
WiFi card exercised this on real hardware or emulation.

**Userspace WiFi/PCMCIA tools cross-compiled and confirmed running**
(`userspace/bin/`, all static ARM binaries, sanity-checked via `qemu-arm`
user-mode emulation — actual runs, not just `file` checks):

- `iwconfig`/`iwlist`/`iwpriv`/`iwspy`/`iwgetid`/`iwevent` (wireless-tools
  29) — the *only* userspace API this driver can ever speak. `hostap` only
  ever supported Wireless Extensions (WEXT), never `cfg80211`/`nl80211` —
  modern tools like `iw` or `nmcli` categorically cannot control this
  card. `BUILD_STATIC=y` only affects whether `libiw` itself is `.a` or
  `.so`, not libc linking — needed `LDFLAGS=-static` separately to get
  fully static binaries.
- `pccardctl` (pcmciautils 018) — this is the modern name for what used
  to be called `cardctl` in the old pcmcia-cs package; added a
  `cardctl -> pccardctl` symlink for muscle memory. Needed `libsysfs`
  (sysfsutils 2.1.0, separately cross-compiled — small, self-contained,
  configured `--enable-static --disable-shared`) which isn't packaged
  anywhere modern; used a local `sysfs/` header shim
  (`userspace/sysfs-shim/`) since `pccardctl.c` expects
  `<sysfs/libsysfs.h>` but the built header lands at top level.
- `wpa_supplicant` + `wpa_cli` (2.11) — needed for WPA/WPA2-PSK networks;
  `iwconfig` alone only gets you open or WEP. Built with
  `CONFIG_DRIVER_WEXT=y` (already the defconfig default) and
  `CONFIG_TLS=internal` to avoid cross-compiling OpenSSL. Getting this to
  link took trimming a long chain of modern features this 2003-era card
  will never use, each one surfacing as the *next* missing dependency
  once the previous was cut — AP mode, WPS, P2P, EAP-TLS/PEAP/TTLS/FAST/
  IKEv2 (all need real TLS), MACsec, D-Bus control interface, SAE (WPA3)
  and EAP-PWD (both pull in `dragonfly.c`), and DPP/Wi-Fi Easy Connect
  (needs EC crypto). What's left: WEP, WPA-PSK, WPA2-PSK, and the basic
  EAP methods that don't need TLS (MD5/MSCHAPv2/PEAP-GTC/LEAP/etc.) —
  everything this hardware could plausibly ever connect to.
- `wpa_passphrase` deliberately **not** built — pulls in a `dbus-1`
  dependency for no clear reason and isn't needed; `wpa_supplicant.conf`
  can take a plaintext ASCII passphrase directly instead of a
  pre-hashed PSK.

**Keyboard/keypad support was silently missing — caught before it shipped.**
`pxa_defconfig` does *not* enable `CONFIG_KEYBOARD_MATRIX` (the actual
QWERTY driver `corgi_keypad_init()`'s `"matrix-keypad"` platform device
needs) or `CONFIG_KEYBOARD_GPIO_POLLED` (needed for the lid/tablet-mode/
headphone `gpio-keys-polled` device). Both were building fine — the
platform devices were being created with no driver bound to them, so the
keyboard simply wouldn't have worked, with no compile-time signal at all.
Both enabled now (`=y`, built-in). This class of gap — features that
compile clean but are functionally absent — won't show up in any of the
build-log-diffing work above; worth deliberately auditing `.config` against
what the device actually needs, not just trusting `pxa_defconfig`'s
defaults.

## What actually broke, beyond the original read-through

The initial read-through-only scoping below (SPI, UDC, keypad, ads7846,
IrDA, `.handle_irq`) was right in kind but incomplete — actually compiling
surfaced more API changes that a source read missed:

- **`matrix_keypad_platform_data` and `ads7846_platform_data` are both
  gone**, same swnode/`property_entry` migration as SPI/UDC. Ported
  `corgi_keypad_init()` and the ads7846 `spi_board_info` entry to
  `software_node`/`PROPERTY_ENTRY_*`, mirroring `spitz_mkp_init()` and
  `spitz_ads7846_props` exactly (including dropping the old
  `wait_for_sync()` board callback — the driver now polls the hsync GPIO
  itself once given `ti,hsync-gpios`).
- **`linux/input-event-codes.h` needs an explicit include** — `KEY_*`
  macros no longer come in transitively through `matrix_keypad.h`.
  `spitz.c` already includes it directly; `corgi.c` didn't.
- **`linux/printk.h` needs an explicit include** for `pr_err()` in the
  new error-handling paths added above.

## Files here

- `corgi_v6.0.c` — original board file, last mainline version (v6.0).
- `corgi_patched.c` / `modules/corgi_pm_patched.c` — **compile-verified clean
  (zero warnings) against Linux 7.1.4**, kept in sync with the working
  copies under `kernel-src/linux-7.1.4/arch/arm/mach-pxa/`.
- `corgi.h` — the board's GPIO/hardware-constant header, deleted alongside
  `corgi.c` in the same cleanup. Pulled unchanged from v6.0 — it's pure
  `#define`s, nothing in it depends on removed kernel APIs.
- `zImage-corgi-7.1.4` / `kernel.config-corgi-7.1.4` — the actual build
  output: a ready-to-flash (untested on hardware) compressed kernel image
  and the `.config` that produced it.
- `kernel-src/linux-7.1.4/` — full kernel source tree with everything
  above applied, plus `Kconfig`/`Makefile` wiring for `MACH_CORGI`.
- `modules/w100/w100fb.c`, `modules/w100/w100fb.h` (public, `include/video/`)
  — last pre-removal version (v6.0) of the driver for Corgi's actual display
  chip, the ATI Imageon W100. Removed Oct 2022 as collateral of the
  board-file cleanup ("all of which are now removed, so remove this
  driver as well" — not removed for being broken).
- `modules/w100/w100fb_private.h` — the driver's own private
  register-definitions header (`drivers/video/fbdev/w100fb.h` — same
  filename as the public one, different file). Also needed, not obvious
  until the compile actually asked for it.
- `modules/w100/w100fb_patched.c` — **compile-verified clean (zero warnings)**
  against Linux 7.1.4, kept in sync with the working copy under
  `kernel-src/linux-7.1.4/drivers/video/fbdev/w100fb.c`.
- `modules/w100/Kconfig_fbdev`, `modules/w100/Makefile_fbdev` — the
  `FB_W100` Kconfig entry and Makefile wiring, for reference when
  re-adding it to a current tree.
- `sharpsl_pm_v6.0.c` / `sharpsl_pm_current.c` — diffed for reference; this
  shared power-management driver needed almost no changes (see below).

## What `corgi_patched.c` changes vs. `corgi_v6.0.c`, and why

1. **Dropped `.handle_irq = pxa25x_handle_irq` from all three
   `MACHINE_START` blocks** (Corgi/Shepherd/Husky). `struct machine_desc`
   no longer has a `.handle_irq` field — confirmed by grepping
   `arch/arm/include/asm/mach/arch.h` and by `spitz.c`'s current
   `MACHINE_START` not setting it either. `pxa25x_init_irq()` wires the IRQ
   flow handler internally now.

2. **Dropped IrDA support** (`corgi_ficp_platform_data`, the
   `pxa_set_ficp_info()` call, and the `irda-pxaficp.h` include).
   `include/linux/platform_data/irda-pxaficp.h` is gone from mainline
   (404), consistent with the IrDA net subsystem being obsolete and mostly
   excised. No path back short of reviving `net/irda` wholesale — not worth
   it for a personal ROM.

3. **Replaced the UDC (USB gadget) platform_data wiring.**
   `pxa_set_udc_info()` / `pxa2xx_udc_mach_info` / `udc.h` are all gone.
   A commit from **2026-04-27** (`usb: udc: pxa: remove unused
   platform_data`) explicitly stripped this: *"None of the remaining
   boards put useful data into the platform_data structures, so
   effectively this only works with DT based probing... pxa25x version now
   does it the same way [as pxa27x]: gpio descriptors."*
   `pxa25x_udc.c` now does
   `devm_gpiod_get_index_optional(&pdev->dev, "pullup", 0, ...)` against
   `dev_name() == "pxa25x-udc"` (confirmed from `pxa25x_device_udc.name` in
   `devices.c`). The patched file adds a `gpiod_lookup_table` for that
   exact con-id instead — same `GPIO_LOOKUP_IDX` mechanism the file
   already uses for SPI chip-selects, so this isn't new territory, just
   applying an existing pattern to one more GPIO.

4. **`pxa_set_mci_info()` gained a second (properties) argument.**
   Current signature is
   `pxa_set_mci_info(const struct pxamci_platform_data *info, const struct property_entry *props)`.
   Passed `NULL` for `props` — the property/swnode path is how `spitz.c`
   wires MMC now, but plain `platform_data` + `NULL` props still works,
   it's just not the newest style. Fine to leave as a later polish pass.

Everything else in the file — SCOOP, SPI devices (ads7846/corgi-lcd/
max1111), matrix keypad, GPIO keys, GPIO LEDs, NAND, NOR flash, I2C,
`sharpsl_pm`'s own trivial API renames (`timer_delete_sync`, `remove()`
returning `void`) — compiles against the same code paths `spitz.c`
exercises today, so risk there is low.

## What's still open

- **`w100fb.c` now builds clean** (see status section above) — the
  "decade of API drift, multi-week" estimate turned out to be pessimistic;
  it only needed three mechanical fixes. **Still fully untested** on real
  hardware or emulation (QEMU's `spitz` has no W100 chip), so treat
  "compiles" as necessary, not sufficient.
- **Acceleration/rotation follow-up, not yet started.** The driver already
  has real 2D acceleration (`fb_fillrect`/`fb_copyarea` go through a
  Rage/Radeon-lineage 2D GUI engine via `mmDP_GUI_MASTER_CNTL`/ROP3 codes,
  not software blitting) — `fb_imageblit` is the one op left on the
  software path, and there's an `INIT_MODE_ROTATED`/
  `pixclk_divider_rotated` mode path whose scope (real hardware
  content-rotation vs. fixed panel-wiring timing) isn't confirmed yet.
  Worth investigating whether ATI's X-Forge 3D SDK (shipped for
  W100-based devices) documents capability beyond what this 2D-only
  fbdev driver ever exposed. Deliberately sequenced *after* getting a
  booted, displaying baseline on real hardware — no point optimizing a
  driver we haven't confirmed shows a picture yet.
- **USB gadget (UDC) fix is compile-verified, not runtime-verified** —
  builds clean against the `"pullup"` GPIO descriptor the current
  `pxa25x_udc.c` expects, but hasn't been tested on real hardware.
- **Nothing here has booted on real hardware or emulation yet.** QEMU
  never modeled Corgi specifically (only ever had Spitz/PXA270, and that
  was removed in QEMU 9.2 — last version with it is 9.1.0), so hardware
  boot-testing is the only way to validate this beyond "it compiles."
  Flash kernel-only (zImage + `updater.sh`, no `initrd.bin`) via CF/SD to
  keep it non-destructive to the existing Cacko rootfs.
- `Kconfig`/`Makefile` wiring (`MACH_CORGI`/`SHEPHERD`/`HUSKY`,
  `PXA_SHARP_C7xx`) is done and confirmed working — this part is no
  longer open.

## Toolchain notes

Built with crosstool-ng (`arm-unknown-linux-gnueabi`, armv5te/xscale/
soft-float), independent of the `retrocacko` project's own
`arm-unknown-linux-musleabi` toolchain (different project, unrelated
concerns — kernel builds don't link target libc, so musl vs. glibc is
irrelevant here). Two build issues worth remembering if rebuilding this
toolchain from scratch:

- **`libcody` (GCC's C++ modules support) fails against very new host
  compilers** (hit this against host GCC 16) with `char8_t`-related
  errors — `no matching function for call to 'S2C(const char8_t [2])'`
  across `buffer.cc`/`client.cc`/`cody.hh`/`server.cc`. Since kernel
  builds never need C++, the real fix is `CT_CC_LANG_CXX=n` (skips
  `libcody` entirely) — but note `ct-ng` fingerprints `.config` against
  saved restart checkpoints and refuses to resume across a config change,
  so decide on this *before* the first build attempt, not after a
  failure.
- **`CT_DEBUG_CT_SAVE_STEPS=y` is essential** for being able to resume
  after a failure without redoing the whole multi-stage bootstrap, but it
  keeps every completed step's build directory around instead of
  cleaning as it goes — this can fill disk fast on a tight partition.
  Delete superseded checkpoints under `.build/*/state/` (keep only the
  most recent) if space gets tight mid-build; the live `build/` working
  directory is also safe to delete between invocations since a resume
  restores it from the checkpoint anyway. Once the toolchain is installed
  to `~/x-tools/`, the entire `.build/` scratch tree is disposable.

## QEMU notes

Built QEMU **9.1.0** from source (`qemu-spitz/`) — the last release with
the `spitz` machine before it was removed in 9.2 (Sept 2024). Two
build-time issues, both the same "old software vs. this system's very new
host tooling" pattern as the toolchain's `libcody` problem:

- **`mkvenv`'s bootstrap needs Python's `distlib` module**, not present
  on this system and would otherwise need `sudo pacman -S
  python-distlib`. Worked around with a local venv instead (no root
  needed): `python3 -m venv pyvenv-bootstrap && pyvenv-bootstrap/bin/pip
  install distlib`, then `configure --python=.../pyvenv-bootstrap/bin/python3`.
- **`block/nfs.c` fails against this system's newer installed `libnfs`**
  (`nfs_pread_async`/`nfs_pwrite_async` signatures changed upstream in
  libnfs since QEMU 9.1 was released). We don't need NFS disk backend
  support to boot a local kernel image, so `configure
  --disable-libnfs` sidesteps it rather than patching dead code.

The minimal busybox rootfs used for the boot smoke test is built to a local
scratch directory (not tracked in git — the project's actual rootfs is
`rootfs/`, staged onto the device's `home` partition; this one is just a
throwaway QEMU boot aid) — `busybox-1.36.1` built statically
(`CONFIG_STATIC=y`), with the `tc` applet disabled (`networking/tc.c`
fails against current kernel headers with an incomplete `struct
tc_cbq_wrropt`; not needed for a shell). Applet symlinks were generated by
listing `busybox --list` through `qemu-arm` (user-mode emulation) since the
binary can't run directly on this x86_64 host.

## Our build toolchain (separate from the crosstool-ng thread above)

A second, independent toolchain is used for the actual kernel/flashing-tool
builds in this session's work: `dosbox-armv5-zaurus/buildroot`'s output
toolchain, at
`/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin/
arm-buildroot-linux-uclibcgnueabi-*`. It's a sibling project's buildroot
tree (built for that project's own DOSBox-on-Zaurus work), reused here
because it already has the OABI-clean uClibc patches this hardware's
ancient recovery kernel needs (see `docs/archive/DEADLETTER.md` for why OABI matters —
Cacko's recovery-mode kernel is 2.4.18, pre-EABI, and rejects any ELF with a
nonzero `EF_ARM_EABI_VER`).

### Kernel builds

```sh
cd kernel-src/linux-7.1.4
ARCH=arm CROSS_COMPILE=/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin/arm-buildroot-linux-uclibcgnueabi- \
    make -j$(nproc) zImage
```

Known regressions to check for after any `.config` change, before trusting
a build:

- **`ARCH_MULTI_V7` defaults to `y`** and will silently build an ARMv7
  kernel (wrong CPU family entirely) if `ARCH_MULTI_V5`/`CPU_XSCALE` aren't
  explicitly forced. Verify with `nm vmlinux | grep -c corgi_init` (should
  be `1`) after every build — a `0` here, even with no build errors, means
  the wrong machine/CPU config silently won.
- **Toggling `CONFIG_MODULES` off then back on collapses previously-`=m`
  symbols to `=y` permanently** on the next `oldconfig` (it doesn't re-ask
  about already-answered symbols). Never disable `MODULES` as an
  intermediate step when trimming a config — adjust individual symbols
  instead.
- **`ATAGS` gates the entire "Legacy board files" Kconfig section**
  (`MACH_CORGI`/`MACH_HUSKY`/etc. live inside `if ATAGS`) — starting from
  `allnoconfig` leaves the whole board-file section invisible until
  `ATAGS` is explicitly enabled.
- Always check the kernel's size against whatever NAND slot it's headed
  for *before* flashing — see `docs/DEADLETTER-MTD2-MTD3.md` for what happens
  when that budget gets exceeded even though the physical partition has
  more nominal room.

### Freestanding ARM host tools (`piko-install`, `piko-backup`, etc.)

These are hand-written, no-libc OABI binaries that run *inside Cacko's
recovery kernel* (not the mainline kernel above), built with the same
toolchain but very different flags — no kernel headers, no libc, custom
syscall macros (`swi` with the syscall number embedded in the immediate,
OABI convention, not the EABI r7-register convention):

```sh
GCC=/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin/arm-buildroot-linux-uclibcgnueabi-gcc
STRIP=/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin/arm-buildroot-linux-uclibcgnueabi-strip
$GCC -mabi=apcs-gnu -mfloat-abi=soft -march=armv5te -static -nostdlib \
     -ffreestanding -fno-builtin -Os -o piko-install piko-install-final.c -lgcc
$STRIP piko-install
```

Verify the result is genuinely OABI before ever putting it on the SD card:
`arm-buildroot-linux-uclibcgnueabi-readelf -h piko-install | grep -i flags`
should show `0x600` (no EABI version set). `0x5000200` ("Version5 EABI")
means the build accidentally picked up EABI flags and **will not run** on
the recovery kernel — this exact mistake happened once already (a stale
EABI-built `piko-install` sat on the card undetected until `readelf`
caught it).

`flash/encsh.c` (the `updater.sh` cipher tool) and `flash/piko-backup.c`
are built the same way. `encsh` itself is the one exception — it's a
**host-side** tool (operates on files before they reach the SD card, never
runs on the Zaurus), so it's built with the system's native `gcc`, not this
cross-toolchain.

## Launching QEMU for this project's builds

The `qemu-spitz/build/qemu-system-arm` binary (built by the crosstool-ng
thread above, see "QEMU notes") is reused here to smoke-test kernel builds
before ever flashing real hardware. Two things to know:

1. **QEMU's `-M spitz` needs `CONFIG_MACH_SPITZ=y` compiled in**, separate
   from whatever real-hardware machine type the board actually needs
   (`MACH_HUSKY` for this project's SL-C760/860 — see `docs/archive/HANDOFF.md` for why
   it's Husky, not Corgi, despite the codename). Multiple machine
   descriptors coexist fine in one kernel image; QEMU's board ID (713) and
   the real device's ID are matched independently at runtime. A kernel
   built without `MACH_SPITZ` will hang completely silently in QEMU
   (`dump_machine_table()` spinning) with **zero console output at
   all** — don't mistake this for a real regression before checking this
   first.
2. **`CONFIG_CMDLINE_FORCE=y` (needed on real hardware, forces
   `console=tty0`) silently discards QEMU's `-append "console=ttyS0"`.**
   For a QEMU test, temporarily flip the `.config`:
   ```sh
   ./scripts/config --enable MACH_SPITZ \
       --set-str CMDLINE "console=ttyS0 earlyprintk" \
       --disable CMDLINE_FORCE
   make ARCH=arm CROSS_COMPILE=... olddefconfig
   make ARCH=arm CROSS_COMPILE=... -j$(nproc) zImage
   ```
   then restore the real cmdline (`console=tty0 ...`, `CMDLINE_FORCE=y`)
   and rebuild before flashing anything to real hardware. Don't ship the
   QEMU-testing `.config` variant to the device.

Smoke-test invocation:

```sh
qemu-spitz/build/qemu-system-arm -M spitz \
    -kernel path/to/zImage \
    -append "console=ttyS0 earlyprintk" \
    -serial stdio -nographic -monitor none
```

A working boot prints `Machine: SHARP Husky` (or `Spitz`, depending on
which `.config` variant) within the first ~10 lines, followed by normal
kernel init through to a shell prompt if an initramfs is embedded. Total
silence (not even the QEMU deprecation warning's followup) almost always
means point 1 above, not a real kernel bug.
