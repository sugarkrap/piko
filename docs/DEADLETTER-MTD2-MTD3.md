# Dead Letter — How We Bricked an SL-C860 Flashing mtd2/mtd3

*Written 2026-07-22. Read this before ever writing to mtd1/mtd2/mtd3 again.*

---

## Summary

While chasing full networking/SSH support, we went from "raw NAND writes don't
work" to "found the real tool Cacko uses" to "let's use the whole partition,
there's plenty of room" to **a fully bricked SL-C860** (no LED response on
cold boot *or* recovery boot) in the space of one session. The device was
swapped for a spare SL-C760 (confirmed hardware-identical, see below) and
we are now flashing under a much stricter policy. This file exists so nobody
repeats the mistake on the replacement board.

---

## Part 1 — Getting mtd2/mtd3 writes working at all

### The wrong turn: raw MTD ioctls

Sharp's proprietary "logical address" NAND layer (`nandlogical`'s
`MEMWRITELADDR`/`MEMREADLADDR` ioctls) is only wired up for `mtd1`/"smf" in
Cacko's recovery kernel — it returns `-EINVAL` on `mtd2`/`mtd3` at every
offset tried. The first fix attempt was to bypass it entirely: open
`/dev/mtd2` directly, `MEMERASE` ioctl per block, then raw `write()`.

This looked like it worked — `write()` returned full success every time —
but verification consistently found the data corrupted past the *first*
eraseblock of any multi-block write, regardless of chunk size tried (16 KiB,
256 KiB, and finally genuine 512-byte NAND-page-granular writes with
individual `lseek()` per page). `mtd3`'s apparent success turned out to be a
false positive: its content is almost entirely `0xFF` padding already, so a
silently-failed write was indistinguishable from a successful one there.

**Root cause was never fully nailed down.** Working hypothesis: this
recovery kernel's MTD char-device `write()` may only honor the first
`write()` call on an fd and silently no-op the rest — untested, because a
better path turned up first.

### The right turn: read the actual Cacko installer

Told to "get closer to how Cacko installs," we read the genuine, previously
decoded `updater.sh` (Cacko's real ROM installer) in full. It revealed the
real mechanism for `ISLOGICAL=0` targets (rootfs/home, as opposed to the
kernel which uses `ISLOGICAL=1` via `nandlogical`):

1. **`/sbin/eraseall $TARGET_MTD`** — bulk-erase the *entire* partition once,
   up front. Not a per-chunk `MEMERASE` loop.
2. **`/sbin/nandcp -a $ADDR $CHUNK $TARGET_MTD`** — a completely different
   tool from both `nandlogical` and raw `write()`, invoked per 1 MiB chunk.
   Its stdout reports a line containing `"mtd address START-END(...)"`;
   `updater.sh` parses `END` out with
   `fgrep "mtd address" | cut -d- -f2 | cut -d'(' -f1` and feeds it in as the
   **next** chunk's `-a` address, rather than computing offsets itself —
   meaning `nandcp` handles bad-block-aware address translation internally
   and the caller just follows whatever it reports back.

We rewrote `piko-install-final.c`'s `flash_one_raw()` to shell out to these
two real tools (fork/exec, same pattern as the existing `nandlogical`
wrapper) instead of reinventing NAND writes ourselves. First real-hardware
test looked promising: `eraseall` hit a known bad block on `mtd3`
(`MTD Erase failure: Input/output error` at ~0x22a8000, matching a
previously-found bad block) and correctly skipped past it to 100% complete;
`nandcp` wrote chunks at exactly the expected addresses with `bad block: 0`.

### The verify-buffer bug (found, fixed, never actually retested)

The one visible failure — `VERIFY FAILED for mtd2.jffs2`, mismatch at file
offset 16384 — turned out to be **our own bug**, not a write problem: the
verify readback buffer (`verifybuf`, sized `CHUNK_SIZE` = 524288 bytes, an
old buffer shared with the `nandlogical` path) was being read into with up
to `NANDCP_CHUNK` = 1,048,576 bytes — a straight buffer overflow corrupting
adjacent memory. Fixed by giving the raw verify path its own
correctly-sized `verify_raw_buf[NANDCP_CHUNK]`. **This fix was built but
never actually re-verified on hardware** — attention moved to the kexec
architecture question before a clean write+verify pass was ever confirmed
end-to-end.

---

## Part 2 — Dropping two-stage kexec, and the size budget mistake

The two-stage kexec design (tiny bootstrap kernel in `mtd1`, kexec-ing into a
full-featured kernel + rootfs stored in `mtd2`) was abandoned mid-session —
"let's drop the two phase anyway, since we have more memory than we need" —
in favor of a single kernel with full NET/USB-gadget/PCMCIA/HOSTAP support
flashed directly into `mtd1`.

This is the decision that caused the brick.

`flash/kernel-flash.sh` (Cacko's own installer) caps the kernel write at
`MTD_PART_SIZE=1294336` bytes (~1.26 MB), starting at offset 917504 within
`mtd1`/"smf". `mtd1`'s total physical size is 7,340,032 bytes (0x700000),
which left what looked like ~6.4 MB of headroom past Cacko's own budget. We
treated this as "just Cacko's own historical kernel size, not a
hardware/bootloader ceiling" and raised `piko-install-final.c`'s own size
check to 6,422,528 bytes to fit a 1.77 MB full-featured kernel — a write
**~4x larger** than anything Cacko's own installer had ever written at that
logical range.

The kernel flashed via `nandlogical` with no errors reported. The device
never booted again — no LED response at all on cold boot, and none on the
recovery-mode button combo either. Full silence on both paths.

### Why this probably happened

Our own QEMU boot log for this exact device family shows:

```
Sharp SL FTL: 448 blocks used (424 logical, 24 reserved)
```

Only **24 reserved/spare blocks** of wear-leveling headroom in the flash
translation layer Sharp's NAND driver uses for `smf`. A write several times
larger than anything Cacko's own installer ever wrote at that logical
address range plausibly forced the FTL to reclaim physical blocks that were
actually holding the recovery firmware, even though the write's *logical*
offset (917504 onward) looked safely clear of it.

### Confirmed with real evidence (2026-07-22, offline analysis of `smf-backup.bin`)

Once a full `smf` backup existed from the *replacement* board (see
`flash/smf-backup.bin`), it became possible to actually check this theory
against real data instead of just reasoning about the FTL abstractly.

Scanning the 7MB `smf` partition in 4KB blocks and classifying each as
zero-filled, erased (`0xFF`), or containing real data turns up a striking
structure: **43 separate ~16KB "data" regions, spaced roughly every 128KB,
scattered across the *entire* partition** — not just around the kernel.
One such region (`0x20000`) contains genuine ARM machine code with
embedded debug strings straight out of Sharp's own driver:
`"nand_logical_init() - duplicate logical no"`,
`"nand_logical_read_block() - bad log_no"`,
`"nand_logical_read_block() - empty log_no"` — this is **Sharp's actual
`nand_logical` driver implementation** (the same logical-address
translation layer `nandlogical`/the recovery menu depend on), stored
redundantly in NAND-wear-leveling fashion rather than at one fixed
location. Other regions (e.g. `0x600000`) look like high-entropy/encrypted
data instead — likely a different structure type sharing the same layout
convention.

Computing which of these 43 regions each kernel write actually overlaps:

| Write | Range | Regions touched |
|---|---|---|
| Our safe, currently-working bootstrap kernel (1,284,872 bytes) | `0xE0000`–`0x219B08` | **9** |
| The oversized kernel that bricked the other board (1,775,640 bytes) | `0xE0000`–`0x291818` | **13** |

The oversized write touches **4 additional** regions the safe write never
reaches (`0x220000`, `0x240000`, `0x260000`, `0x280000`). This is real,
quantified evidence — not just plausible reasoning — that the oversized
write corrupted meaningfully more of this redundant driver/FTL structure
than any write that has ever worked. Most likely explanation: Sharp's
design tolerates losing some number of these redundant copies (up to
whatever the safe kernel's 9-region footprint represents) but not
14+ — the oversized write pushed past that resilience margin and left no
recoverable copy of something the boot path depends on.

**The lesson: Cacko's own conservative kernel-size budget was never just a
historical convention. Treat it as the real, hard ceiling — matching it is
the safety margin, not an inconvenience to work around.** This is no longer
just an inference from the reserved-block count; it's directly visible in
the partition's actual structure.

---

## Part 3 — Recovery attempts on the bricked SL-C860

- **QEMU testing was not diagnostic here.** QEMU's `spitz` machine (PXA270)
  never exercises the real Husky-specific code path the SL-C860 needs, so it
  can only rule out "kernel is fundamentally broken for any board," not
  confirm real-hardware boot health.
- **Boot-ROM-level factory flash menu** (SL-C750/760/860, documented in
  TRIsoft's quickstart guide): battery pull + reinsert + hold "OK" + power on
  (AC power only, no USB needed) reaches a Japanese menu with 4 options
  (Cancel / fsck / full-erase-format / Update). **This menu did appear** on
  the bricked device — meaning it's genuinely boot-ROM-resident, independent
  of whatever got corrupted in NAND. Neither option 3 (format) nor option 4
  (update, the real SD-card reflash path) recovered the device — both
  returned to the same dead state.
- **JTAG investigated as a last resort.** Confirmed via the official Intel
  PXA255 Electrical/Mechanical/Thermal Specification (256-lead 17×17mm mBGA):
  JTAG ball locations are nTRST=H11, TCK=H12, TMS=H13, TDI=H15, TDO=H16 — but
  these are BGA balls under the CPU package itself, not accessible without
  X-ray-guided rework. No schematic or teardown documenting whether Sharp
  broke these out to an accessible header/test-point cluster on this board
  was found. A candidate 5-trace cluster was photographed on the board but
  turned out on closer inspection to be more consistent with a factory
  bed-of-nails ICT (in-circuit test) pattern (~8-9 points, not 5) than a
  dedicated JTAG header. Inconclusive; not pursued further.
- **Outcome: board considered unrecoverable by any means available.** Swapped
  for a spare SL-C860's confirmed hardware twin.

---

## Part 4 — SL-C760 vs SL-C860 (why the swap is safe to treat as equivalent)

Per TRIsoft's official SL-C750/760/860 quickstart guide, the **SL-C760 and
SL-C860 are the same board** — the only documented difference is the
case/display cover color (C760: white, C860: "champagne silver"). Same
PXA255 @ 400 MHz, same 128 MB Flash / 64 MB SDRAM, same display, same
battery. This matches the earlier (separate) finding that Sharp/Cacko's own
tooling reports the SL-C860 with machine ID **Husky (543)**, identical to
the SL-C760 — they're the same hardware under different paint.

---

## Part 5 — Current policy on the replacement board

This is explicitly the **last spare board** — there is no second replacement
if this one is bricked too. Current rules, until stated otherwise:

1. **`mtd1` gets only the small, JFFS2+KEXEC-only bootstrap kernel**, budget
   capped at Cacko's original 1,294,336 bytes, no exceptions, ever. The
   single-stage full-featured kernel idea is dead — do not revisit it.
2. **`mtd2`/`mtd3` are untouched** — no write, no erase — until we've
   confirmed via read-only mount what's already on them. This is a
   different, unknown board; we don't yet know its mtd2/mtd3 state.
3. Any future write to `mtd2`/`mtd3` should reuse the `eraseall`+`nandcp`
   mechanism (Part 1 above, genuinely Cacko's own tooling) — never raw
   `MEMERASE`+`write()` again — but only after the verify-buffer fix is
   actually confirmed working end-to-end on real hardware, not just built.
4. Revisiting two-stage kexec (bootstrap kernel in `mtd1` + full kernel/rootfs
   in `mtd2`) is back on the table, specifically *because* it keeps `mtd1`
   permanently small and safe — the risk was always the oversized `mtd1`
   write, not the concept of a second-stage kernel living in `mtd2`.

> **Update:** the "isolate mtd1 and mtd3 passes with a reboot gap" rule
> this doc's Part 5 assumes was later relaxed, once a confirmed-good full
> `smf` backup existed for this board — see "Combined single-pass
> playbook" in `docs/FLASH-MTD1-MTD3-SAFE.md`. The mtd1 size/offset budget
> (rule 1) is untouched and still absolute.
