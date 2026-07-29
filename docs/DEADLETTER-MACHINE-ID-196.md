# Dead Letter — the board's machine ID is 19, not 196 (196 is Sharp's own kernel talking about itself)

*Written 2026-07-29, corrected 2026-07-30 once the 19-vs-196 distinction
was actually nailed down on real hardware. Read this before debugging any
"flashes fine, verifies byte-for-byte, never boots" symptom on mtd1.*

**tl;dr — two numbers, do not conflate them:**

| number | what it is | where it's used |
|---|---|---|
| **19** | what the bootloader *actually passes in r1* at boot, read directly off LED blink-code digit readouts on real hardware | the number a mainline kernel must match to boot this board |
| **196** | what Sharp's own factory kernel *calls itself internally* ("SHARP Shepherd"), found by decompiling that kernel out of a NAND dump | forensic/historical context only — was mistakenly treated as sufficient to boot mainline; it is not |

The rest of this document is written the way it was originally investigated
(196 first, then 19), because that history matters for the lessons at the
bottom — but if you only need the answer for "what machine number does a
mainline kernel need to match," it's **19**, registered as
`MACH_TYPE_SHARP_BOOTLOADER` in `modules/mach-pxa/corgi_patched.c`.

---

## Summary

A freshly built `mtd1` bootstrap kernel flashed perfectly, verified
byte-for-byte, landed on exactly the right FTL blocks — and never booted.
No console, no LED, nothing. An entire session went into it.

The first-found cause was that Sharp's own kernel declares this machine as
**nr = 196**, which mainline Linux never registered — so `setup_machine_tags()`
matched nothing, and `dump_machine_table()` ended in its bare `while (true);`,
a completely silent, console-less infinite loop indistinguishable from a bad
flash. Registering 196 as a new `MACHINE_START` was necessary, but — as a
*later* session (2026-07-30) discovered — **not sufficient**: it fixed the
false assumption that mainline had no matching descriptor at all, but it
did not fix the false assumption about *which number the bootloader passes*.
Reading LED blink-code digits directly off real hardware showed the
bootloader passes **19** in r1, not 196. 196 is only what Sharp's *own*
kernel calls itself in its one-and-only machine descriptor — a kernel that,
having exactly one descriptor, has no reason to ever validate r1 against it.
19 is upstream `MACH_TYPE_L7200` (an unrelated LinkUp SDP board) and is
almost certainly not a deliberately assigned ID for this hardware at all;
it's just whatever the bootloader happens to leave in r1. Both numbers are
now registered in `modules/mach-pxa/corgi_patched.c` (`MACH_TYPE_SHARP_BOOTLOADER = 19` and
`MACH_TYPE_SHARP_LEGACY = 196`), but **19 is the one that actually matches
at boot time on this board.**

**No mainline kernel this project had built before either fix could have
booted this board**, regardless of config, offset, or compression.

## The evidence

Decompressing the Cacko/Sharp kernel straight out of the board's own NAND
(`SYSTC760.DBK` → gzip stream in `smf`) yields:

```
Linux version 2.4.18-rmk7-pxa3-embedix-021129 (zaurus@sharplinux)
  (gcc version 2.95.2 19991024) #1 Thu, 6 Nov 2003 09:29:23 +0900
```

It contains exactly **one** Sharp machine descriptor — no Corgi, no Husky:

| field | value |
|---|---|
| `name` | `SHARP Shepherd` |
| `nr` | **196** |
| `phys_ram` | `0xa0000000` |
| `phys_io` | `0x40000000` |
| `io_pg_offst` | `0x00003e00` |

`phys_ram`/`phys_io` confirm this is the right struct and a genuine PXA
entry, not a false positive.

Mainline `arch/arm/tools/mach-types` knows only:

| machine | nr |
|---|---|
| corgi | 423 |
| poodle | 424 |
| tosa | 520 |
| husky | 543 |
| shepherd | 545 |

**196 appears nowhere.** It is a pre-registration Sharp/Embedix number that
never made it into the ARM machine registry. The name collision is a trap:
mainline's "shepherd" (545) is *not* the same thing as the "SHARP Shepherd"
string in Sharp's kernel, which is machine 196.

## Independent confirmation

`piko-install` was taught to dump `/proc/cpuinfo` (it runs under Cacko's
recovery kernel, started by the *same* bootloader):

```
Hardware	: SHARP Shepherd
```

Same answer, from a completely separate path.

## The fix

`modules/mach-pxa/corgi_patched.c` gains two machine descriptors, not one:

```c
/* What the bootloader ACTUALLY passes in r1 — read off LED blink-code
 * digits on real hardware, stable across power cycles. Upstream this
 * number belongs to MACH_TYPE_L7200 (unrelated hardware); deliberately
 * NOT spelled that way since this board is not an L7200. */
#define MACH_TYPE_SHARP_BOOTLOADER	19

MACHINE_START(SHARP_BOOTLOADER, "SHARP Zaurus (bootloader nr 19)")
	.fixup		= fixup_corgi,
	.map_io		= pxa25x_map_io,
	...
MACHINE_END

/* What Sharp's OWN kernel calls itself internally. Not what the
 * bootloader passes, but registered anyway as forensic/fallback context. */
#define MACH_TYPE_SHARP_LEGACY	196

MACHINE_START(SHARP_LEGACY, "SHARP Shepherd (Sharp legacy nr 196)")
	.fixup		= fixup_corgi,
	.map_io		= pxa25x_map_io,
	...
MACHINE_END
```

Hardware-wise this is the same board as Corgi/Shepherd/Husky for both
descriptors, and `machine_is_corgi()` is false for both 19 and 196, so
either takes the identical non-Corgi path. **19 is the one that actually
matches what the bootloader passes and boots the board; 196 is kept
registered mainly so a match against Sharp's own internal number is no
longer a silent hang either, and for anyone cross-referencing Sharp's
kernel output later.**

A belt-and-braces fallback also stays in `modules/arch-arm/atags_parse_patched.c`: any
unmatched machine number is blinked out in decimal on the LEDs and boot
continues on a known-good descriptor, rather than vanishing into
`dump_machine_table()`. **That failure mode must never again be silent.**
This is exactly the mechanism that caught 19 in the first place, once 196
alone turned out not to be enough.

## Why it took so long, and what to do differently

1. **The symptom pointed at the flasher.** A byte-perfect flash that
   doesn't boot looks like a flashing bug. It wasn't — see
   `docs/DEADLETTER-MTD1-OFFSET.md` and the FTL/`.dbk` forensics that
   independently verified offset, FTL mapping, physical placement, and
   content. This groundwork held up throughout and was never revisited as
   a suspect, including in the follow-up session that found 19.
2. **`dump_machine_table()` hangs silently.** With no serial and no
   framebuffer console (`AGENTS.md` hard constraints), a machine-ID
   mismatch produces *zero* observable output. Treat "silent forever" as a
   strong hint toward this failure class.
3. **`AGENTS.md` used to assert this device is Husky (543).** That came
   from the *original* SL-C860, which was bricked and swapped
   (`docs/DEADLETTER-MTD2-MTD3.md` Part 3). The current board is neither
   Husky nor (as first assumed) 196 — it's 19 at the bootloader level, with
   196 as a related-but-different internal Sharp number. `AGENTS.md` now
   states this correctly; if you see "Husky" describing this board's
   machine ID anywhere else, it's stale.
4. **Read the vendor kernel earlier.** Decompressing Sharp's own kernel out
   of a NAND dump and reading its machine descriptor took minutes and gave
   a real, verifiable number (196) — a genuinely useful step. The mistake
   was stopping there and assuming that number was also what the
   bootloader passes, instead of independently confirming r1's actual
   contents on real hardware. Vendor-kernel forensics and bootloader
   behavior are two different questions; both are worth answering, but
   don't substitute one for the other.

Related: `docs/DEADLETTER-LED-MARKERS.md` (how the LED instrumentation that
found both this and the later 19 correction was itself broken for 13 flash
cycles first) and `docs/DEADLETTER-BOOTSTRAP-BOOTS-2026-07-30.md` (the
session that found 19 and got the board to boot for the first time).
