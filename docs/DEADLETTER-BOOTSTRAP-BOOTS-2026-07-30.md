# Dead Letter — the bootstrap kernel boots on real hardware, first time ever

*Written 2026-07-30, the night it finally happened. Read this before touching
`clk_pxa_cken_init()`, the sharpsl NAND driver, the bootstrap's initramfs, or
any "byte-perfect flash, never boots" symptom on `mtd1`/`smf`.*

---

## Summary

After weeks of a bootstrap kernel that flashed perfectly, verified
byte-for-byte, landed on exactly the right FTL blocks, and never booted —
it booted. On real hardware, for the first time this project has ever
achieved. This doc is the full chain of fixes that got there, plus the
honest story of how the debugging actually went, including two real
missteps along the way (one small, one large).

The short version: a clock-registration hang was bisected and worked
around (root cause still unknown — flagged below, not resolved); a
framebuffer driver that had *never* been buildable in this tree was made
buildable; a missing static NAND partition table was supplied; a real,
long-standing NAND ECC driver gap was found and fixed at the source after
an early instinct to just silence its symptom was correctly overruled; and
— the big one — an entire detour was spent treating this bootstrap kernel
as if it should mount its own persistent root filesystem, when the actual,
already-documented architecture is an embedded initramfs that runs
entirely in RAM and never roots anything permanently. Once that was
corrected, the bootstrap kernel booted cleanly.

None of this touches or reopens the flasher/FTL/offset work from prior
sessions — that groundwork (`docs/DEADLETTER-MTD1-OFFSET.md`) held for the
whole night and was never a suspect. The FTL layer itself was tested twice
more tonight and worked correctly both times (see step 4 below) — it has
now been cleared as a suspect for good.

---

## 1. Where the day started

Mid-bisection of a boot hang somewhere inside `pxa_timer_init()` /
`pxa25x_clocks_init()`, continuing a multi-day "flashes perfectly,
byte-verified, never boots" investigation. Prior sessions had already
independently confirmed the flasher, the FTL layer, and the `mtd1` offset
(917504) were correct (`docs/DEADLETTER-MTD1-OFFSET.md`,
`docs/DEADLETTER-MACHINE-ID-196.md`) — that work was never revisited as a
suspect tonight, and it kept holding.

## 2. The clock hang — bisected, worked around, root cause still open

Using the blink-code checkpoint scheme from `docs/DEADLETTER-LED-MARKERS.md`
(each checkpoint blinks both LEDs a distinct count so one boot reports how
far execution got), the hang was bisected down to `clk_pxa_cken_init()` in
`modules/clk-pxa/clk_pxa_patched.c`: registering either the LCD clock (`pxa2xx-fb`) or the
static-memory-controller clock (`pxa2xx-pcmcia`, i.e. MEMC) hangs the board
solid. Telling the two apart by LED index alone (15 vs 16) proved
unreliable by eye — see the two stacked comment blocks in
`modules/clk-pxa/clk_pxa_patched.c` right above the fix, left in place on purpose as a
record of the ambiguity.

**The fix skips registering both clocks entirely.** This is safe, not
reckless: *not* registering a clock with the common clock framework does
not disable it — the hardware CKEN enable bit keeps whatever the
bootloader already programmed. The bootstrap kernel drives no display and
needs no runtime control over the memory controller, so it simply never
hands either clock to the framework.

**Flag explicitly: why registering one of these two specific clocks hangs
the board was never established.** This is a targeted unblock, not a
root-cause fix, and the comments in `modules/clk-pxa/clk_pxa_patched.c` say so. It's a real
open question for whoever next has cause to touch clock registration on
this board — don't mistake the workaround for an explanation.

## 3. Getting a real console — CONFIG_FB_W100 never existed

With the clock hang out of the way, the next problem was no display output
at all, on any build, ever. The cause turned out to predate every other bug
being chased tonight: `CONFIG_FB_W100` had never actually existed as a
buildable Kconfig symbol in this tree. `tools/setup-kernel-src.sh` was
copying the W100 framebuffer driver source (`w100fb_patched.c` and
friends) into the kernel tree correctly, but the matching `Kconfig`/
`Makefile` stanzas that make `FB_W100` *selectable* were never added — so
`w100fb.c` was present but never compiled into any kernel this project had
built, independent of every other bug. Fixed by appending the missing
Kconfig/Makefile stanzas in `tools/setup-kernel-src.sh` (search that file
for `FB_W100` — the comment there explains the gap in the same terms as
here).

This alone would have kept the screen dark forever regardless of the clock
fix, the partition table, the ECC bug, or the initramfs architecture — it's
worth remembering as its own independent lesson: a completely dark screen
had at least two unrelated causes stacked on top of each other, and fixing
one doesn't rule out the other still being there.

## 4. Finding the partition table — and clearing the FTL for good

First real console output showed `CONFIG_MTD_SHARPSL_PARTS` had never been
enabled either, so the entire 128 MiB NAND chip appeared as one single,
undivided block device. Enabling it let `sharpslpart`'s on-flash FTL
directory scan run — and **it succeeded cleanly on the first try**,
reporting "Sharp SL FTL: 448 blocks used (424 logical, 24 reserved)",
matching this exact board. This is the second and third time now (after
the `mtd1` offset work) that the NAND/FTL layer has been tested and found
correct. **State this plainly: the FTL/logical-block layer has never been
the cause of any symptom in this project's history, on any occasion it has
actually been tested. It is closed as a suspect.**

What *did* fail was reading the on-flash partition-info record itself —
both redundant copies (logical offsets `0x60000` and `0x64000`, inside the
same FTL-managed area the successful scan just read) came back unreadable.
Root cause not established — possibly genuine flash wear from this
project's many kernel-flash cycles on this board, possibly something else.
Worked around with a static fallback partition table
(`sharpsl_nand_fallback_parts[]` in `modules/mach-pxa/corgi_patched.c`), built from two
independently-confirmed sources, not a guess: this board's own Cacko
`/proc/mtd` dump, and `sharpslpart.c`'s own in-source reference comment
documenting an identical SL-C860's layout. `mtd_device_parse_register()`
only falls back to this table if every real parser fails first, so a
working on-flash table still takes priority — this is a safety net, not a
replacement.

## 5. The wrong-partition detour

`root=` was initially guessed at `/dev/mtdblock2` by analogy with the
Cacko/recovery-menu kernel's own partition numbering. That guess was
wrong, and was caught by direct size-math against `/proc/partitions` on
real hardware: `mtdblock1` is actually "root", `mtdblock2` is "home" under
mainline. This also surfaced, concretely, the numbering trap now
documented in `AGENTS.md`: the Cacko/recovery-menu kernel (what
`piko-install` and every flashing doc's `mtd1`/`mtd3` language refers to)
sees `mtd1=smf / mtd2=root / mtd3=home`, while mainline sees `mtd0=smf /
mtd1=root / mtd2=home` — one off, because a NOR "Filesystem" physmap
device is defined in `modules/mach-pxa/corgi_patched.c` but never actually registered in
`corgi_devices[]`, so mainline's own NAND partitions start counting at
`mtd0` instead of `mtd1`. Any doc or command stating a bare `mtdN` without
saying which kernel it means is a latent trap.

Separately, `CONFIG_JFFS2_FS_WRITEBUFFER` was missing entirely from the
config — without it, JFFS2 refuses to mount any NAND-backed MTD device at
all, regardless of which partition is named. Both were fixed. (Note: this
whole step was operating inside the wrong overall mental model, corrected
in step 7 below — the partition-numbering trap it surfaced is still real
and worth keeping, but "mount home as root" itself was not the right goal
for the bootstrap kernel.)

## 6. The ECC-error wall — and the correction that came from the user, not the assistant

With a partition table and JFFS2 writebuffer support in place, JFFS2's
scan hit `mtd->read(...) returned ECC error`, printed for essentially
every block, 100% failure rate.

**The first instinct was wrong, and worth stating plainly.** The initial
response was to rate-limit the *printing* of the message, treating it as
cosmetic — technically true that JFFS2 already handles this error class
non-fatally by design, but that framing dodged the actual question of why
a 100%-failure-rate symptom existed at all. The user pushed back
explicitly: muting a systemic, universal failure without understanding it
was the wrong instinct, because it would keep lingering and cause real
problems later ("the sooner we get it fixed the better"). That pushback
was correct.

The actual root cause: `sharpsl_attach_chip()` in
`modules/nand/sharpsl_nand_patched.c` never set
`chip->ecc.options |= NAND_ECC_GENERIC_ERASED_CHECK`. Without that flag,
`nand_read_page_hwecc()` never special-cases a genuinely blank (all-`0xFF`)
page, so every erased page on the chip reads back as an uncorrectable ECC
error instead of being recognized as blank. This is a well-known, standard
gap — several other in-tree NAND drivers (`diskonchip`, `davinci_nand`,
`stm32_fmc2_nand`, and `nand_base.c`'s own default) set this exact flag for
this exact reason; it was simply missing here. Fixed at the source (see
the dated comment block in `sharpsl_nand_patched.c`, right above the
`chip->ecc.options |=` line).

**The lesson, named explicitly:** the fingerprint that should have given
this away immediately was that the failures were 100% and perfectly
block-aligned-regular. Real flash wear is scattered and probabilistic,
never universal and never that regular — that pattern was itself the first
clue, and should have prompted "what read path treats every blank page as
an error" rather than "how do I make this stop printing."

## 7. The big one — an entire wrong mental model

Everything in steps 5–6 was built on an assumption that was never actually
checked: that the bootstrap kernel should behave like a miniature stage-2
kernel, mounting its own persistent `root=` filesystem and running as a
normal init. This was architecturally wrong, and it's the largest
misstep of the night.

The user broke the guessing cycle directly: *"let's stop guessing, we have
the working DBK image of a working bootstrap kernel... let's figure out
its actual config."* That led to consulting a handoff document from
another agent (on the machine that had originally built working
bootstraps), pasted into the session, which corrected the model
completely: **the bootstrap kernel has an embedded initramfs that runs
entirely in RAM.** Its `/init` mounts `home` temporarily and read-write
purely to fetch the real stage-2 kernel image and a `kexec` binary, then
jumps into stage 2. It never persistently roots anything. The
`root=/dev/ram0 ro` sitting in an old committed kernel config was
vestigial — silently ignored the instant an initramfs with a working
`/init` exists, exactly as the kernel's own boot-time rule for "initramfs
present → `root=` ignored" says it should be.

**State plainly: this correction was already available, in writing, in
this project's own docs, well before it was actually consulted.**
`docs/HOWTO-BUILD-DEPLOY-KERNEL.md` already describes the two-stage boot
correctly (bootstrap vs. stage-2, and that stage-2's rootfs lives on
`home`, updated over SSH — not something the bootstrap itself persistently
mounts). More pointedly, `docs/archive/DEADLETTER-STAGE2-INIT.md`, from an
earlier 2026-07-22 session, spells out almost exactly this same
initramfs-vs-`root=` mutual-exclusion lesson *for the stage-2 kernel*: "An
embedded initramfs and `root=` are mutually exclusive boot paths: the
initramfs `/init` wins and `root=` is ignored. ... Don't half-configure
both." That lesson generalizes directly to the bootstrap, and reading it
again before reverse-engineering an architecture from partition names and
trial and error would have skipped steps 5 and most of 6 entirely.

**The standing lesson: check what's already documented about the intended
architecture before reverse-engineering one from symptoms.** This project
has a large `docs/` tree specifically so this doesn't have to happen twice;
it happened anyway because the existing docs weren't consulted early
enough.

## 8. Getting the real initramfs

The actual initramfs build artifacts — busybox 1.36.1 source and build,
an unpacked rootfs tree, and a packed `cpio.gz` — did not exist on this
machine at all once the correct architecture was understood. They were
retrieved as a zip from the user's own Downloads folder (from wherever the
working bootstraps were originally built) and extracted into
`initramfs/`. A separate, parallel effort is turning this into properly
tracked, reproducible build tooling instead of a vendored zip; as of
tonight neither `tools/build-initramfs.sh` nor `modules/initramfs/` exist
yet in this tree, so that work had not landed as this doc was written —
worth checking for on a future pass, but it did not block tonight's boot.

## 9. Success

Rebuilt with the corrected architecture: a real embedded initramfs
(`CONFIG_INITRAMFS_SOURCE` pointed at the actual `initramfs-minimal-v2.cpio.gz`,
previously empty in the committed minimal config — a real regression the
handoff doc explicitly flagged) plus `CONFIG_BINFMT_SCRIPT=y` (also
previously unset, needed since `/init` is a `#!/bin/sh` script and would
otherwise fail to execute at all). Several drivers not needed before kexec
were trimmed to fit the tight 1,294,336-byte `smf` flash budget: PCMCIA,
generic PC keyboard/mouse input, I2C, and SquashFS/FAT filesystem support.

**The bootstrap kernel booted successfully on real hardware for the first
time this project has ever achieved.**

## 10. What's next

The bootstrap doesn't yet reach a working stage 2 — but for an
already-understood, separate reason, not a new mystery: `home` currently
holds an older `zImage-full` that was flashed with a config lacking
framebuffer support, so even a fully successful kexec into it would be
headless. The next step, being worked on live and not part of this doc, is
building a corrected stage-2 kernel with W100/framebuffer support and
reflashing `home` — which will also serve as a second proof that
`piko-install` can flash both `smf` and `home` together in a single pass,
a capability already demonstrated once earlier in this project
(`docs/FLASH-MTD1-MTD3-SAFE.md`).

## Closing note

In the user's own words and spirit, worth keeping close to verbatim: the
eventual direction is evolving the bootstrap into something closer to a
real bootloader — in the spirit of GRUB — rather than a single-purpose
kexec shim. Tonight reconfirmed, for the second and third time, that
NAND/FTL fundamentals were never the actual problem in this project's
history. And `piko-install`, this project's own home-grown flashing tool,
has proven itself close to matching Sharp's own factory flasher in
capability and reliability.

---

Related: `docs/DEADLETTER-LED-MARKERS.md` (the blink-code instrumentation
this session's bisection depended on), `docs/DEADLETTER-MACHINE-ID-196.md`
(the 19-vs-196 machine ID finding this session built on top of),
`docs/archive/DEADLETTER-STAGE2-INIT.md` (the earlier, already-correct
initramfs-vs-`root=` lesson that step 7's detour cost time by not
rereading sooner), `docs/HOWTO-BUILD-DEPLOY-KERNEL.md` (stage-2 deploy
path, unaffected by tonight's bootstrap-specific work).
