# piko — mainline Linux on a Sharp Zaurus SL-C760/C860

Mainline Linux **7.1.4** running on 2003 hardware (PXA255, XScale ARMv5TE,
64 MB RAM), booting straight to a graphical desktop — on a board whose
support was deleted from the kernel in 2023.

> **I AM NOT RESPONSIBLE FOR ANY BRICKED DEVICE. USE THIS AT YOUR OWN
> RISK.** This project has already permanently bricked one board; see
> [`docs/DEADLETTER-MTD2-MTD3.md`](docs/DEADLETTER-MTD2-MTD3.md) for the
> full account of how and why.

## Status — 2026-07-31

Boots unattended to a Matchbox desktop, verified on real hardware:

```
two-stage kexec boot  ->  Xfbdev (kdrive)  ->  matchbox-session
                                                 |- matchbox-desktop
                                                 |- matchbox-panel
                                                 '- matchbox-window-manager
```

**Working:** framebuffer X, built-in keyboard (custom XKB layout for the
Zaurus Fn symbol row), touchscreen as an absolute pointer, WiFi (Prism2
PCMCIA), SSH, audio, SD card, MPlayer, real-time clock (`settime`, and an
automatic NTP sync once WiFi is up).

**Open:** the w100 vsync timeout is worked around in `w100fb_pan_display()`
rather than root-caused ([`DEADLETTER-W100-VSYNC.md`](docs/DEADLETTER-W100-VSYNC.md)),
and the X.Org/Matchbox component builds are not yet scripted.

## Quick start

Full instructions — including host prerequisites, the toolchain rebuild and
the device-side bootstrap — are in
**[`docs/BUILD-FROM-SCRATCH.md`](docs/BUILD-FROM-SCRATCH.md)**. In short:

```sh
git lfs install                       # flash/mtd3.jffs2 is an LFS object
git clone --recurse-submodules <repo> piko && cd piko
tools/build-uclibc-toolchain.sh       # one-off, slow (tens of minutes+)
tools/setup-kernel-src.sh --force     # fetch kernel.org tree + apply our patches
tools/build-and-deploy.sh             # rebuild + deploy to a reachable device
```

Once the board is on WiFi, routine updates go over SSH — **no NAND flash
needed**. Reserve the SD-card recovery flash for bootstrap (`mtd1`) changes
or an unreachable board.

## Repository map

| Path | What it is |
|---|---|
| `modules/` | **Source of truth** for every kernel change, grouped by the subsystem it patches. |
| `kernel-src/` | Regenerated from `modules/` by `setup-kernel-src.sh`. Gitignored — edits here vanish. |
| `tools/` | Build/deploy scripts. `build-and-deploy.sh` is the main entry point. |
| `flash/` | SD-card recovery path: freestanding OABI binaries + `updater.sh` tooling. |
| `rootfs/` | Hand-written device config and helper scripts staged onto the `home` partition. |
| `userspace/` | Cross-compiled userland: X11, Matchbox, wireless tools, ALSA, MPlayer. |
| `initramfs/` | Bootstrap-kernel initramfs, rebuilt by `build-initramfs.sh`. |
| `toolchain/` | Locally built cross-toolchain. Gitignored, multi-GB. |
| `docs/` | How-tos and post-mortems — see [`docs/README.md`](docs/README.md). |

Anything gitignored is *regenerable*; the `.gitignore` documents the
reasoning per directory and is worth reading before adding files.

## Before you change anything

Read **[`AGENTS.md`](AGENTS.md)**. It carries the hard constraints that
explain why this project is shaped the way it is — no USB cable, no serial
cable, a keyboard that cannot type `/` or `:`, and exactly one spare board
left. They are not preferences.

The `docs/DEADLETTER-*.md` files are post-mortems of bugs that already cost
real time or real hardware. Read the relevant one *before* touching kexec,
flashing, the cipher, or the machine ID.
