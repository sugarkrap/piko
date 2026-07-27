# Dead Letter — the D+M service menu can restore a fully wiped NAND

*Written 2026-07-22. This corrects a previous "unrecoverable" conclusion —
read this before ever declaring a board dead.*

---

## The discovery

`docs/DEADLETTER-MTD2-MTD3.md` Part 3 investigated recovery options for the
*original* bricked SL-C860 and concluded "board considered unrecoverable by
any means available" after trying:

- The boot-ROM **OK-button menu** (battery pull + reinsert + hold "OK" +
  power on, AC only) — a 4-option menu (Cancel / fsck / full-erase-format /
  Update). Options 3 and 4 were tried; neither recovered the device.
- JTAG — ruled infeasible (BGA-package balls, no accessible header found).

**What was never tried on that board: the D+M service/diagnostic menu.**
This is a *different, deeper* menu from the OK-button one:

1. Power down, remove all power sources.
2. Hold **D** and **M** together, then restore power (battery + AC).
3. This reaches a multi-page Japanese diagnostic menu, documented by
   TRIsoft (`trisoft.de`) for the SL-C750/760/860 family. Page 3 has
   **NAND Flash Back Up** and **NAND Flash Restore**.
4. Restore reads a complete raw 1:1 NAND image (main data + spare/OOB,
   covering `mtd0` through `mtd3` as one whole-chip blob, not
   partition-selective) from a **CompactFlash** card.
5. The image file must be named `systcXXX.dbk`, where `XXX` is the
   device's actual model number — **the menu validates this filename
   itself** and refuses a mismatched one. Confirmed live: this board (a
   physical SL-C760, despite arriving as a replacement for an SL-C860 —
   see `DEADLETTER-MTD2-MTD3.md` Part 4 for why they're hardware-identical
   apart from case color) rejected `SYSTC860.DBK` and asked for
   `SYSTC760.DBK` specifically.

This was used successfully 2026-07-22 to recover this board after
`mtd1`/smf was wiped by an unrelated mistake (full incident writeup:
`docs/DEADLETTER-RAW-FLAG.md`) — the normal OK-button "Update" trigger had
stopped responding entirely at that point, exactly the same symptom the
original SL-C860 showed, and D+M recovered it anyway.

**Important workflow detail found the second time through this recovery**:
the restored image is genuine pre-Cacko factory firmware, not a
Cacko-preloaded state. After a D+M restore, Cacko itself has to be
reinstalled (via its own normal install process) before the OK-button
"Update" menu / `updater.sh` / `piko-install` flow has anything to run
against — that whole flow depends on Cacko's own recovery environment,
which the stock `.dbk` image doesn't include. Budget time for "install
Cacko, then reflash our own custom kernel" as two separate steps any time
D+M restore is used, not one.

**Confirmed by direct experience (not just caution): running our own
`updater.sh`/`piko-install` against a board that does NOT already have
Cacko installed bricks it — the OK-menu "Update" trigger stops responding
at all afterward, same symptom as the `raw`-flag incidents documented in
`docs/DEADLETTER-RAW-FLAG.md`. This makes sense in hindsight: `piko-install`
was built by reverse-engineering Cacko's own genuine `updater.sh` and
assumes Cacko's specific partition/rootfs conventions and its own bundled
`nandlogical`/`eraseall`/`nandcp` tools; stock Sharp firmware doesn't
provide the same environment.**

**However — D+M service-menu recovery survived this too, fully intact.**
This is a strong, repeated pattern now: whatever the OK-menu "Update"
trigger depends on lives (at least partly) in `mtd1`'s broader
redundant-region structure and is fragile to any mismatched write there,
while D+M appears to be a genuinely robust, independent foundation (likely
rooted in `mtd0`/"Filesystem", ~6.8 MiB, which per `nand/sharpslpart.c`
contains the actual partition table defining `mtd1`–`mtd3`'s boundaries —
see the `PARTITIONINFO1` structure documented there). **Standing rule: never
run this project's `updater.sh`/`piko-install` against a board without
Cacko already installed. Always install genuine Cacko first, via its own
official installer, before ever touching a board with this project's
tooling.**

## Where to get a genuine factory image

TRIsoft (<https://www.trisoft.de>) hosts per-model factory NAND dumps as
free downloads on each model's dedicated page:

| Model | Page | Direct file |
|---|---|---|
| SL-C760 | `en_c760howto.htm` | `download/760NAND291003.zip` → `systc760.dbk`, 138,543,120 bytes |
| SL-C860 | `en_c860howto.htm` | (same filename pattern — not yet fetched, but same source; page structure confirmed identical for C760) |
| SL-C750 | `en_c750howto.htm` | (same pattern, unverified) |

Sanity-check any downloaded `.dbk` against the known NAND geometry
(`docs/HANDOFF.md`: 128MiB Samsung part, 512B page + 16B OOB) before
trusting it: 128MiB × 528/512 ≈ 138,412,032 bytes, which should be within a
small header/footer's difference of the actual file size. The SL-C760 file
matched this exactly (138,543,120 bytes, ~131KB over the raw-geometry
estimate).

**Get the filename from the device's own D+M menu, not from assumption** —
model mixups are exactly what this menu's validation exists to catch, and
this project already has one archived example (`SYSTC860.DBK`) of an image
for the wrong model sitting around from the original board.

## Open follow-up: the original "unrecoverable" SL-C860 may not be dead

Since D+M was never tried on the original bricked SL-C860 before it was
declared unrecoverable and swapped out (`DEADLETTER-MTD2-MTD3.md` Part 3),
and a `SYSTC860.DBK` already exists on the project's SD card (same
138,543,120-byte size and 1 janv. 2003 date pattern as the confirmed-genuine
SL-C760 download, strongly suggesting it's also a real TRIsoft factory
image rather than something dumped from the actual bricked unit — its exact
provenance wasn't tracked, so treat it as "probably genuine, not yet
independently re-verified against a fresh TRIsoft download") — **if that
original board is still physically available, it is worth trying D+M +
NAND Flash Restore on it before treating it as permanently dead.** This whole-chip restore only failed on the *current*
board's incident because that board was never fully bricked at the
boot-ROM level to begin with; there is no evidence D+M restore has actually
been tried and failed on hardware this badly damaged. Whether it can
recover a board where the OK-menu Update trigger doesn't respond either
(the original SL-C860's exact symptom) is now directly testable, since
that's precisely the state this current board was just recovered from.

## Standing policy

- **Before declaring any board in this project unrecoverable, D+M + NAND
  Flash Restore must be tried**, not just the OK-button menu. They are
  different code paths with different scope (whole-chip vs
  partition/userspace-triggered).
- Keep verified, model-correct `.dbk` factory images for every board model
  this project has ever touched (currently: SL-C760 confirmed-working,
  SL-C860 downloaded but unverified against real hardware) somewhere
  durable, not just on a shared SD card that gets overwritten between
  sessions.
