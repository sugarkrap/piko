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
`docs/archive/` to keep the top level lean). **`docs/README.md` indexes
every document** — start there rather than guessing from filenames.

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

## Current state (2026-07-30, late)
The full two-stage chain works on real hardware and is **verified live**,
not inferred:

- **Bootstrap boots + kexecs into stage 2.** See
  `docs/DEADLETTER-BOOTSTRAP-BOOTS-2026-07-30.md` for the chain of fixes.
- **Stage 2 boots to a login/shell** with the w100 framebuffer up.
- **WiFi + SSH work.** The board holds a real DHCP lease (verified at
  `10.208.47.72` — *not* the `10.208.47.22` static fallback in
  `wifi-up.sh`, which is what a broken data path looks like) and dropbear
  accepts both password (`root`/`zaurus`) and key auth. Required the
  hostap `skb->cb` fix (`docs/archive/DEADLETTER-HOSTAP-SKB-CB.md`) and
  restoring the MEMC clock (skipping it silently killed PCMCIA).
- **Audio plays.** Needed two independent mainline fixes plus a mandatory
  mixer setting — see `docs/DEADLETTER-AUDIO-I2S-SILENT.md`. Do not treat
  a registered sound card as working audio; verify the `pxa-dma` interrupt
  count actually moves.

Not yet verified: MPlayer video playback (built via `tools/build-mplayer.sh`,
staged at `userspace/stage-mplayer/`, not yet run on hardware), and the
w100 vsync timeout (worked around in `w100fb_pan_display()`, root cause
still open).

**Routine updates no longer need a NAND flash.** With the device reachable
over WiFi, use `tools/build-and-deploy.sh` (rebuild + chunked SSH deploy)
followed by `softreboot` (self-kexec). Reserve the SD-card recovery flash
for bootstrap/`mtd1` changes or an unreachable board.

> Anything under `kernel-src/` is **regenerated** by
> `tools/setup-kernel-src.sh` from tracked sources in `modules/`. Editing
> `kernel-src/` directly survives until the next `--force-kernel-src`, then
> disappears with no warning. Every kernel fix must land in `modules/` with
> a matching `copy_in` line, and the honest way to prove it did is to run
> `build-and-deploy.sh --force-kernel-src` and confirm the affected object
> changes md5 while the others do not.
