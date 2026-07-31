# LCD smearing on the C760: panel sampling phase

*Solved 2026-07-31. The panel was latching pixel data at the wrong point
in the pixel clock, because the per-unit phase calibration does not
survive our two-stage kexec boot.*

## Symptom

Fine **vertical striping** across flat mid-grey areas, and visible
smearing on detailed UI content. Reported originally as "a ghosting
effect, only in Matchbox, not in X" — which was true but badly
misleading, and cost a lot of time.

It is not Matchbox-specific. Matchbox is simply the only thing on this
system that draws high-frequency detail. The X root weave under `-retro`
and large solid blocks have almost nothing to smear, so they look fine.
Flat **mid-grey** is the most legible test target: white and black sit
at the drive rails and hide the artifact.

## Root cause

`phadadj` is the panel's data sampling phase. Like `comadj` (VCOM) and
the touchscreen calibration, it is per-unit factory data delivered in the
Sharp param block at physical `0xa0000a00`, and it does not reach our
stage-2 kernel — see the boot line:

```
corgi-lcd spi1.1: VCOM comadj=125 from built-in default -- param block
                  MISSING; phadadj=-1; battery adadj=-1
```

Crucially, the stock fallback for `phadadj` is **not a tuned default**.
It is "no phase bits set", i.e. phase 0:

```c
adj = sharpsl_param.phadadj;
adj = (adj < 0) ? PHACTRL_PHASE_MANUAL
                : PHACTRL_PHASE_MANUAL | ((adj & 0xf) << 1);
```

So every board that loses the param block runs phase 0 regardless of what
its panel actually wants. Latching mid-transition makes adjacent pixels
bleed together — vertical striping in flat fields, smearing on detail.

## The fix

`modules/lcd/corgi_lcd_patched.c` supplies a real fallback:

```c
#define DEFAULT_PHAD_VGA      (6)
```

**6 is this unit's value**, found by sweeping. A board whose param block
survives uses its own value and never reaches this default.

## Re-tuning on a different unit

The same file exposes the value as a writable param, so no rebuild is
needed to find it (`CONFIG_LCD_CORGI=y`, so there is no module to
reload — the param is writable purely for this):

```sh
fbtest blocks                                     # userspace/src/fbtest.c
for v in 0 2 4 6 8 10 12 14; do
    echo $v > /sys/module/corgi_lcd/parameters/phadadj
    sleep 4                                       # watch the GREY block
done
```

Valid range 0–15. The screen blinks on each write — that is the panel
power-on sequence re-running, not a fault. Once you have the value, put
it in `DEFAULT_PHAD_VGA` and rebuild, or pass `corgi_lcd.phadadj=N` on
the kernel command line.

> Note: the cmdline route means editing the kexec `--append` in
> `modules/initramfs/init`, which lives in the **stage-1 bootstrap** —
> reflashing mtd1. The driver default is a stage-2 change and therefore
> the cheap path.

`comadj` (VCOM) is exposed the same way and works the same, if a board
ever does need it.

## Ruled out along the way — don't redo these

**VCOM (`comadj`).** The leading hypothesis for a long time, and wrong.
Sweeping 60→155 changes nothing visible, and the writes demonstrably
land: one write produces 102 SPI transfers, exactly matching the
M62332FP bit-bang sequence (start + 3 bytes x 8 bits x 3 + acks + stop +
surrounding register writes).

**W100 PLL frequency (`fastpllclk`).** Toggling Corgi's `.pll_freq`
(75MHz) to `.fast_pll_freq` (100MHz) makes no visible difference, tested
both idle and under a continuous-rewrite stress load. `rcS` keeps it at
0, where an earlier session found it eliminated a 1px horizontal jitter.

This is also the mainline equivalent of the widely-repeated Cacko fix
`echo 75 > /proc/driver/w100/fastsysclk`. Two things to know: mainline
made the knob a **boolean** where Cacko took a target MHz, and 75MHz is
already our default — so that specific tip is a no-op here. The Cacko
advice was real, but it was solving a different problem.

**Software / X / Matchbox.** A framebuffer capture of the desktop is
pixel-clean at high zoom, and `fbtest` reproduces the artifact writing
straight to `/dev/fb0` with X uninvolved.

## Method note

A photo of the screen and a framebuffer capture answer *different*
questions, and both were needed here:

- `fbgrab` + `tools/decode-fb.py` proved the pixel data was correct,
  ruling out the whole software stack.
- But a capture says nothing about what the panel does with that data,
  and cannot show a temporal artifact at all. When the user said they
  saw something the capture did not, the capture was **not** the
  authority — their eyes were.
- `fbtest` patterns exist to drive that: `hlines` (clean here),
  `blocks` (shows it, especially the grey one), `pairs`, `vlines`.

`/dev/fb0` rejects `read(2)` on this board (`dd` gives `EINVAL` at any
block size); it must be `mmap`ed, which is why `fbgrab` exists at all.

## Still open: the same lost param block

`adadj = -1` in that boot line is the battery ADC calibration, which is
why `/proc/apm` reports

```
1.13 1.2 0x02 0xff 0xff 0xff -1% -1 ?
```

`mb-applet-battery` forces `PERCENTAGE = -1` when it reads `<= 0`, clears
its gauge to black and skips the fill — so the empty black gauge under
the battery icon means "level unknown", not a rendering bug. (`miniapm.png`
is a battery cylinder in rows 2–18 *plus a separate gauge* in rows 21–31;
it renders 1:1 at 32x32 and the applet's bar maths lands exactly on it.)

`'TUCH'` is likewise why the touchscreen calibration is hardcoded as
`EVDEV_ABS_CAL_*` in the kdrive `evdev.c`.

Recovering the block properly — having stage 1 pass it to stage 2, or
reading it out of NAND once and pinning the values — would fix the
battery readout and the touchscreen calibration in one go, and would make
`DEFAULT_PHAD_VGA` unnecessary.

## Gotcha for anyone touching this code

Every field of `struct sharpsl_param_info` is `unsigned int` while
"absent" is stored as `-1`, so a `< 0` test **must** cast to `int` first
or it is silently always-false. The stock driver relies on an implicit
conversion; a version of this patch nearly shipped that pushed
`0xffffffff` into the VCOM DAC as 255.
