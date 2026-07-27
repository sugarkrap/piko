# Dead Letter — kexec must be told `--atags` on this ATAG board

*Written 2026-07-22. Fix applied, pending final on-hardware confirmation
(the previous fix in this chain, the kexec_load syscall number, is what
first let kexec jump into stage 2 at all — see
`docs/DEADLETTER-KEXEC-SYSCALL.md`).*

---

## Symptom

Once the `kexec_load` syscall bug was fixed, `kexec -l` + the automatic
`kexec -e` in `/init` worked: the bootstrap kernel printed
`kexec_core: Starting new kernel` and its final `Bye!`, then **jumped into
stage 2 and froze with zero output** — no bootstrap banner (not a loop), no
stage-2 banner, no kernel panic, just a dead screen still showing the
bootstrap kernel's last frame.

"Frozen with zero output after Bye!" means stage 2 was entered but hung
**before it could re-initialize the LCD/framebuffer console** — i.e.
somewhere between the kexec purgatory and stage-2's `setup_arch` /
`paging_init`, which is before any console exists on a framebuffer-only
(no serial console we can see) setup.

## Cause

`/init` invoked:
```sh
kexec -l /mnt/home/boot/zImage-full \
    --append="console=tty0 root=/dev/mtdblock2 rootfstype=jffs2 rw"
```
with **neither `--atags` nor `--dtb`**. In `kexec/arch/arm/kexec-zImage-arm.c`:

- ATAGs are only built and loaded when `--atags` is passed (guarded by
  `if (use_atags)`).
- When neither `--atags` nor a `--dtb` is given, the loader falls back to
  looking for a device tree at `/sys/firmware/fdt` (`have_sysfs_fdt()`).
  This is an **ATAG-based board** (board-file `MACHINE_START(HUSKY)`, no
  device tree), so that file does not exist and `dtb_file` stays NULL.
- Result: stage 2 is handed **no boot data at all** — no `ATAG_MEM`
  (memory layout), nothing. `kexec -l` still returns 0 (not a hard error).

Without `ATAG_MEM`, the new kernel doesn't know where/how much RAM there is
and hangs in early setup, before console. Silent freeze.

## Fix

Add `--atags` to the load:
```sh
kexec -l /mnt/home/boot/zImage-full \
    --atags \
    --append="console=tty0 root=/dev/mtdblock2 rootfstype=jffs2 rw"
```

`--atags` makes kexec read `/proc/atags` from the **running bootstrap
kernel** (which requires `CONFIG_ATAGS_PROC=y` — confirmed present) and
reproduce them for stage 2, preserving `ATAG_MEM` and replacing
`ATAG_CMDLINE` with `--append`. Stage 2 needs `CONFIG_ATAGS=y` to consume
them — confirmed present in the stage-2 config too.

Note: stage 2 is built `CONFIG_CMDLINE_FORCE=y` with
`CONFIG_CMDLINE="console=tty0"`, so it ignores the passed `ATAG_CMDLINE`
and uses its own `console=tty0`. That's fine — the load-bearing part of
`--atags` here is `ATAG_MEM`, not the cmdline. The `--append` is kept for
intent/clarity even though FORCE discards it.

## Standing lessons

1. **On an ATAG board, kexec needs `--atags` explicitly.** No atags and no
   dtb = the new kernel gets no memory map and hangs before console. This
   is not reported as an error by `kexec -l`.
2. **A silent freeze immediately after `Bye!` with no stage-2 output** is
   the fingerprint of a boot-protocol/early-setup problem (atags, machine
   id, memory), not a userland or driver problem — because it's before the
   new kernel's console comes up. `user_debug` won't help here (that's for
   userspace faults); the GPIO/LED boot markers in `head.S` / `pxa25x.c` /
   `corgi.c` are the diagnostic of last resort for pre-console kernel hangs
   (visible on AC power via the orange GPIO13 LED sequence).

## Known next issue (separate from this one)

Even once stage 2 boots past this freeze, its kernel config still has a
setup problem for actually running the stage-2 userland: it embeds
`initramfs-minimal-v2.cpio.gz` (the *bootstrap* initramfs, whose `/init`
mounts home and kexecs) as a built-in initramfs, and `CONFIG_CMDLINE_FORCE`
strips the `root=/dev/mtdblock2` we pass. So stage 2 may run the wrong
`/init` (potentially re-kexec looping) instead of the real stage-2 rootfs
init (`nand-root/init`: usb0 + dropbear + zsh). That's the next fix — a
`zImage-full` rebuild (mtd3), tackled after this `--atags` change is
confirmed to get stage 2 to a console.
