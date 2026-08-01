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
| Iterating on the bootstrap kernel with the device on WiFi | `flash/run-stage2-smf-update.sh` |
| Device unreachable / unbootable | SD-card recovery flash — `docs/FLASH-MTD1-MTD3-SAFE.md` |

`piko-update` is the single updater for the whole ROM — one package covers
both partitions. But they are not equally dangerous, and the mechanism
deliberately keeps them apart.

| | `home` (mtd3) | `smf` (mtd1) |
|---|---|---|
| Holds | userspace, `/etc`, stage-2 kernel + modules | the tiny kexec bootstrap kernel |
| Seen by the updater as | ordinary files on a live JFFS2 filesystem | raw NAND behind the Sharp FTL |
| Installing means | `rename()` into place | erase + reprogram blocks |
| If it goes wrong | boot `zImage-full.bak`, or fix it over SSH | **the board does not boot, and there is no serial or USB to recover through** |
| How often | every package | only when the bootstrap really changed, and only on an explicit second command |

Both *can* be written while the system runs — nothing executes from either
partition at runtime, because the bootstrap kexec'd the stage-2 kernel
into RAM at boot. The asymmetry isn't whether a write is possible, it's
whether a torn write is survivable. See "Updating the bootstrap" below.

## Building a package

```sh
flash/build-update-package.sh [output.tar]      # defaults to ./update.tar
```

This cross-compiles `piko-update` itself, packages the entire
`rootfs/` overlay (`etc/*`, `usr/sbin/*`, `init` — whatever's actually
committed there, so this can't drift out of sync with a hand-picked file
list), adds the SSH file-transfer binaries if `userspace/stage-ssh` has
been built (`tools/build-ssh.sh` — `/usr/bin/scp`,
`/usr/libexec/sftp-server`, `/usr/bin/dbclient`, `/usr/bin/dropbearkey`;
the dropbear *server* only with `PIKO_SSH_REPLACE_DROPBEAR=1`, see
`docs/DEADLETTER-DROPBEAR-PTY.md` for why that one is opt-in), and — if a
built `kernel-src/linux-7.1.4` checkout is present
locally (see `docs/HOWTO-BUILD-DEPLOY-KERNEL.md`) — also includes the
stage-2 `zImage` and every module `chunked-deploy.sh` deploys. Every file
gets an MD5 in a `MANIFEST` entry written into the package.

**It also carries the X11/Matchbox desktop** (libX11/libXft/fontconfig/
freetype, the four Matchbox apps, `st`) if the X11 toolchain and
third-party deps are available locally — see `docs/HOWTO-MATCHBOX-DESKTOP.md`.
`tools/build-x11-stack.sh` + `tools/build-st.sh` + `tools/build-matchbox-
payload.sh` build/stage it (all idempotent), then every file in the
resulting payload is folded into `MANIFEST` individually. Same graceful
fallback as a missing `kernel-src/`: not fatal if the X11 prerequisites
aren't ready, the package just ships without the desktop, noted as such
in `MANIFEST`. Set `SKIP_X11=1` to skip it deliberately (e.g. a
kernel-only respin).

**kernel-src/ is gitignored and only reconstructed on demand.**
`tools/setup-kernel-src.sh` automates `docs/archive/HANDOFF.md`'s manual
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
(see `docs/archive/DEADLETTER-WIFI-SSH.md`). A package that fails this is never
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
4. If the package carries a bootstrap image (`boot/zImage-smf`), it is
   compared against what `smf` actually holds. Identical — the normal case
   — and nothing happens. Different, and `/boot/smf-pending` is written.
   **No NAND is erased or written here either way.**
5. The device reboots via `softreboot` (the proven-safe kexec self-jump —
   this hardware's normal reboot path is indistinguishable from a hard
   poweroff, see `softreboot`'s own comments).

Useful flags:

- `--dry-run` — verify only, never installs or reboots. Use this first to
  sanity-check a package before committing to it.
- `--no-reboot` — installs but leaves the reboot to you (run `softreboot`
  by hand when ready).
- `--commit-smf` — see below. Also reachable as `smfcommit`.

## Updating the bootstrap

Every package can carry the mtd1 bootstrap kernel, and shipping it costs
nothing: `piko-update` compares it against what's already in flash
(`piko-smf-write --compare`, read-only) and does nothing at all when they
match. Since the bootstrap changes maybe twice a year, essentially every
update takes that path and never goes near NAND.

When it *has* changed, the write is still not done there and then:

1. `piko-update` records `/boot/smf-pending` and reboots into the
   newly-installed stage-2 — **the old bootstrap is untouched and still
   working at this point.**
2. If the device comes back up, that reboot has just proved the new kernel
   and rootfs boot. `rcS` prints a reminder at the console.
3. You plug in the AC adapter and type:

   ```
   smfcommit
   ```

4. That re-verifies the staged image's MD5, re-checks that flash really
   differs, takes a **verified** full-partition backup to
   `/boot/smf-backup-<hash>.bin`, then erases and writes, and finally
   verifies the readback before clearing the marker.
5. The new bootstrap takes effect at the next **cold** boot.

If the device *doesn't* come back up in step 2, don't run `smfcommit` —
the old bootstrap is still in place, which is the whole point of the
ordering.

Note this is the reverse of `docs/FLASH-MTD1-MTD3-SAFE.md`, which flashes
mtd1 *before* mtd3. That procedure recovers a board that is already down;
this one runs on a board that currently boots, where a working mtd1 is the
only thing keeping it recoverable. Same constraint, opposite order.

`smfcommit` refuses on battery power (`--force` overrides). Losing power
partway through the erase+program window is the realistic way to lose this
board. If `/proc/apm` can't be read at all it warns and continues rather
than blocking — a sensor that is merely broken shouldn't make the tool
unusable — so check the adapter yourself.

There is no A/B slot for the bootstrap and there can't be: the Sharp
bootloader reads a fixed logical address (917504), so a second copy
elsewhere in the partition could never be selected. The ~5 MB free above
the kernel slot is not usable for this.

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
SYMLINK <path, relative to /> -> <target>
...
```

Every other entry must have a matching `MANIFEST` line and vice versa —
`flash/build-update-package.sh` always produces this shape; hand-rolling
one is only useful for testing `piko-update` itself.

The `SYMLINK` line exists for the X11/Matchbox payload's shared-library
SONAME aliases (e.g. `libX11.so.6 -> libX11.so.6.3.0`) — a symlink has no
content to hash, so its archive entry (tar typeflag `2`, no data blocks)
is matched by path and its `linkname` is cross-checked against the
`MANIFEST`-recorded target instead, same "trust nothing, verify
everything" spirit as the MD5 check on regular files.
