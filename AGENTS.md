# AGENTS.md — read this before working on zaurus-refresh

Mainline Linux (7.1.4) revival for a Sharp Zaurus SL-C760/SL-C860 (PXA255,
XScale ARMv5TE). **Machine ID is NOT "Husky."** Two numbers matter and they
are not the same thing:

- **19** — what the bootloader actually passes in register r1 at boot,
  confirmed by repeated LED-blink digit readouts on real hardware. Nothing
  in mainline's `arch/arm/tools/mach-types` claims 19 for this board (it's
  `L7200` upstream, unrelated hardware) — a new `MACHINE_START` descriptor
  had to be added that matches it anyway, purely because the alternative is
  `dump_machine_table()`'s silent infinite loop. **This is the number any
  mainline kernel booting this board must match against.**
- **196** — what Sharp's own factory kernel (extracted/decompiled from this
  board's NAND) calls itself internally ("SHARP Shepherd"). Useful
  historical/forensic context, and also now registered as a fallback
  `MACHINE_START`, but it is NOT what the bootloader passes and NOT
  primarily what boots this board.

See `docs/DEADLETTER-MACHINE-ID-196.md` for the full 19-vs-196 story. Two-
stage kexec boot: a tiny bootstrap kernel (embedded initramfs, runs
entirely in RAM — see `docs/DEADLETTER-BOOTSTRAP-BOOTS-2026-07-30.md`) in
the `smf` NAND partition fetches and kexecs a full stage-2 kernel + rootfs
stored on `home`. Full history and bug post-mortems are in `docs/` (the
`DEADLETTER-*.md` files — read the relevant one before touching kexec,
flashing, the cipher, etc.; resolved/historical material has been moved to
`docs/archive/` to keep the top level lean).

## HARD CONSTRAINTS — do not violate

### There is NO USB cable, and there never will be
The Zaurus's USB-client cable is **rare, not buyable on its own, and did not
ship with any Zaurus.** It is permanently out of the equation.

- **Never propose `usb0` / `g_ether` / USB-ethernet-gadget as a solution**
  (for SSH, networking, debugging, file transfer, anything). Do not suggest
  "just plug it into the laptop over USB." It cannot happen.
- `g_ether.ko` may still be built (harmless), but it is not a usable path.

### There is NO serial cable either
The serial/debug cable is likewise unavailable (expensive, same rarity). So:

- **The ONLY remote-access path to this device is WiFi → SSH** (Prism2
  PCMCIA card + dropbear). Getting WiFi fully working is therefore
  essential infrastructure, not a nice-to-have. Prioritize it.
- For pre-console kernel debugging there is no serial console — use the
  framebuffer console (`console=tty0`), the GPIO/LED boot markers in
  `corgi.c`/`pxa25x.c`/`head.S`, and `CONFIG_DEBUG_USER` + `user_debug=`
  for userspace faults.

### The device keyboard cannot type many characters
When the user operates the Zaurus directly, they **cannot type**: `/`  `:`
`[`  `]`  `|`  — and letters get transposed when reading photos of the
screen. There is no tab-completion or line editing in the bootstrap shell.

- **Do not ask the user to type slash/colon-heavy commands by hand.**
  Instead, ship **short, single-word helper scripts** on the device in a
  `$PATH` directory (e.g. `/usr/sbin/wifiup`, `/usr/sbin/netinfo`) that
  encapsulate the real commands. The user types the short name; the script
  does the slashes/colons/pipes internally.
- Prefer diagnostics that dump to the framebuffer for a photo over
  interactive typing.

### This is the LAST spare board
A previous board was permanently bricked (see `docs/DEADLETTER-MTD2-MTD3.md`).
There is no replacement.

- Scope every flash to **only what changed** — `mtd1` (bootstrap, `raw=0`
  nandlogical at offset **917504**) vs `mtd3` (home, `raw=1` eraseall+nandcp).
  Never copy the `raw`/offset fields between partitions
  (`docs/DEADLETTER-RAW-FLAG.md`, `docs/DEADLETTER-MTD1-OFFSET.md`).
- **`mtdN` numbering is context-dependent — always say which kernel.** The
  Cacko/recovery-menu kernel that `piko-install` runs under (and every
  flashing doc's `mtd1`/`mtd3` above) sees `mtd1=smf / mtd2=root /
  mtd3=home`. The mainline kernel this project builds sees `mtd0=smf /
  mtd1=root / mtd2=home` instead — one off, because a NOR "Filesystem"
  physmap device is defined in `modules/mach-pxa/corgi_patched.c` but never registered in
  `corgi_devices[]`, so mainline's NAND partitions start counting at
  `mtd0`. A bare "mtdN" with no kernel context is a trap; say which one.
- md5-verify every file staged to the SD card.
- The SD card is shared with other sessions — always re-verify/regenerate
  `updater.sh` (the `piko-install`-invoking `flash/updater-encoded.sh`,
  built by `tools/encode-updater.py` — not tracked in git, see
  `docs/DEADLETTER-CIPHER.md`) before each flash.
- Recovery of last resort is the D+M service menu + a model-correct factory
  `.dbk` (`docs/DEADLETTER-NAND-RECOVERY.md`) — but never rely on it; avoid
  the mistake instead.

## Current state (2026-07-30)
The bootstrap kernel boots successfully on real hardware as of tonight
(2026-07-30) — see `docs/DEADLETTER-BOOTSTRAP-BOOTS-2026-07-30.md` for the
full chain of fixes that took. It does not yet reach a working stage 2:
`home` currently holds an older stage-2 `zImage-full` built without
framebuffer support, so kexec into it (if it even completes) is headless.
A stage-2 rebuild with `CONFIG_FB_W100=y` is in progress. Once that lands,
stage 2's own login/service-stack state (BusyBox init + inittab + rcS +
mdev, zsh login as `root`/`zaurus` or `piko`/`piko`, wireless-tools +
wpa_supplicant + hostap/Prism2 for WiFi, dropbear for SSH) needs to be
re-verified on real hardware — treat prior claims about it working end to
end as unconfirmed until then. See `docs/archive/DEADLETTER-WIFI-SSH.md`
for the WiFi data-path history.
