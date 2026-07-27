# Dead Letter — mtd1 kernel writes need `start_addr=917504`, not 0

*Written 2026-07-22. Read this before touching `piko-install-final.c`'s
`mtd1` target entry again.*

---

## What happened

`piko-install-final.c`'s `mtd1` target used `start_addr=0` through most of
this session, including several flashes that appeared to work fine (booted
to console, reached real `kexec -l` attempts). After a D+M NAND restore
(both via another board's raw dump and via the verified, model-correct
`SYSTC760.DBK` — see `docs/DEADLETTER-NAND-RECOVERY.md`) and a fresh Cacko
reinstall, the exact same kernel image, flashed with the exact same
(correct, `raw=0`/nandlogical) method, produced total silence: no console
output, no GPIO13 LED marker, no boot at all, cold or soft — even though
every other variable (kernel source, `.config`, initramfs content,
partition table) was checked and confirmed unchanged from the last working
state.

`flash/kernel-flash.sh` — Cacko's own genuine installer script, sitting in
the repo the whole time — writes the kernel starting at logical offset
**917504** (`ADDR=917504`), not 0. This was noticed once earlier in the
session and dismissed with the reasoning "`start_addr=0` already produced
working boots this session, so it must be fine." That reasoning was wrong.

## Why offset 0 "worked" before, and stopped working after a NAND restore

`nandlogical`'s "logical address" is not necessarily a fixed linear byte
offset into the physical partition — it plausibly depends on the current
bad-block table / wear-leveling state, i.e. it's a genuinely *logical*
address translated through whatever the FTL currently thinks the mapping
is. Every earlier "successful" test this session ran on a board with a
long, organic history of prior writes across many earlier sessions of this
project. Every test on this specific `start_addr` bug ran on a *freshly*
D+M-restored, freshly Cacko-reinstalled board — a genuinely different
FTL/bad-block state than what offset 0 had previously, coincidentally,
mapped correctly against.

The working theory: Cacko's own bootloader always looks for the kernel at
logical offset 917504, unconditionally. Writing at offset 0 may only ever
have produced a bootable result by accident — either because stale data
already at the 917504 region from a still-intact prior Cacko kernel
happened to remain valid, or because that particular board's FTL state
happened to place logical-address-0 writes somewhere overlapping what the
bootloader actually reads. Once the board's FTL state was reset by a raw
NAND restore, that coincidence stopped holding, and instead of "the wrong
place but still somehow OK," it became "the wrong place, silently."

## The fix

```c
/* CORRECT */
{ "/dev/mtd1", "zImage", 917504, 1294336, 0, 0 },
```

Confirmed working on real hardware 2026-07-22, after this exact change and
nothing else.

## Standing policy

- **`mtd1`'s `start_addr` is always `917504`.** Never 0. This matches
  `flash/kernel-flash.sh` exactly and is now confirmed correct on real
  hardware, not just inferred from a forensic hex-dump comparison against
  a different, now-dead board (which was the earlier, weaker form of this
  same evidence — see `docs/DEADLETTER-MTD2-MTD3.md` Part 2, "Our safe,
  currently-working bootstrap kernel ... `0xE0000`" — `0xE0000` ==
  `917504`, the same number, from an entirely independent source).
- **Don't trust "it worked before" as justification for leaving a
  discrepancy against Cacko's own genuine tooling unfixed.** When this
  project's own code disagrees with a genuine, literal, line-by-line-read
  Cacko script for the exact same operation, assume the genuine script is
  right and go find out why the discrepancy seemed to work, rather than
  assuming past success proves current code correct — especially on a
  board whose NAND state changes across sessions (reflashes, D+M restores,
  Cacko reinstalls all change the FTL's internal state even when the
  *content* being written looks identical).
- `max_size` (the budget check) is independent of `start_addr` — confirmed
  by reading `kernel-flash.sh`'s own check (`DATASIZE -gt MTD_PART_SIZE`,
  no offset math involved). No interaction to worry about between the two
  fields.
