# Dead Letter — w100 clock domains: what got raised, what didn't, and why

*Written 2026-08-05. Closes out the "w100 clock domains: runtime control +
rotated-mode refresh fix" handoff. Read `docs/DEADLETTER-W100-VSYNC.md`
first — this builds directly on it (same PCLK/SCLK/PLL model, same
~25.6 Hz rotated-mode starting point).*

---

## What shipped

Three new sysfs attributes on `/sys/devices/platform/w100fb/`, all in
`modules/w100/w100fb_patched.c` / `modules/w100/w100fb.h`:

- **`clocks`** (read-only) — dumps `xtal`/`pll`/`sclk`/`pclk` in Hz, the
  current mode/rotation/divider, and raw (uncalibrated) `CLK_TEST_CNTL`
  counts. Computed from tracked state (`par->pll_freq_hz`, the mode
  table's src/divider fields), not measured — cross-checked once against
  an independent `FBIO_WAITFORVSYNC` loop and agreed to within
  measurement noise.
- **`sysclk`** (read-write, MHz) — runtime PLL override. Resolves against
  `xtal_12500000[]` on an exact hit, or synthesizes a `w100_pll_info` via
  `w100_pll_compute()` otherwise (the `f = xtal/(M+1) * (N_int+1+N_fac/8)`
  formula, M fixed at 0). Clamped to 50–125 MHz in the kernel
  (`W100_PLL_MAX_MHZ`/`W100_PLL_MIN_MHZ`). A failed lock recovers to
  `par->pll_freq_last_good` rather than stranding the chip on a
  half-applied bad value. Persists across mode changes via
  `w100_target_pll_mhz()`, which `w100_init_clocks()` and `calc_hsync()`
  both defer to.
- **`pixclk`** (read-write, Hz) — runtime pixel-clock override, resolved
  per-orientation every time `w100_set_dispregs()` runs (rotated and
  non-rotated legitimately want different dividers off the same source).
  Hard-clamped to 25.0 MHz (`W100_PCLK_MAX_HZ`) **unconditionally in the
  kernel**, not just at the sysfs entry point — this is the one value in
  the whole plan that can run the panel out of spec rather than merely
  corrupt a frame.
- **`sdram_mode_reg`** (read-write, hex) — CAS-latency bisection tool,
  added mid-session once the 100 MHz SCLK failure below pointed at it.
  Does **not** take effect as a live write: it only changes what the next
  genuine external-memory off→on transition programs (see
  `w100_setup_memory()`'s JEDEC-style precharge/MRS sequence). Poking a
  live, actively-scanned-out SDRAM's mode register directly would not be
  safe — there is no way to reissue that sequence without first taking
  the memory offline.

All four stayed reachable at runtime, per the plan's own rule, before
anything went into a compiled-in default.

## What got validated on hardware, and what didn't

| Config | Result |
|---|---|
| pixclk div=5 (12.5 MHz) @ PLL=75, VGA rotated | Clean — pixel-perfect FILL/BLIT, 30/30 vsync waits, ~30 Hz (up from stock ~25.6 Hz) |
| pixclk div=4 (15 MHz) @ PLL=75, VGA rotated | **Visible garbage.** Reverted. Panel-scanout-timing limit, unrelated to SDRAM. |
| sysclk=100 MHz, stock CL2, VGA (external SDRAM live) | **Visible garbage**, twice, reproducibly |
| sysclk=100 MHz, **CL3** (`sdram_mode_reg=0x650031`), VGA | Clean — pixel-perfect FILL/BLIT, 30/30 vsync waits |
| sysclk=125 MHz, CL3, VGA | **Subtle failure** — 3/256 pixels wrong in a BLIT readback (single-bit diff, 0x07e0 vs 0x07c0), invisible on screen and to vsync timing |
| sysclk=75 MHz, CL3, VGA | Clean (confirmed before baking CL3 in as the universal default — see below) |
| sysclk=100/125 MHz, stock CL2, **QVGA** (internal SRAM only) | Clean at both — this is Stage 1's original walk, later reconfirmed after the bug fix below |

The 125 MHz / external-SDRAM combination remains unresolved. It is not a
dead end — `ext_timing_cntl`/`io_cntl` were never touched, only
`sdram_mode_reg`'s CAS-latency field — but nothing beyond CL3 was tried
this session. The 3-wrong-pixels-out-of-256 result is also worth
remembering on its own: it is exactly the kind of failure a screenshot or
a vsync-timing check cannot catch. **Trust the pixel-perfect
`W100FB_IOC_FILL`/`BLIT` readback test over "the screen looks fine" for
any future memory-timing change** — `tools/src/w100accel-test.c accel`
mode is the tool, and its `info` mode's `fix.smem_len` is how you confirm
external memory is actually live/idle rather than assuming it from
resolution (see the bug below).

## The bug: QVGA didn't reliably mean "external memory off"

Found by hand, mid-session, while investigating why `fix.smem_len`
reported the full external-memory size right after switching to a
240x320 mode that should have fit in internal SRAM.

`w100fb_set_par()` decides whether external SDRAM needs to be mapped
(`want_extmem`) from the larger of the visible frame and the requested
virtual size:

```c
needed = max_t(unsigned long,
               (unsigned long)par->xres * par->yres,
               (unsigned long)info->var.xres_virtual * info->var.yres_virtual)
         * BITS_PER_PIXEL / 8;
```

`par->xres`/`par->yres` are the **old** resolution — this function
assigns the new one a few lines later, `par->xres = info->var.xres;`.
`info->var.xres`/`info->var.yres` already hold the new,
`check_var()`-validated target (the fbdev core runs `check_var()` before
`set_par()` and stores the result into `info->var` first). So dropping
from the 640x480 desktop to 240x320 computed `needed` against the OLD
640x480 (307200 px) instead of the NEW 240x320 (76800 px) — comfortably
past the internal-SRAM threshold either way, so `want_extmem` came out
true and external SDRAM never actually powered down.

**This silently defeated every "switch to QVGA to keep a test off
external memory" step this plan depends on**, including this session's
own first attempt at the CAS-latency test and, per the smem_len evidence,
very possibly parts of the original Stage 1 PLL walk too (that walk still
passed cleanly regardless — plausibly because QVGA's much lighter actual
memory traffic doesn't expose a marginal SDRAM-timing issue the same way
full VGA scanout does, but this was never controlled for at the time).

Fixed with a one-line change: read `info->var.xres`/`info->var.yres`
instead of `par->xres`/`par->yres`. Verified via `w100accel-test info`'s
`fix.smem_len` both before (2097152, external) and after (393216,
internal) the fix, on the same 640x480→240x320 transition.

**There is no symptom for this beyond checking `fix.smem_len` directly.**
No dmesg warning, no visible corruption at QVGA itself (small buffer, low
traffic). If you are relying on "switch to QVGA" as a safety property for
any future w100 memory-timing work, confirm it with
`w100accel-test info`, don't assume it from the resolution alone.

## What got baked in (`modules/mach-pxa/corgi_patched.c`)

Per the plan's own rule — nothing baked in without a runtime-validated
value behind it:

- **`corgi_fb_mem.sdram_mode_reg`: `0x00650021` → `0x00650031`** (CAS
  latency 2 → 3). This is a universal default, not per-mode — validated
  clean at 75 MHz (the normal-speed VGA default) and 100 MHz (the
  pre-existing `fast_pll_freq=100` VGA path, which had shipped since
  before this file's own git history but had apparently never actually
  been run in VGA before this session, since the corruption it caused was
  new information here). A higher CAS latency only adds settling margin —
  it cannot remove margin a lower-frequency clock didn't need — and that
  reasoning was itself confirmed on hardware (pixel-perfect at 75 MHz +
  CL3) rather than left as an assumption.
- **QVGA mode's `pll_freq`: 75 → 100, `fast_pll_freq`: 0 → 100** — wait,
  **125**. QVGA never touches external SDRAM (see above), so this is a
  much lower-stakes change than the VGA one; both values were walked and
  reconfirmed clean after the `smem_len` bug fix, with external memory
  genuinely verified off both times.
- **VGA mode's `pixclk_divider_rotated`: left at 6.** The validated div=5
  improvement (~30 Hz) is deliberately **not** baked in, because this
  field is shared between the normal (75 MHz) and `fastpllclk` (100 MHz)
  PLL states — at 100 MHz the same divider gives 16.7 MHz, close to the
  already-failed 15 MHz (div=4 @ 75 MHz), and almost certainly the same
  panel-scanout-timing failure (a function of the actual pixel-clock Hz,
  not which divider/PLL combination produced it, unlike the SDRAM
  timing issue above). Baking div=5 in here would silently turn the
  pre-existing `fastpllclk` toggle into a corruption trigger. The
  `pixclk` sysfs attribute still reaches div=5 at runtime today; it
  just isn't the compiled-in default. Fixing this properly needs
  per-speed dividers (see Stage 4 below).

Verified on a cold boot after deploy: clean dmesg (only the one expected
pre-CRTC vsync timeout), pixel-perfect FILL/BLIT, clean vsync waits, and
both the QVGA 100/125 MHz path (via a resolution switch + `fastpllclk`)
and the VGA 75 MHz + CL3 default confirmed working from a genuine
power-on state, not just a runtime override.

## What's still open

- **125 MHz with external SDRAM live.** Untried: a CAS latency above 3,
  or adjusting `ext_timing_cntl`/`io_cntl` alongside `sdram_mode_reg`.
  Whether external memory clocks 1:1 off SCLK or through a divider in
  `ext_cntl` is also still unknown — same open question
  `docs/DEADLETTER-W100-VSYNC.md`'s handoff left about the `BM_MEM_*`
  register set.
- **Per-speed pixclk dividers** (`pixclk_divider_fast`,
  `pixclk_divider_rotated_fast` on `struct w100_mode`, selected on
  `par->fastpll_mode` the way `w100_set_dispregs()` already selects
  `pixclk_divider` vs `_rotated` on orientation). Needed before the
  validated div=5 rotated-mode improvement can be baked in safely
  alongside the existing `fastpllclk` toggle.
- **Live `pixclock` reporting** (`w100fb_check_var()` still hardcodes
  `var->pixclock = 0x04`) and **real SCLK power management**
  (`sclk_post_div_slow` still tracks `_fast` 1:1) were both out of scope
  this session — see the original handoff's stage 4 for the sketch of
  each.
