# Documentation index

Three kinds of document live here:

- **HOWTO-** — the current way to do a thing. Follow these.
- **DEADLETTER-** — post-mortems. Each one is a bug that already cost real
  time or real hardware, written up so it cannot happen twice. Read the
  relevant one *before* touching that area, not after.
- **FLASH-** — procedures that write to NAND. Highest-risk operations here.

`archive/` holds material that is resolved or superseded. It is kept for
the reasoning, not as instructions — do not follow it.

## Start here

| Doc | What it covers |
|---|---|
| [`../AGENTS.md`](../AGENTS.md) | **Read first.** Hard constraints: no USB, no serial, machine ID 19 vs 196, one spare board. |
| [`BUILD-FROM-SCRATCH.md`](BUILD-FROM-SCRATCH.md) | Fresh clone → running desktop. Host prerequisites, toolchain, the gaps that are still manual. |

## How-to

| Doc | What it covers |
|---|---|
| [`HOWTO-BUILD-DEPLOY-KERNEL.md`](HOWTO-BUILD-DEPLOY-KERNEL.md) | The routine loop: rebuild the stage-2 kernel + modules, deploy over SSH, verify. Includes the `.config` traps that silently produce a wrong kernel. |
| [`HOWTO-QEMU-SMOKE-TEST.md`](HOWTO-QEMU-SMOKE-TEST.md) | Boot a build under QEMU before it touches the board — and the two traps that look like kernel bugs but are not. |
| [`HOWTO-OFFLINE-UPDATE.md`](HOWTO-OFFLINE-UPDATE.md) | Building the self-contained update package, and how CI boot-tests it. |
| [`HOWTO-MATCHBOX-DESKTOP.md`](HOWTO-MATCHBOX-DESKTOP.md) | Building the X11 + Matchbox stack. Contains the non-guessable version pins and configure lines. |
| [`HOWTO-X11-TOUCHSCREEN.md`](HOWTO-X11-TOUCHSCREEN.md) | Getting the touchscreen working as an absolute pointer under Xfbdev. |
| [`HOWTO-SCREEN-ROTATION.md`](HOWTO-SCREEN-ROTATION.md) | Turning the display around on the swivel hinge — in the w100's CRTC, for free. Why not `xrandr`. |
| [`HOWTO-LCD-PHASE-CALIBRATION.md`](HOWTO-LCD-PHASE-CALIBRATION.md) | Fixing LCD smearing via the panel's sampling phase. |
| [`HOWTO-OVERCLOCK.md`](HOWTO-OVERCLOCK.md) | CPU speed steps and the `mhz` tool — which frequencies the PXA255 can actually produce, and what the memory bus does when you raise them. |
| [`HOWTO-RTC-TIME.md`](HOWTO-RTC-TIME.md) | The real-time clock: why the board used to boot at 1970, and the `settime` / `ntpsync` tools. |

## Flashing (destructive)

| Doc | What it covers |
|---|---|
| [`FLASH-MTD1-MTD3-SAFE.md`](FLASH-MTD1-MTD3-SAFE.md) | The safe NAND flash procedure. Read fully before any flash. |

## Post-mortems

Boot and machine identity:

| Doc | The bug |
|---|---|
| [`DEADLETTER-MACHINE-ID-196.md`](DEADLETTER-MACHINE-ID-196.md) | The 19-vs-196 machine ID story — why "flashes fine, never boots". |
| [`DEADLETTER-BOOTSTRAP-BOOTS-2026-07-30.md`](DEADLETTER-BOOTSTRAP-BOOTS-2026-07-30.md) | The chain of fixes that got the bootstrap kernel to kexec stage 2. |
| [`DEADLETTER-KEXEC-SYSCALL.md`](DEADLETTER-KEXEC-SYSCALL.md) | The kexec syscall path. |
| [`DEADLETTER-LED-MARKERS.md`](DEADLETTER-LED-MARKERS.md) | Why LED boot markers can silently lie to you. |

NAND and flashing:

| Doc | The bug |
|---|---|
| [`DEADLETTER-MTD2-MTD3.md`](DEADLETTER-MTD2-MTD3.md) | **The bricked board.** How it happened. |
| [`DEADLETTER-MTD1-OFFSET.md`](DEADLETTER-MTD1-OFFSET.md) | The `mtd1` offset (917504) and nandlogical path. |
| [`DEADLETTER-RAW-FLAG.md`](DEADLETTER-RAW-FLAG.md) | Why the `raw` flag must never be copied between partitions. |
| [`DEADLETTER-NAND-RECOVERY.md`](DEADLETTER-NAND-RECOVERY.md) | Last-resort recovery: the D+M service menu and factory `.dbk`. |
| [`DEADLETTER-CIPHER.md`](DEADLETTER-CIPHER.md) | The `updater.sh` cipher, and regenerating it before every flash. |

Devices and drivers:

| Doc | The bug |
|---|---|
| [`DEADLETTER-AUDIO-I2S-SILENT.md`](DEADLETTER-AUDIO-I2S-SILENT.md) | Registered sound card ≠ working audio. Two mainline fixes plus a mandatory mixer setting. |
| [`DEADLETTER-W100-VSYNC.md`](DEADLETTER-W100-VSYNC.md) | The w100 vsync timeout — worked around, root cause still open. |
| [`DEADLETTER-DROPBEAR-PTY.md`](DEADLETTER-DROPBEAR-PTY.md) | Dropbear PTY allocation. |

## Archive

Superseded or resolved. Kept for the reasoning.

| Doc | Why it is kept |
|---|---|
| [`archive/PORTING-NOTES-2026-07.md`](archive/PORTING-NOTES-2026-07.md) | The original README: which mainline APIs broke and why when the removed board files were pulled forward. Still the best explanation of what `modules/` actually changes. |
| [`archive/HANDOFF.md`](archive/HANDOFF.md) | The original manual kernel-reconstruction procedure, now automated by `tools/setup-kernel-src.sh`. |
| [`archive/HANDOFF-2026-07-28-X11-XFBDEV.md`](archive/HANDOFF-2026-07-28-X11-XFBDEV.md) | X11/Xfbdev bring-up snapshot. |
| [`archive/DEADLETTER.md`](archive/DEADLETTER.md) | The original combined post-mortem log, since split into the files above. |
| [`archive/DEADLETTER-HOSTAP-SKB-CB.md`](archive/DEADLETTER-HOSTAP-SKB-CB.md) | The hostap `skb->cb` fix that made WiFi work. |
| [`archive/DEADLETTER-WIFI-SSH.md`](archive/DEADLETTER-WIFI-SSH.md) | The kernel/module ABI mismatch that broke the device once. |
| [`archive/DEADLETTER-KEXEC-ATAGS.md`](archive/DEADLETTER-KEXEC-ATAGS.md) | ATAGS handling across kexec. |
| [`archive/DEADLETTER-STAGE2-INIT.md`](archive/DEADLETTER-STAGE2-INIT.md) | Stage-2 init bring-up. |
| [`archive/WARNING.md`](archive/WARNING.md) | Superseded warning notice. |
| [`archive/LOG-shepherd-discovery-20260729.txt`](archive/LOG-shepherd-discovery-20260729.txt) | Raw device log from the machine-ID discovery session. Evidence, not instructions. |
