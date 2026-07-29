# Safe flash playbook: mtd1 then mtd3 (via piko installer)

> **When to use this doc:** only for recovery (device unreachable over
> SSH / unbootable) or when the *bootstrap* partition (`mtd1`/`smf`)
> itself needs to change. For routine stage-2 kernel/module updates on a
> working, WiFi-reachable device, use `tools/build-and-deploy.sh` instead
> — see `docs/HOWTO-BUILD-DEPLOY-KERNEL.md`. That path writes directly to
> the live `home` (`mtd3`) filesystem over SSH; no SD card or NAND
> flash involved.

This procedure is for the last spare board. It follows the dead-letter constraints:
- mtd1 uses nandlogical (`raw=0`) at offset `917504` only.
- mtd3 uses eraseall+nandcp (`raw=1`).
- Historically: do not flash mtd1 and mtd3 in one combined installer run
  (see "Combined single-pass playbook" below for when this is now allowed).

## Combined single-pass playbook (requires a confirmed backup)

`piko-install` has always been able to flash multiple `piko.cfg` targets in
one run (`flash/src/piko-install.c`'s `main()` loops over every `target`
line), but this doc previously said not to use that for mtd1+mtd3 together
— the separate-pass procedure below exists specifically to get a
reboot-and-verify checkpoint after mtd1, before mtd3 (which starts with a
full `eraseall`) is touched. If mtd1's write is bad, that checkpoint is
what catches it before mtd3 is gone too.

That checkpoint is a mitigation for *not having a good backup*, not a
requirement in its own right. It's safe to skip it when both of these hold:

1. A confirmed-good full NAND/`smf` backup exists (see
   `flash/piko-backup.c` / `docs/HOWTO-OFFLINE-UPDATE.md`) that can restore
   this board if the combined run leaves it unbootable.
2. You accept flashing mtd1 and mtd3 back-to-back with no boot check in
   between — if mtd1 silently produces a bad kernel, mtd3 still gets
   erased+written before anyone finds out.

Given both, use `flash/piko.cfg.mtd1-mtd3-combined` as `piko.cfg`:

```sh
SD=/path/to/sd
cp piko-install zImage mtd3.jffs2 "$SD"/
cp flash/piko.cfg.mtd1-mtd3-combined "$SD"/piko.cfg
cp updater-uncoded.sh "$SD"/updater.sh   # or your encoded updater, per HOWTO-OFFLINE-UPDATE.md
sync
md5sum piko-install zImage mtd3.jffs2 flash/piko.cfg.mtd1-mtd3-combined "$SD"/piko-install "$SD"/zImage "$SD"/mtd3.jffs2 "$SD"/piko.cfg
```

`piko-install` flashes targets in file order and **aborts on the first
target failure** — it will not erase/write mtd3 if the mtd1 pass fails.
Order in the config (mtd1 first) matters for this to be useful; don't
reverse it.

After the run: read `piko-log.txt` off the SD card and confirm
`ALL TARGETS FLASHED AND VERIFIED` before rebooting. If it reports a
failure, do not power-cycle into normal boot — restore from the backup
first.

Without a confirmed backup, fall back to the separate-pass procedure below.

## Files used (profile-locked, preferred)
- mtd1 pass: `piko-install-mtd1-safe` + `zImage`
- mtd3 pass: `piko-install-mtd3-safe` + `mtd3.jffs2`
- updater wrappers:
  - `updater-mtd1-safe.sh` (runs mtd1 installer)
  - `updater-mtd3-safe.sh` (runs mtd3 installer)

Do not use `piko-install`/`piko-install-final` here. In this workspace state,
those names are ambiguous because they were found to be byte-identical.

(This restriction is about the separate-pass procedure below, where which
compile-time default target a plain `piko-install` binary was built with
matters. It doesn't apply to the combined single-pass playbook above, which
always ships an explicit `piko.cfg` — the compile-time default is never
consulted, so a single `piko-install` binary is fine there.)

## 0) Preflight on host (in this repo)
Run:

```sh
cd flash
md5sum piko-install-mtd1-safe piko-install-mtd3-safe zImage mtd3.jffs2 updater-mtd1-safe.sh updater-mtd3-safe.sh
```

Save the output as your session proof before touching NAND.

## 1) Stage SD for mtd1 only
Copy these to SD card root (`/mnt/card` on device):
- `piko-install-mtd1-safe`
- `zImage`
- `updater.sh` = contents of `updater-mtd1-safe.sh`

Host-side staging command (edit SD mount path):

```sh
SD=/path/to/sd
cp piko-install-mtd1-safe zImage "$SD"/
cp updater-mtd1-safe.sh "$SD"/updater.sh
sync
```

Then re-check on host:

```sh
md5sum piko-install-mtd1-safe zImage updater-mtd1-safe.sh "$SD"/piko-install-mtd1-safe "$SD"/zImage "$SD"/updater.sh
```

## 2) Flash mtd1 from recovery menu
- Boot Cacko update flow and run Update with this SD.
- Let `piko-install` finish fully.

## 3) Hard reboot and verify mtd1 boot first
- Power cycle and confirm bootstrap path still boots.
- If mtd1 boot is not confirmed, stop and recover before touching mtd3.

## 4) Stage SD for mtd3 only
Copy these to SD root:
- `piko-install-mtd3-safe`
- `mtd3.jffs2`
- `updater.sh` = contents of `updater-mtd3-safe.sh`

Host-side staging command:

```sh
SD=/path/to/sd
cp piko-install-mtd3-safe mtd3.jffs2 "$SD"/
cp updater-mtd3-safe.sh "$SD"/updater.sh
sync
```

Then re-check:

```sh
md5sum piko-install-mtd3-safe mtd3.jffs2 updater-mtd3-safe.sh "$SD"/piko-install-mtd3-safe "$SD"/mtd3.jffs2 "$SD"/updater.sh
```

## 5) Flash mtd3 from recovery menu
- Run Update again with the mtd3-staged SD.
- On success, reboot and continue normal bring-up.

## Critical reminders
- Never use `raw=1` path for mtd1.
- Never use `start_addr=0` for mtd1; must be `917504`.
- Keep each pass isolated with a reboot/validation gap between them, unless
  running the combined single-pass playbook above with a confirmed backup
  in hand.

## In-system SMF update note

If recovery-menu flashing is unavailable and SMF must be updated from the
running OpenEmbedded system, do not use a raw `MEMERASE` + `write()` helper on
`/dev/mtd0`. That destroys the Sharp FTL mapping stored in OOB and breaks cold
boot even when the payload bytes themselves verify.

The only verified in-system path in this repo is `picoupdate` backed by the
current `pico-smf-write`, which uses `ioctl(MEMWRITE)` with `MTD_OPS_AUTO_OOB`
and rewrites the logical block numbers in first-page OOB so `sharpslpart` can
find the SMF kernel again after reboot.
