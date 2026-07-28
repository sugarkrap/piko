# How to update the ROM without WiFi (`piko-update`)

*Written 2026-07-28. Every update path before this one
(`tools/build-and-deploy.sh` / `tools/chunked-deploy.sh`) needs the device
reachable over WiFi/SSH — a bar this hardware often can't clear (no CF
WiFi card compatible with this kernel, or no subnet to join). `piko-update`
removes that dependency: point it at a package file on the SD card and it
updates the running system directly, no network involved at all.*

## Which path do I use?

| Situation | Use |
|---|---|
| Device boots, WiFi/SSH work | `tools/build-and-deploy.sh` — see `docs/HOWTO-BUILD-DEPLOY-KERNEL.md` |
| Device boots, but WiFi/SSH is unavailable or you'd rather not depend on it | **`piko-update`** (this doc) |
| Device unreachable / unbootable, or the `smf`/mtd1 bootstrap partition itself needs to change | SD-card recovery flash — `docs/FLASH-MTD1-MTD3-SAFE.md` |

`piko-update` only ever touches the `home` partition (mtd3) — the same
territory `chunked-deploy.sh` already updates live over SSH (stage-2
kernel, modules, `/etc`, `/usr/sbin`). It never does raw NAND I/O and
never touches `smf`/mtd1; that stays `flash/picoupdate.sh`'s job.

## Building a package

```sh
flash/build-update-package.sh [output.tar]      # defaults to ./update.tar
```

This cross-compiles `piko-update` itself, packages the entire
`rootfs/` overlay (`etc/*`, `usr/sbin/*`, `init` — whatever's actually
committed there, so this can't drift out of sync with a hand-picked file
list), and — if a built `kernel-src/linux-7.1.4` checkout is present
locally (see `docs/HOWTO-BUILD-DEPLOY-KERNEL.md`) — also includes the
stage-2 `zImage` and every module `chunked-deploy.sh` deploys. Every file
gets an MD5 in a `MANIFEST` entry written into the package.

**kernel-src/ is gitignored and only reconstructed on demand.**
`flash/setup-kernel-src.sh` automates `docs/HANDOFF.md`'s manual
reconstruction procedure: download a pristine kernel.org tarball, then
apply every hand-patched file this repo tracks under `modules/` — Corgi
board files, the W100 driver, the sharpsl NAND driver, hostap_cs +
lib80211 + michael_mic, and the `mach-pxa`/`wireless`/`crypto`
Kconfig+Makefile wiring for `MACH_CORGI`/`SHEPHERD`/`HUSKY` and the
`lib80211`/`CRYPTO_MICHAEL_MIC` re-additions (`modules/mach-pxa/`,
`modules/wireless/`, `modules/crypto/` — full working copies pulled
directly from the machine that built `zImage-corgi-7.1.4`). This is
everything needed; reconstruction should always succeed given kernel.org
access. CI (`.github/workflows/build-update-package.yml`) runs this same
script and caches the reconstructed tree, so a run where nothing
kernel-related changed restores instantly instead of re-downloading. If
reconstruction ever does fail for some other reason (a kernel.org hiccup,
an `oldconfig` failure), both CI and `build-update-package.sh` fall back
to a rootfs-only package rather than failing outright.

**Whenever CI does produce a full package, it's boot-tested before being
uploaded.** `flash/qemu-smoke-test.sh` boots the built kernel under QEMU
(`-M spitz`), insmod's every shipped kernel module, and runs
`piko-update --dry-run` against the actual package — catching the same
kernel/module ABI mismatch that broke this device for real once already
(see `docs/DEADLETTER-WIFI-SSH.md`). A package that fails this is never
uploaded. This only runs for full packages (nothing kernel-side to test
in a rootfs-only one), and it validates the *shared* PXA2xx boot path —
QEMU's spitz machine is PXA270 with no W100 chip, so `corgi.c`/
`corgi_pm.c`/`w100fb.c` themselves never execute here. It is not a
substitute for real hardware.

## Applying it on the device

Copy the resulting `update.tar` to the SD card, then on the device:

```
piko-update /mnt/card/update.tar
```

What happens, in order:

1. Every file in the archive is streamed into a scratch tree
   (`/tmp/piko-update.staging`) and its content MD5-checked against
   `MANIFEST` — **nothing under `/` is touched during this step.** Any
   mismatch, truncated archive, or file missing from either the archive
   or the manifest aborts the whole update cleanly.
2. Only once every file verifies does it get installed: each staged file
   is renamed into place (this rootfs has no separate `/tmp` tmpfs, so
   staging and the real destinations are the same filesystem — the
   install step is a metadata-only rename, not a write to a live path).
3. If the package replaces `/boot/zImage-full`, the previous copy is
   renamed to `/boot/zImage-full.bak` first — the exact file the existing
   panic-recovery procedure in `docs/HOWTO-BUILD-DEPLOY-KERNEL.md` already
   looks for (`cp zImage-full.bak zImage-full; reboot`, done by hand at
   the console since there's no serial/USB cable).
4. The device reboots via `softreboot` (the proven-safe kexec self-jump —
   this hardware's normal reboot path is indistinguishable from a hard
   poweroff, see `softreboot`'s own comments).

Useful flags:

- `--dry-run` — verify only, never installs or reboots. Use this first to
  sanity-check a package before committing to it.
- `--no-reboot` — installs but leaves the reboot to you (run `softreboot`
  by hand when ready).

**Config files and userspace binaries are always overwritten, no conflict
detection.** If you've hand-edited something under `/etc` or `/usr/sbin`,
`piko-update` will replace it same as anything else — the ROM is too
young to build a merge/conflict story yet, so for now the rule is simply
"the package wins." Revisit this once real conflicts start happening in
practice.

## Package format, if you're building tooling against it

A plain (uncompressed) POSIX `ustar` tar — deliberately not `.zip` and not
gzipped: `piko-update` reads the archive itself with a small from-scratch
reader (see `userspace/src/piko-update.c`), since this rootfs's busybox
build can't be assumed to have `tar`/`unzip`/`gzip`/`md5sum` available
(the same reasoning `chunked-deploy.sh` already documents for why it
ships its own `md5sum`). The first entry must be a file named `MANIFEST`:

```
PIKO-UPDATE-PACKAGE 1
# free-text lines starting with '#' are printed on the device and ignored
<32-hex-char md5>  <path, relative to />
...
```

Every other entry must have a matching `MANIFEST` line and vice versa —
`flash/build-update-package.sh` always produces this shape; hand-rolling
one is only useful for testing `piko-update` itself.
