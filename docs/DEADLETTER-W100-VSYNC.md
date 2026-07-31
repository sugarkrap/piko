# Dead Letter — w100 vsync: what was actually wrong (and what wasn't)

*Written 2026-07-30. Notable mostly as a correction: the thing this was
filed against — "the vline status bit never asserts" — turned out to be
false. Measuring it took about ten minutes and invalidated a comment that
had already been propagated into the source and would have sent the next
person chasing a hardware fault that does not exist.*

---

## What was believed

That `w100_vsync()` fails on this hardware: the vline status bit in
`mmGEN_INT_STATUS` never asserts, `w100fb_pan_display()` therefore always
got `-ETIMEDOUT`, and `vsync_mode=1` (the CRTC frame-counter path) was the
suggested workaround. That claim was written into a comment in
`w100fb_pan_display()`.

## What is actually true

Measured with `FBIO_WAITFORVSYNC` on `/dev/fb0` (a small static ARM binary;
the device has no `devmem` and no `debugfs`):

```
mode 0 (vline irq status)        mode 1 (CRTC frame counter)
wait 0: ret=0 elapsed=39064 us   wait 0: ret=-1 ETIMEDOUT elapsed=63689 us
wait 1: ret=0 elapsed=38365 us   wait 1: ret=-1 ETIMEDOUT elapsed=62050 us
wait 2: ret=0 elapsed=39482 us   wait 2: ret=-1 ETIMEDOUT elapsed=61540 us
...  8/8 succeed                 ...  8/8 fail
```

- **Mode 0 works.** 8/8, consistently ~39 ms. The *only* vline timeout ever
  logged on a running system is a single one at `t=0.866s`, during the very
  first mode set, before the CRTC is running — expected and harmless.
- **Mode 1 is the broken one.** `mmCRTC_FRAME` reads back a hardwired `0x0`
  and never increments. So does `mmCRTC_FRAME_VPOS`. Confirmed by sampling
  both over `/dev/mem` while the panel was actively displaying:
  ```
  CRTC_FRAME sampled 8x:      00000000 00000000 ... (never changes)
  CRTC_FRAME_VPOS sampled 8x: 00000000 00000000 ... (never changes)
  ```
  So the recommended workaround was the one path that cannot work here.

The panel period really is ~39 ms (**~25.6 Hz**), not the ~16.8 ms a 60 Hz
panel would give. `CRTC_TOTAL` reads `htotal=651 vtotal=643`, matching the
480x640 mode in `corgi_fb_modes[]` (480+86+85, 640+3+0). The low refresh is
a hardware/mode property, not a bug introduced here — but it is what makes
the old timeout marginal, below.

## The real defects

**1. The timeout was an iteration count pretending to be a duration.**
```c
int timeout = 30000;  /* VSync timeout = 30[ms] > 16.8[ms] */
...
udelay(1);
timeout--;
```
Each iteration also does a `readl` across the slow external bus, so 30000
iterations actually cost **~61 ms** of wall time (measured via the mode-1
timeout path), not 30 ms. And the period it is racing is ~39 ms, not the
16.8 ms the comment assumes. So the budget's true length depended on bus
timing and sat close enough to the real period to be luck-of-the-draw under
load. Replaced with an explicit `ktime` deadline
(`W100_VSYNC_TIMEOUT_MS = 100`, ~2.5 frames at the measured rate).

**2. Success was inferred from "budget left", not from seeing the event.**
The old code returned 0 whenever the counter had not hit zero. That
conflates "saw the vline assert" with "ran out of *clear* attempts but had
counter left", and reports a timeout when the clear phase alone drained the
budget. Now tracked explicitly with a `got_vline` flag, and both loops share
one deadline so a slow clear phase cannot silently starve the wait.

**3. It busy-waited a full frame at 100% CPU.**
`udelay(1)` in a loop, up to ~39 ms, on every single call — and
`w100fb_pan_display()` calls it on every pan. A double-buffered userspace
(MPlayer `-vo fbdev`, X11) therefore spent an entire frame period spinning
on a 400 MHz PXA255 that needs those cycles to decode.

Now it sleeps between polls. **This is only safe because the bit is
latched**: `mmGEN_INT_STATUS` is write-1-to-clear, so once the vline event
happens the bit stays set until cleared. A coarse poll cannot miss it — it
only costs a little latency in *noticing* it. Do not convert this to a
level-sensitive read without revisiting that.

Measured over 20 waits (~780 ms wall), `/proc/stat` deltas in 10 ms ticks:
```
user +2   system +29   idle +42
```
i.e. **~420 ms now returned to idle** instead of being burned in a spin
loop. Roughly 24 ms per frame handed back to the decoder.

`pan_display` can in principle be reached from fbcon in a non-sleepable
context, so the helper falls back to a coarse `udelay(100)` when
`!preemptible()` rather than assuming process context.

## Why not use the interrupt instead

That would be the proper fix, and it is not available: the Corgi w100
platform device declares **only a MEM resource, no IRQ**
(`corgi_fb_resources[]` in `modules/mach-pxa/corgi_patched.c` — a single
`IORESOURCE_MEM` covering `0x08000000-0x08ffffff`). The chip's vline
interrupt output is not routed to a PXA GPIO on this board, so there is
nothing to request. Polling is the only option; the goal is only to poll
cheaply.

## Standing notes

- **`vsync_mode=1` is not a fallback on this board.** It is documented in
  the module param description and in the timeout message now, so nobody
  re-derives this. Keep mode 0.
- Ignoring the `w100_vsync()` return value in `w100fb_pan_display()` is
  still correct and is kept: a failed vsync means "possible tearing", not
  "the pan is impossible". Failing the pan breaks panning for every
  userspace consumer for no benefit.
- The ~25.6 Hz refresh is worth remembering when judging video performance —
  the panel itself caps pan-driven output around 25 fps regardless of how
  fast the decoder is.

## Reproducing the measurements

The two throwaway diagnostics used here are worth rebuilding if this is ever
revisited: one calls `FBIO_WAITFORVSYNC` in a loop and prints `ret`/`errno`
and elapsed microseconds per call; the other `mmap`s `/dev/mem` at
`0x08010000` (the w100 register block — same mapping the `w100-warmup` code
in `corgi_patched.c` uses) and samples `mmCRTC_FRAME`, `mmCRTC_FRAME_VPOS`
and `mmGEN_INT_STATUS` repeatedly. Sampling a register several times and
checking whether it *changes* is the cheap way to tell "running" from
"enabled but dead" — the same trick that cracked the I2S bug the same night
(`DEADLETTER-AUDIO-I2S-SILENT.md`).
