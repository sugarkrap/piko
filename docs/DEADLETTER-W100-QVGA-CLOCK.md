# Dead Letter — QVGA mode runs the w100 at 1/6th the clock, not just 1/4 the pixels

*Written 2026-08-04, right after shipping `matchbox-fbrun --qvga` (PR #116)
and the w100fb 2D-accel ioctls (PR #118). Both were designed on the
assumption that QVGA is "the same work, at a quarter of the pixels" —
measured on hardware, that assumption is wrong in a way that specifically
undercuts the 2D-accel ioctls' whole reason for existing.*

## What was assumed

- `matchbox-fbrun --qvga`'s rationale (its own usage comment): a program
  that struggles at native resolution can "ask for a quarter of the
  pixels, for the same physical screen size" — implying roughly
  proportional speedup, since the panel does the pixel-doubling back up
  to native glass in hardware.
- `modules/w100/w100fb_accel.h`'s rationale: keep a sprite/tile cache in
  spare VRAM (only available once `xres`/`yres` is small enough to leave
  room — i.e., in QVGA) and have the 2D engine composite it in "instead of
  a CPU memcpy," implying the engine is the faster option.

Neither claim accounted for the mode switch also changing the chip's own
clock source.

## What is actually true

`w100fb_set_par()` → `w100fb_activate_var()` → `w100_init_clocks()`
(`modules/w100/w100fb_patched.c:1895-1906`) programs `sclk_cntl.sclk_src_sel
= mode->sysclk_src`. The mode comes from the board's table
(`modules/mach-pxa/corgi_patched.c:219-266`), selected by `xres`, and the
two entries differ on more than resolution:

| | 480×640 (landscape 640×480) | 240×320 (landscape 320×240) |
|---|---|---|
| `sysclk_src` | `CLK_SRC_PLL` | `CLK_SRC_XTAL` |
| `pll_freq` | 75 (100 in fast-PLL mode) | 0 — **no PLL at all** |
| `pixclk_src` | `CLK_SRC_PLL` | `CLK_SRC_XTAL` |

`corgi_fb_info.xtal_freq = 12500000` (`corgi_patched.c:277`). So in QVGA,
SCLK runs at **12.5 MHz instead of 75 MHz — a 6× drop** (8× against the
100 MHz fast-PLL mode).

SCLK is not a "just the display" clock. The fields `w100_init_clocks()`'s
sibling init function programs alongside it —`sclk_force_mc`,
`sclk_force_extmc` (the memory controller, internal *and* external),
`sclk_force_e2`/`sclk_force_e3`/`sclk_force_cp` (the 2D engines and
command processor) — are all on the same SCLK domain
(`w100fb_patched.c` around the `w100_soft_reset()` power-state init).
Dropping to XTAL doesn't just slow the display refresh; it slows the
memory controller and the 2D engine together.

**Measured on hardware:**

- **2D engine throughput: 19 → 3 MB/s = 6.3× slower** — tracks the 6×
  clock ratio almost exactly. The engine is close to purely SCLK-bound,
  with negligible fixed overhead.
- **CPU→framebuffer throughput: 9-14 → ~4 MB/s = 2.3-3.5× slower** — less
  than the clock ratio, because that path is partly bounded on the PXA
  side (bus arbitration, write-combining buffering) rather than purely by
  the w100's own memory controller clock.

## Why this matters for both features

QVGA has 1/4 the pixels of VGA (240×320 vs 480×640). Multiply that against
the measured per-byte slowdowns to get the real net effect on a
full-frame operation, not the naive "1/4 the pixels = 4× faster":

- **CPU blit to the framebuffer:** (1/4 the bytes) × (2.3-3.5× slower per
  byte) → net **1.14×-1.74× faster**, not 4×. Real, but a much smaller win
  than `--qvga`'s usage comment implies for a CPU-bound app.
- **2D engine fill/blit:** (1/4 the bytes) × (6.3× slower per byte) → net
  **~1.6× SLOWER**, not faster at all, despite processing a quarter of
  the pixels. In QVGA specifically, the 2D engine can be slower than just
  having the CPU write the pixels directly (3 MB/s engine vs ~4 MB/s CPU,
  measured) — the opposite of "hardware acceleration beats CPU".

The sharpest version of this: **QVGA is exactly the mode where spare VRAM
becomes available** (small visible frame leaves room in the fixed window
for a bigger virtual buffer — see PR #118's `smem_len` fix) — **and it is
also the mode where the 2D engine that's supposed to composite that spare
VRAM efficiently is at its slowest relative to CPU.** The feature that
unlocks the accel ioctls' intended use case is the same feature that
weakens their reason for existing. This doesn't make either feature
wrong, but "use the 2D engine instead of a CPU memcpy" is not a safe
default assumption in QVGA — measure the actual op size and pick
accordingly, the same "measure before optimizing" lesson this project has
hit before (`DEADLETTER-W100-VSYNC.md`).

## What this does NOT mean

- **Not a driver bug.** `pll_freq = 0` / `CLK_SRC_XTAL` for the QVGA
  table entry is very likely a deliberate power-saving choice (skip PLL
  power draw for the smaller, presumably lower-priority mode) — dropping
  SCLK for a smaller mode is a normal, defensible use case for XTAL,
  it just wasn't decided with this project's accel-ioctl use case in mind
  originally.

## Open follow-up (not started)

Whether it's worth adding a way to force the PLL to stay active in QVGA
mode (e.g. a module parameter, or the `fastpll_mode`/`fast_pll_freq`
mechanism `w100_init_clocks()` already has a branch for) for the specific
case where a program wants QVGA's spare VRAM *and* full-clock 2D
compositing, trading the power saving for engine throughput. Not
attempted here — needs a real decision about whether that tradeoff is
wanted before touching clock programming on the one board this project
has.

## Docs updated as a result

- `userspace/src/matchbox-fbrun.cxx`'s `--qvga` usage comment — added the
  measured throughput caveat.
- `modules/w100/w100fb_accel.h`'s rationale comment — added the same, with
  the specific "engine can be slower than CPU in QVGA" warning.
