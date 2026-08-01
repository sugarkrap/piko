# Dead Letter — the `raw` flag is per-partition, not a global setting

*Written 2026-07-22. Read this before ever touching `piko-install-final.c`'s
`targets[]` array again.*

---

## What happened

While adding an md5sum diagnostic to the bootstrap kernel's `/init` (to rule
out silent NAND corruption of a `kexec` binary that was crashing with
`SIGILL`), `mtd1`/"smf"'s target entry in `piko-install-final.c` was set to
`raw=1` instead of `raw=0`:

```c
/* WRONG -- routes mtd1 through flash_one_raw() */
{ "/dev/mtd1", "zImage", 0, 1294336, 1, 0 },
```

`raw=1` calls `flash_one_raw()`, which shells out to `/sbin/eraseall` +
`/sbin/nandcp` — this is genuine Cacko tooling (see
`docs/DEADLETTER-MTD2-MTD3.md` Part 1), but it is **only wired up for
`mtd2`/`mtd3`**. Sharp's proprietary `nandlogical` "logical address" layer
(`MEMWRITELADDR`/`MEMREADLADDR` ioctls) is the only thing that understands
`mtd1`/"smf" — confirmed by `flash/kernel-flash.sh`, Cacko's own genuine
installer script, which calls `/sbin/nandlogical $SMF_MTD WRITE $ADDR ...`
exclusively, never `nandcp`.

The result: `eraseall` ran across the **entire physical 7,340,032-byte smf
partition**, not just the ~1.3MB region any correct kernel write ever
touches. `nandcp` then silently no-op'd on `mtd1` (never wired up there), so
nothing meaningful got written back. The device booted into blinking LEDs
with zero console output, and — worse than any previous mistake this
session — **the normal SD-card "Update" recovery trigger stopped responding
entirely.**

## Why this was worse than a normal-budget mtd1 write gone wrong

`docs/DEADLETTER-MTD2-MTD3.md` Part 2 forensically identified ~43 repeated,
redundant data regions spread across the *whole* smf partition (not just the
kernel-sized region at the front). A correctly-budgeted kernel write only
ever overlaps ~9 of them. This session's `eraseall` mistake wiped all of
them at once — including, evidently, whatever the SD-card "Update" trigger's
own bootstrap depends on, since that stopped working too. A previous
oversized-but-still-in-partition write (the incident in
`DEADLETTER-MTD2-MTD3.md`) only reached 13 of the 43 regions and *that* was
enough to permanently brick the other board. A whole-partition erase is
categorically worse.

## The fix

```c
/* CORRECT -- routes mtd1 through the nandlogical WRITE/READ path */
{ "/dev/mtd1", "zImage", 0, 1294336, 0, 0 },
```

**Rule going forward: `mtd1` is always `raw=0`. `mtd2`/`mtd3` are always
`raw=1`. Never copy a `targets[]` entry between an `mtd1` line and an
`mtd2`/`mtd3` line without flipping this field.** Consider it worth a
one-line comment directly above the array every single time it's edited.

## How the device was recovered

The SD-card "Update" trigger (recovery menu option 4) no longer worked at
all once smf's redundant structures were wiped — confirming the hypothesis
above. What did work: the **D+M service/diagnostic menu** (documented by
TRIsoft, distinct from the normal OK-button 4-option menu used everywhere
else in this project):

1. Power down, remove all power sources.
2. Hold **D** and **M**, then restore power (battery + AC).
3. This reaches a deeper diag menu (multiple pages); page 3 has **"Flash
   RESTORE"**, which reads a complete raw 1:1 NAND image (main data + OOB,
   whole chip — `mtd0` through `mtd3` together, not partition-selective)
   from a CompactFlash card.
4. The image must be named `systcXXX.dbk` where `XXX` matches the device's
   actual model — **the menu itself validates/looks for this specific
   filename** and will refuse a mismatched one. This board is a physical
   SL-C760 (the replacement for the original SL-C860 bricked in the
   `DEADLETTER-MTD2-MTD3.md` incident — see that file's Part 4 for why
   they're confirmed hardware-identical apart from case color), so it
   needed `SYSTC760.DBK`, not `SYSTC860.DBK`, despite the two being
   otherwise interchangeable stock Cacko 1.23 images.
5. Genuine factory `.dbk` images are hosted per-model at
   <https://www.trisoft.de> (e.g. the SL-C760 one is
   `http://www.trisoft.de/download/760NAND291003.zip`, 138,543,120 bytes
   unzipped — matches the expected 128MiB-Samsung-NAND-with-16B-OOB
   geometry documented in `docs/archive/HANDOFF.md`: 128MiB × 528/512 ≈
   138,412,032 bytes, within a small header/footer's difference of the
   real file size). Confirm the model before writing anything — get it
   from the device's own recovery menu (which filename it asks for), not
   from assumption.
6. This restores `mtd0`–`mtd3` all together, back to stock factory state —
   meaning `root`/`home` also revert, wiping any in-progress work there.
   Not a problem here because `piko-backup` dumps of the current
   `root`/`home` content already existed on the SD card
   (`root-backup.bin`, `home-backup.bin`) before this incident, so nothing
   was permanently lost — but this is the trade-off of this recovery path:
   it's whole-chip, not partition-selective.

## Standing policy

- Before any flash session, re-read the `raw` field of every entry in
  `targets[]` out loud (or in a comment) and confirm it against this file's
  rule. This has now caused two separate incidents from the same class of
  mistake (partition/mechanism mismatch) — once for `mtd2`/`mtd3` (see
  `DEADLETTER-MTD2-MTD3.md` Part 1) and once for `mtd1` (this file).
- Keep a genuine, verified factory `.dbk` for this board's actual model
  (SL-C760 — `SYSTC760.DBK`) somewhere durable (not just a CF card), given
  how load-bearing it just turned out to be.

## 2026-07-24 follow-up: why in-system smf writes were corrupting cold boot

Later testing on the replacement SL-C760 confirmed the same mechanism from a
different angle: an in-system writer (`flash/piko-smf-write.c`) initially used
raw `MEMERASE` + `write()` against `/dev/mtd0` (mainline names the Sharp FTL
partition `mtd0`, not `mtd1` as in older recovery environments). That looked
successful at the character-device level, but it wiped the OOB bytes that store
Sharp's logical-to-physical block mapping, so cold boot stopped finding the SMF
kernel even though the payload data compared cleanly.

The verified fix was to stop treating SMF as plain raw NAND data. The repaired
writer now:

- scans the existing FTL map by reading the first-page OOB for each physical
   eraseblock,
- preserves or allocates physical blocks per logical block,
- erases only the target physical blocks, and
- programs data with `ioctl(MEMWRITE)` in `MTD_OPS_AUTO_OOB` mode so the free
   OOB bytes (actual bytes 8-15 on this 512B+16B device) are written together
   with the data pages.

For Sharp SL, the first page of each block must carry three redundant 16-bit
copies of the logical block number in OOB bytes 8-13, with the same even-parity
encoding that `drivers/mtd/parsers/sharpslpart.c` expects. Once that metadata
was restored, OpenEmbedded cold-booted normally again.

Operational rule: **raw `MEMERASE` + `write()` on the SMF partition is still a
brick-class bug even if the write range stays within the 1.3MB kernel budget.**
The issue is not just range; it is loss of FTL OOB metadata.
