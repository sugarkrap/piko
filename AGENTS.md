# AGENTS.md — read this before working on zaurus-refresh

Mainline Linux (7.1.4) revival for a Sharp Zaurus SL-C760/SL-C860 (PXA255,
XScale ARMv5TE, "Husky" machine ID). Two-stage kexec boot: a tiny JFFS2+KEXEC
bootstrap kernel in the `smf` NAND partition kexecs a full stage-2 kernel +
rootfs stored on `home`. Full history and bug post-mortems are in `docs/`
(the `DEADLETTER-*.md` files — read the relevant one before touching kexec,
flashing, the cipher, etc.).

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
- md5-verify every file staged to the SD card.
- The SD card is shared with other sessions — always re-verify/regenerate
  `updater.sh` (the `piko-install`-invoking `flash/updater-encoded.sh`,
  built by `tools/encode-updater.py` — not tracked in git, see
  `docs/DEADLETTER-CIPHER.md`) before each flash.
- Recovery of last resort is the D+M service menu + a model-correct factory
  `.dbk` (`docs/DEADLETTER-NAND-RECOVERY.md`) — but never rely on it; avoid
  the mistake instead.

## Current state (2026-07)
Two-stage boot works end to end; stage 2 boots to a zsh login (users
`root`/`zaurus`, `piko`/`piko`). Service stack is BusyBox init + inittab +
rcS + mdev (NOT systemd — far too heavy for 64MB/400MHz/uClibc). WiFi:
wireless-tools + wpa_supplicant + hostap/Prism2 modules are in place; the
card associates but the data path is still being brought up. SSH via dropbear
is the goal once WiFi passes traffic. See `docs/archive/DEADLETTER-WIFI-SSH.md`.
