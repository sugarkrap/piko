# Handoff — verify the w100fb video-mem fix and 2D accel ioctls

> **RESOLVED 2026-08-04 — done on the real board, archived.** Everything
> below is the original handoff, kept for its reasoning; the outcome is at
> the top. PR #118 was merged; one defect the review had not caught was
> found and fixed before this ran (see "The bug found on the way").

## Outcome

Run against the board over WiFi (`root@10.208.47.1`), on `Linux zaurus
7.1.4 #54 PREEMPT Tue Aug 4 20:43:39 CEST 2026`:

| Mode | Result |
|---|---|
| `probe` | **PASS** — `W100FB_IOC_SYNC` answered; the deployed kernel has the ioctls |
| `accel` | **PASS** — FILL and BLIT readback, all 16×16 pixels `0x07e0` |
| `spare` | **PASS** — off-screen sprite composited on-screen, all 16×16 pixels `0x001f` |
| `smem` | **SKIP** — see below; not a driver problem |

`spare` is the one that matters: an off-screen sprite living in spare VRAM,
blitted onto the visible framebuffer by the 2D engine. That is the feature
PR #118 exists for, and it works on real hardware.

### The bug found on the way

`w100_accel_fillrect()`/`w100_accel_copyarea()` wrote their pitch argument
straight into `mmDST_PITCH`/`mmSRC_PITCH`, which are **pixel**-granularity
registers (`w100_init_graphic_engine()` programs them with a bare
`par->xres`; `mmGRAPHIC_PITCH`, a few functions away, is the byte-
granularity one). Every caller was passing bytes — the fbcon hooks passed
`par->xres*BITS_PER_PIXEL/8`, and `w100fb_accel.h` documents its ioctl
fields as bytes per row. At 16bpp that is exactly 2× the pitch the engine
wants, which does not fault or clip: it strides twice as far per row and
shears the output diagonally. It also regressed fbcon's own fills and
scrolls, which had inherited the correct pitch from mode-set time before
PR #118 generalized these primitives.

Fixed in `w100fb: hand the 2D engine a pixel pitch, not a byte pitch` —
the primitives take `*_pitch_px` and the four callers convert, with
`w100fb_rect_fits()` rejecting a byte pitch that is not a whole number of
pixels. `accel` passing above is the hardware confirmation: with the
doubled pitch, only row 0 of a 16×16 fill would have landed where the
readback looks.

### Why `smem` reports SKIP

The check was "did `smem_len` grow after claiming a bigger
`yres_virtual`". On this device it can't: `smem_len` is already 2097152
(the full external SDRAM window, well past the 393216 internal-SRAM
bucket) *before* the test claims anything, so there is no room to grow —
and the old failure text then asserted `set_par()` had left external SDRAM
unmapped, which 2 MiB flatly contradicts. It was a false negative, and it
cost a real detour.

The precondition is not reachable from a live session: the desktop runs
double-buffered VGA (640×960 virtual = 1,228,800 bytes, already past the
threshold), and `matchbox-fbrun --qvga` dropping to a small mode does not
power external memory back off. Verified separately that the mode switch
itself works — `virtual_size` 640,960 → 320,240 and stride 1280 → 640
inside the wrapper.

`tools/src/w100accel-test.c` now tests the property that was actually
meant — does `smem_len` cover the virtual size `set_par()` accepted — and
reports SKIP, not FAIL, when external memory was already on. Isolating the
video-mem fix needs the board to reach a small mode without a large
virtual buffer having been claimed first (booting straight into QVGA);
`spare` exercises the same memory for real and has no such limitation.

## What this is

[PR #118](https://github.com/sugarkrap/piko/pull/118), branch
`claude/matchbox-fbrun-video-mode-ho7r0b`, two changes to
`modules/w100/w100fb_patched.c`:

1. **The video-mem fix.** `w100fb_set_par()` used to decide whether
   external SDRAM gets mapped in (`extmem_active`) from the *visible*
   frame size alone. `w100fb_check_var()` approves a virtual
   (pannable/spare) buffer independently of that, so a small visible mode
   (QVGA) with a larger `yres_virtual` could get approved by `check_var()`
   while `set_par()` left external SDRAM suspended — `smem_len` then
   under-reported what was really usable, and anything past the internal-
   SRAM threshold landed on memory that had been powered down, not just
   "not yours". Now decided from whichever is larger, the visible frame
   or the accepted virtual size.
2. **Public 2D accel ioctls.** `W100FB_IOC_FILL` / `W100FB_IOC_BLIT` /
   `W100FB_IOC_SYNC` (new `modules/w100/w100fb_accel.h`), generalizing the
   existing `fillrect`/`copyarea` engine primitives to target any
   `(offset, pitch)` surface in the framebuffer's mapped memory, not just
   the live framebuffer — so a process can keep a sprite/tile cache in
   spare VRAM (now honestly exposed by fix #1) and have the chip's 2D
   engine composite it in, instead of a CPU `memcpy`.

Neither has been compiled against a real kernel tree or run on hardware.
That's this handoff's job.

## How do I know the fixes are actually there?

The PR is **open, not merged** — that was deliberate (see the PR
description: uncompiled kernel code, last spare board). Two ways to check
what's actually in a given tree:

```sh
# Is the branch's tip present locally / does it have the commit?
git log --oneline -1 claude/matchbox-fbrun-video-mode-ho7r0b -- modules/w100/w100fb_patched.c

# Does a given checkout (e.g. what tools/build-and-deploy.sh is about to
# build) actually have both changes? These are unique to this PR --
# grep for them rather than trusting a branch name or commit message:
grep -n "want_extmem" modules/w100/w100fb_patched.c        # fix #1
grep -n "W100FB_IOC_FILL" modules/w100/w100fb_patched.c    # fix #2 wired into the ioctl handler
test -f modules/w100/w100fb_accel.h && echo "accel header present"
```

If you're building from `main` and these greps come up empty, the PR
hasn't been merged yet — merge it (or build straight from the branch)
before continuing. If you're not sure which checkout you're in:
`git branch --show-current` and `git log -1 --oneline`.

## Build + deploy

`CONFIG_FB_W100=y` in `kernel.config-corgi-7.1.4` — **built in, not a
module.** A `w100fb.ko` reload won't pick this up; it needs a full kernel
rebuild and redeploy.

```sh
tools/build-and-deploy.sh root@<device-ip>     # see docs/HOWTO-BUILD-DEPLOY-KERNEL.md
```

If the device is unreachable, `--build-only` builds without one (see that
doc for the SD-card recovery path — should not be needed here, `home`
(`mtd3`) takes kernel updates over SSH with no NAND flash involved).

Read `docs/HOWTO-BUILD-DEPLOY-KERNEL.md` in full before doing this if you
haven't — in particular "always deploy everything together" (a kernel
without its matching modules bricked SSH access once already, see
`docs/archive/DEADLETTER-WIFI-SSH.md`) and the `.config` traps section.

After deploying and rebooting, confirm the new kernel is actually the one
running before testing anything:

```sh
ssh root@<device-ip> "dmesg | grep -i w100fb"
```

## Test

A ready-to-run test tool is already in this PR:
`tools/src/w100accel-test.c` (same style as the existing
`tools/src/fbflip.c` diagnostic — read that file's own header comment if
this is your first time building one of these).

```sh
arm-unknown-linux-uclibcgnueabi-gcc -march=armv5te -O2 -static \
    -Wall -Wextra -o w100accel-test tools/src/w100accel-test.c
scp w100accel-test root@<device-ip>:/tmp/
```

Then, over SSH on the device, **in this order**:

```sh
/tmp/w100accel-test probe    # confirms the new kernel actually has the ioctls
/tmp/w100accel-test accel    # 2D engine only, on-screen, safe regardless of mode
matchbox-fbrun --qvga -- /tmp/w100accel-test smem    # video-mem fix
matchbox-fbrun --qvga -- /tmp/w100accel-test spare   # BOTH fixes together, end to end
```

`smem` and `spare` need a small visible mode to be meaningful — at native
VGA resolution the visible frame alone already forces external memory on
regardless of whether the fix is present, so the test would pass
trivially either way. `--qvga` (from this same PR's sibling work,
`matchbox-fbrun`'s video-mode switch) is the easiest way to get there and
reverts automatically when the test program exits — that's exactly the
tool that flag exists for.

Each mode prints `PASS:`/`FAIL:` lines and exits nonzero if anything
failed, so it's fine to script:

```sh
for m in probe accel; do /tmp/w100accel-test $m || echo "$m: FAILED"; done
matchbox-fbrun --qvga -- sh -c '/tmp/w100accel-test smem; /tmp/w100accel-test spare'
```

### What each mode actually proves

| Mode | Proves | If it fails |
|---|---|---|
| `probe` | The running kernel has the new ioctls at all | Wrong kernel deployed — redeploy, or the greps above came up empty on the tree you built from |
| `accel` | FILL/BLIT/SYNC work against the **live framebuffer** surface | 2D-engine wiring is broken independent of fix #1 — read the ioctl error string, it names the failing ioctl |
| `smem` | `smem_len` actually grows once a bigger virtual buffer is claimed in a small mode | Fix #1 is missing or reverted — the exact bug the PR describes |
| `spare` | An **off-screen** sprite (fix #1's spare VRAM) gets composited onto the screen by the 2D engine (fix #2) — the actual feature | Fails cleanly, not silently: if fix #1 is missing, the kernel's own `w100fb_rect_fits()` bounds check rejects the off-screen FILL with `-EINVAL` before this test's own checks even run — the error message says so |

## If something fails

- **`probe` fails** → wrong kernel. Re-check the greps above against
  `kernel-src/linux-7.1.4/drivers/video/fbdev/w100fb.c` (the copy that
  actually got compiled, via `tools/setup-kernel-src.sh`), not just this
  repo's `modules/w100/w100fb_patched.c` — if you changed a tracked patch
  file and didn't pass `--force-kernel-src` to `build-and-deploy.sh`, the
  stale copy in `kernel-src/` is what got built.
- **`smem` fails** → fix #1 regressed or was never applied. This is a
  pure logic bug in `w100fb_set_par()`, no hardware surprises expected —
  re-read the PR description's explanation and compare against the
  current `modules/w100/w100fb_patched.c`.
- **`accel`/`spare` FILL or BLIT ioctl itself fails (not the pixel
  readback)** → likely a real hardware/register surprise the review
  didn't catch (wrong offset math, scissor interaction, FIFO timing).
  Worth a `DEADLETTER-*.md` if it's a genuine driver bug, same as
  `docs/DEADLETTER-W100-VSYNC.md` for the panning work this ioctl code
  reuses primitives from.
- **Pixel readback is wrong but the ioctl call itself succeeded** → the
  engine did *something*, just not what was asked — check
  `w100_accel_fillrect()`/`w100_accel_copyarea()`'s register writes
  against `w100fb_fillrect()`/`w100fb_copyarea()`'s original (pre-PR)
  behavior; `git log -p` the PR commit for the exact diff.
- **Screen visibly corrupts, device becomes unreachable** → do **not**
  keep iterating on hardware. Recover per `docs/HOWTO-BUILD-DEPLOY-KERNEL.md`
  / `docs/FLASH-MTD1-MTD3-SAFE.md` as needed, write up what happened as a
  new `docs/DEADLETTER-*.md`, and stop — this is the one board.

## When this is done

If everything above passes on real hardware: update the PR description's
test-plan checklist, merge it, and delete this file (or move it to
`docs/archive/` per this repo's own convention for resolved handoffs —
see `archive/HANDOFF-2026-07-28-X11-XFBDEV.md` for the pattern). If
something is genuinely broken and fixed along the way, a `DEADLETTER-*.md`
for it is more valuable long-term than a comment buried in a PR thread.
