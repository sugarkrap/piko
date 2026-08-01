# Dead Letter — LED boot markers lie, and a 16-checkpoint bisect built on them

*Written 2026-07-29. Read this before using either LED as a boot-progress
signal, and before trusting `corgi.c`'s own comments about them.*

---

## Summary

Chasing why a freshly-built `mtd1` bootstrap kernel wouldn't boot, we
added GPIO/LED "checkpoint" markers at successively earlier points in the
boot path — 16 of them, each costing a real NAND flash cycle on the last
spare board — and bisected the failure down from "somewhere in boot" to a
single function. The conclusion was wrong, because **the instrument was
never validated.**

The tell was visible in the raw data the whole time:

| Marker operation | Count | "Fired"? |
|---|---|---|
| Turns an LED **ON** | 3 | **all 3** |
| Turns an LED **OFF** | 13 | **none** |

A 100% correlation with *what the marker does* rather than *where it sits*
in the boot path. The decisive case: checkpoint 16 sits only a handful of
instructions after checkpoint 3, same function, no branches between them —
yet 3 "fired" and 16 did not. Real boot progress cannot behave that way.

**The root cause was polarity, not the hardware.** The resting state of
both LEDs during a bootstrap boot is *dark* (the orange LED is lit while
the recovery menu flashes, then goes out when the device reboots into the
bootstrap kernel). An OFF marker therefore drives the LED to the state it
was already in and **cannot produce an observable event**, no matter
whether the code ran. Thirteen flash cycles on the last spare board
produced exactly zero bits of information for that reason alone. The three
ON markers, by contrast, were genuine positives.

## Why the LEDs lie

**Orange / charge LED (GPIO13).** It is not a plain GPIO output. Per
hands-on hardware experience (and consistent with the hardware-gating
comment in `modules/mach-pxa/corgi_pm_patched.c`), the board itself owns the
charging logic: GPIO13 acts closer to an *enable* — it tells the Zaurus
"you may now light or extinguish the LED depending on the charger
situation" — and the hardware then decides based on real adapter
presence. Consequences:

- Turning it **on** and turning it **off** are not symmetric operations:
  ON is reliable, OFF is not. Only use dark->lit as a signal.
- AC must be connected for it to be visible at all — but note that AC
  alone does *not* light it during a bootstrap boot (observed: dark), so
  a dark->lit transition really is the kernel's doing.
- The stage-2 kernel used to separately check `AC_IN` presence (the
  `sharpsl-charge` LED trigger, fed by ACIN polling in `sharpsl_pm.c`) —
  which was **redundant**, since the hardware already gates on real
  charger presence. **Simplified 2026-07-31:** GPIO13 is now driven high
  once at boot (`LEDS_GPIO_DEFSTATE_ON` + both `retain_state` flags, no
  trigger) and never touched again, which is what Sharp's own kernels
  did. Nothing in the kernel derives this LED's state anymore.

**Green LED (SCOOP PA11).** Driving it ON from the bootstrap kernel
works reliably; driving it back OFF does not — the same asymmetry as the
orange LED. Separately, `modules/mach-pxa/corgi_patched.c`'s `corgi_poweroff()` /
`corgi_restart()` carry comments claiming green-off/green-on signal
halt/reboot to the bootloader; **this has never been observed on the
stock ROM.** Treat those comments as unverified folklore until someone
reproduces the behaviour.

Both LED behaviours deserve dedicated research when work returns to the
stage-2 kernel. They are currently the *only* pre-console debugging
channel this project has (no serial cable, no USB — see `AGENTS.md`),
which makes getting their semantics right infrastructure, not trivia.

## A second, independent bug in the same investigation

Six of the OFF markers (checkpoints 4/6/8/10/12/14) poked SCOOP at
physical `0x10800000` from C code running **after** `__enable_mmu`. Only
`0x40E00000` (the PXA GPIO bank) was ever added as a diagnostic
identity-mapped section in `__create_page_tables()`; SCOOP never was. So
those markers were dereferencing an unmapped address and would fault on
their own memory access — very possibly *causing* a hang at the exact
point they were meant to be observing, and in any case producing no
signal about real boot progress.

Note also that `paging_init()` calls `prepare_page_table()` as its first
step, which wipes *every* low virtual-address mapping. Any diagnostic
identity mapping must be explicitly preserved there (see
`modules/arch-arm/mmu_patched.c`) or every marker placed after that point is a guaranteed
false negative regardless of boot state.

## Lessons

1. **Validate the instrument before bisecting with it.** A control build
   that drives the signal to a *known* state at a point already believed
   reached costs one flash cycle and would have caught this immediately.
2. **A static LED state is not an event.** "LED is lit" conflates "our
   code ran" with "it was already lit". Prefer an unambiguous state
   *change*, a blink, or a signal the hardware cannot assert on its own.
3. **Watch for correlation with the operation instead of the location.**
   If every marker of one kind works and every marker of another kind
   doesn't, regardless of where they sit, the instrument is broken — stop
   bisecting and go fix the instrument.
4. **Know the resting state before choosing a polarity.** Signal in the
   direction that differs from baseline; here that is always dark->lit.
5. **A marker that touches unmapped memory is not a passive observer** —
   it can crash the very boot it is measuring.

## What replaced it

A blink-code scheme: each checkpoint blinks BOTH LEDs a distinct count,
so one boot reports how far execution got instead of yielding one bit per
flash cycle. It only relies on dark->lit transitions, drives both LEDs in
unison so losing one doesn't lose the run, and degrades gracefully — if
the OFF half proves unreliable, the LEDs simply latch on at the first
checkpoint reached. See `piko_blink` in `modules/arch-arm/head_patched.S` (assembly,
pre-MMU) and the C copies in `modules/arch-arm/setup_patched.c` / `modules/arch-arm/mmu_patched.c`.

## What this does NOT cast doubt on

The `mtd1` flash path itself is independently verified correct and is not
the cause of the boot failure — see `docs/DEADLETTER-MTD1-OFFSET.md` and
the FTL/`.dbk` forensics that confirmed offset, FTL mapping, physical
placement, and byte-for-byte content. Start from the kernel build, not
the flasher.
