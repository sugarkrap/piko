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
| Device boots, WiFi/SSH work, you're updating the stage-2 kernel and/or modules | **`flash/build-and-deploy.sh`** (this doc) |
| Device is unreachable over SSH / unbootable / bricked | SD-card recovery flash — `flash/FLASH-MTD1-MTD3-SAFE.md` |
| You need to change the *bootstrap* partition itself (`mtd1`/`smf`, the tiny kexec loader) | SD-card recovery flash — `flash/FLASH-MTD1-MTD3-SAFE.md` |

The stage-2 kernel + rootfs live on `home` (`mtd3`), which is a normal
writable filesystem while the device is running — that's why it can be
updated directly over SSH with no NAND flash involved at all. NAND
flashing via the Cacko recovery menu + SD card is now reserved for the
bootstrap partition and true recovery, per `AGENTS.md`'s "last spare
board" constraint (scope every flash to only what changed).

## Normal path: `flash/build-and-deploy.sh`

```sh
flash/build-and-deploy.sh [user@host]      # defaults to root@10.43.112.72
```

This does three things, in order, and stops immediately if any step fails:

1. **Checks the device is reachable over SSH first** (fails fast with a
   pointer to the recovery doc if not — no point spending 15-25 minutes
   building if there's nothing to deploy to).
2. **Cross-compiles `zImage` + all modules** with the buildroot toolchain,
   logging full (untruncated) output to `/tmp/kbuild-<timestamp>.log`.
3. **Deploys** by calling `flash/chunked-deploy.sh`, which pushes (over a
   known-flaky WiFi link, chunked + retried + size-verified):
   - the new `zImage` → `/boot/zImage-full` (auto-backs up the old one to
     `/boot/zImage-full.bak`)
   - all sound-stack modules → `/lib/modules/<kver>/zaurus-audio/`
   - **all WiFi/PCMCIA modules** → their real `/lib/modules/<kver>/kernel/...`
     depmod-tree paths (`pcmcia_core`, `pcmcia_rsrc`, `pcmcia`,
     `soc_common`, `pxa2xx_base`, `pxa2xx_sharpsl`, `hostap`, `hostap_cs`,
     `lib80211` + its WEP/CCMP/TKIP crypto modules, `libarc4`)
   - the `audioon`/`audinfo` helper scripts (single-word names, per
     `AGENTS.md`'s device-keyboard typing constraint)

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
cd kernel-src/linux-7.1.4
export PATH="/home/makaron/Code/dosbox-armv5-zaurus/buildroot/output/host/bin:$PATH"
export ARCH=arm CROSS_COMPILE=arm-buildroot-linux-uclibcgnueabi-
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
- Once built, deploy with `flash/chunked-deploy.sh [user@host]` directly
  (this is exactly what `build-and-deploy.sh` calls after a successful
  build).

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
