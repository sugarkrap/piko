# How to build and deploy a new stage-2 kernel (+ modules)

*Written 2026-07-26, right after fixing the WiFi/PCMCIA ABI-mismatch
regression (`docs/archive/DEADLETTER-WIFI-SSH.md`) caused by redeploying a kernel
without its matching modules. This is now the canonical procedure —
follow it exactly, especially the "always deploy everything together"
rule.*

## Which path do I use?

There are **two** ways to get a new kernel onto this device. Pick based
on whether the device is currently reachable over SSH:

| Situation | Use |
|---|---|
| Device boots, WiFi/SSH work, you're updating the stage-2 kernel and/or modules | **`tools/build-and-deploy.sh`** (this doc) |
| Device is unreachable over SSH / unbootable / bricked | SD-card recovery flash — `docs/FLASH-MTD1-MTD3-SAFE.md` |
| You need to change the *bootstrap* partition itself (`mtd1`/`smf`, the tiny kexec loader) | SD-card recovery flash — `docs/FLASH-MTD1-MTD3-SAFE.md` |

The stage-2 kernel + rootfs live on `home` (`mtd3`), which is a normal
writable filesystem while the device is running — that's why it can be
updated directly over SSH with no NAND flash involved at all. NAND
flashing via the Cacko recovery menu + SD card is now reserved for the
bootstrap partition and true recovery, per `AGENTS.md`'s "last spare
board" constraint (scope every flash to only what changed).

## Normal path: `tools/build-and-deploy.sh`

```sh
tools/build-and-deploy.sh [user@host]      # defaults to root@10.43.112.72
```

Toolchain selection is automatic: the script uses the first compiler it
finds in `PATH` among `arm-buildroot-linux-uclibcgnueabi-gcc`,
`arm-unknown-linux-uclibcgnueabi-gcc`, `arm-linux-gnueabi-gcc`, and
`arm-unknown-linux-gnueabi-gcc`. You can override this explicitly with:

```sh
CROSS_COMPILE=arm-linux-gnueabi- tools/build-and-deploy.sh [user@host]
```

If your compiler binaries are not in `PATH`, point the script at them:

```sh
TOOLCHAIN_BIN_DIR=/path/to/toolchain/bin tools/build-and-deploy.sh [user@host]
```

This does four things, in order, and stops immediately if any step fails:

1. **Checks the device is reachable over SSH first** (fails fast with a
   pointer to the recovery doc if not — no point spending 15-25 minutes
   building if there's nothing to deploy to).
2. **Reconstructs `kernel-src/linux-7.1.4`** via `tools/setup-kernel-src.sh`
   — downloads a pristine kernel.org tarball and applies every tracked
   patch under `modules/` (Corgi board files, W100, sharpsl NAND,
   hostap_cs + lib80211, and the mach-pxa/wireless/crypto Kconfig+Makefile
   wiring). Idempotent — a marker file skips this once a tree is already
   patched, so it's cheap on every run. Pass `--force-kernel-src` to
   `build-and-deploy.sh` if you've changed a tracked patch file and need
   it re-applied.
3. **Cross-compiles `zImage` + all modules** with the buildroot toolchain,
   logging full (untruncated) output to `/tmp/kbuild-<timestamp>.log`.
3b. **Builds the cross-compiled userspace** via `tools/build-userspace.sh`
   — `userspace/src/md5sum`, then ALSA (`tools/build-alsa.sh`), then
   MPlayer (`tools/build-mplayer.sh`). That order is a hard dependency:
   MPlayer links `libasound.a` out of `userspace/stage-alsa`. Every step is
   idempotent, so this is cheap once built. Skip it with
   `--skip-userspace` (which also forwards `--no-userspace` to
   `chunked-deploy.sh`, so a stale staged payload isn't shipped either).
   The X11/matchbox stack is **not** built here — see that script's header.
4. **Deploys** by calling `tools/chunked-deploy.sh`, which pushes (over a
   known-flaky WiFi link, chunked + retried + size-verified):
   - the new `zImage` → `/boot/zImage-full` (auto-backs up the old one to
     `/boot/zImage-full.bak`)
   - all sound-stack modules → `/lib/modules/<kver>/zaurus-audio/`
   - **all WiFi/PCMCIA modules** → their real `/lib/modules/<kver>/kernel/...`
     depmod-tree paths (`pcmcia_core`, `pcmcia_rsrc`, `pcmcia`,
     `soc_common`, `pxa2xx_base`, `pxa2xx_sharpsl`, `hostap`, `hostap_cs`,
     `lib80211` + its WEP/CCMP/TKIP crypto modules, `libarc4`)
   - the `audioon`/`audinfo` helper scripts (single-word names, per
     `AGENTS.md`'s device-keyboard typing constraint), sent verbatim from
     `rootfs/usr/sbin/` — they used to be inline heredocs in
     `chunked-deploy.sh`, which created a second source of truth that
     silently overwrote committed edits to the tracked files
   - the media payload: MPlayer plus the `/usr/share/alsa` config tree and
     `aplay`/`amixer`/`alsactl`. Everything is statically linked, so the
     only "shared" dependency is that config tree — `libasound` opens
     `alsa.conf` by absolute path at runtime even when linked statically,
     and without it every PCM open fails with
     `Unknown PCM cards.pcm.default`.

     MPlayer is ~16 MiB against a ~68 MiB root jffs2. A preflight refuses
     the payload rather than half-deploying if it would leave under 4 MiB
     free (jffs2 needs room to garbage-collect, and a full root is not
     recoverable over SSH on this board). To keep it off flash entirely:

     ```sh
     MPLAYER_DEST=/mnt/card/mplayer tools/build-and-deploy.sh root@<ip>
     ```

     An `/mnt/card` destination is mounted automatically first and is not
     charged against the root filesystem budget.

It does **not** reboot the device automatically — you do that manually
once you're ready:

```sh
ssh -i ~/.ssh/zaurus_ed25519 root@10.43.112.72 reboot
```

### Why "always deploy everything together" matters

The 2026-07-26 regression happened because a kernel rebuild was deployed
*without* redeploying the WiFi/PCMCIA modules alongside it. The stale
on-device `.ko` files were built against a different kernel's
`struct module` ABI, so `insmod` failed with `section size must match`,
PCMCIA never came up, `wlan0` never appeared, and the device became
unreachable over its only remote-access path (WiFi/SSH — there is no
USB or serial cable, see `AGENTS.md`). Recovering required physical
console access.

`build-and-deploy.sh` / `chunked-deploy.sh` always push the **full**
matched set (kernel + every module actually loaded at boot) in one run,
specifically so this can't happen again. Do not hand-roll a partial
deploy (e.g. "just the sound modules" or "just the zImage") outside of
this script.

## Manual steps (if you need to do it by hand)

```sh
tools/setup-kernel-src.sh   # only needed once per fresh clone / patch change
cd kernel-src/linux-7.1.4
export PATH="$PWD/../../toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin:$PATH"
export ARCH=arm CROSS_COMPILE=arm-unknown-linux-uclibcgnueabi-
make -j"$(nproc)" zImage modules > /tmp/kbuild.log 2>&1
echo "exit: $?"
```

- **Always redirect full build output to a log file and grep it** — do
  not rely on `tail -N` to see the real error. A `-jN` parallel build
  interleaves hundreds of compile lines; the actual failing line is
  often far above the final `make[1]: *** Error 2` summary, which `tail`
  alone will hide.
- **Check local disk space before a build** — this repo's `.config` is
  broad (pulls in USB serial/DVB/XFS/etc. drivers unrelated to this
  board) and a full `zImage modules` build can consume several GB and
  15-25+ minutes. A build failing with a bare `Error 2` and no visible
  cause has been disk-space exhaustion before.
- Once built, deploy with `tools/chunked-deploy.sh [user@host]` directly
  (this is exactly what `build-and-deploy.sh` calls after a successful
  build).

## `.config` traps — re-check these after any config change

Each of these produces a build that **succeeds with no error** and is
silently wrong. Check them before trusting a kernel you are about to flash.

- **`ARCH_MULTI_V7` defaults to `y`** and will quietly build an ARMv7
  kernel — the wrong CPU family entirely — unless `ARCH_MULTI_V5` /
  `CPU_XSCALE` are explicitly forced. Verify after every build:

  ```sh
  nm vmlinux | grep -c corgi_init    # must be 1; 0 means the wrong config won
  ```

- **Toggling `CONFIG_MODULES` off and back on permanently collapses
  previously-`=m` symbols to `=y`** on the next `oldconfig` — it does not
  re-ask about already-answered symbols. Never disable `MODULES` as an
  intermediate step when trimming a config; adjust individual symbols.
- **`ATAGS` gates the entire "Legacy board files" section.**
  `MACH_CORGI` / `MACH_HUSKY` / etc. live inside `if ATAGS`, so starting
  from `allnoconfig` leaves the whole board-file section invisible until
  `ATAGS` is enabled.
- **Check the kernel's size against the NAND slot it is headed for
  *before* flashing.** See `docs/DEADLETTER-MTD2-MTD3.md` for what happens
  when that budget is exceeded even though the partition looks big enough.

## After reboot: verify

```sh
ssh -i ~/.ssh/zaurus_ed25519 root@10.43.112.72 "
    uname -a
    lsmod
    dmesg | grep -iE 'error|fail|section size|panic'
    ifconfig wlan0
    iwconfig wlan0
"
```

Expect: new kernel version/build date, all WiFi/PCMCIA + sound modules
`Live` with no errors, zero hits in the dmesg grep, `wlan0` up with an
IP and an associated AP in `iwconfig`.

### Verifying audio actually plays

The sound card registering is **not** evidence that audio works — it can
register, open, and report `state: RUNNING` while transferring nothing. Two
extra checks are cheap and catch the real failure modes (see
`docs/DEADLETTER-AUDIO-I2S-SILENT.md`):

```sh
grep pxa-dma /proc/interrupts       # note the count
aplay -d 4 /root/test-mono22k.wav   # must return exit 0, not hang
grep pxa-dma /proc/interrupts       # count MUST have increased
```

No `amixer` step is needed — the driver's defaults (`Jack Function=Off`,
`Speaker Function=On`, volume 121/127) are already the correct **speaker**
configuration and are verified audible as-is. `Jack Function=Off` means "no
jack plugged in", not "muted"; setting it to `Headphone` routes audio to the
headphone jack and will make the speaker test look broken.

A `pxa-dma` count that does not move means the I2S link is enabled but
unclocked — samples are never transferred no matter what ALSA reports. A
hang (rather than an error) is the same failure. Note there is no `kill`
applet in this busybox, so a hung `aplay` holds the PCM open until
`softreboot`.

## If it goes wrong (no post-kexec panic fallback)

The bootstrap's kexec fallback only covers `kexec -l` (load) failures —
if the new kernel loads but then panics/hangs *after* the jump, there is
**no automatic fallback**; every future power-cycle would retry the same
broken `zImage-full`. Since there's no serial/USB debug cable, recovery
is physical-console only, typed on the device's own limited keyboard
(avoids `/`, `:`, `[`, `]`, `|` — see `AGENTS.md`):

```
cd /boot
cp zImage-full.bak zImage-full
reboot
```

This restores the previous kernel+modules pairing that `chunked-deploy.sh`
automatically backed up before overwriting. Always confirm a build boots
before trusting it, and never let the `.bak` chain get more than one
generation deep without validating in between.
