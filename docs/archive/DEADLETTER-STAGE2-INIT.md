# Dead Letter — stage 2 booted but ran the wrong init (busybox respawn loop)

*Written 2026-07-22. The two prior fixes in this chain got us here: the
kexec_load syscall number (`../DEADLETTER-KEXEC-SYSCALL.md`) let kexec jump
into stage 2, and `--atags` (`DEADLETTER-KEXEC-ATAGS.md`) let stage 2 get
past early setup to a console.*

---

## Symptom

Stage 2 booted fully (framebuffer console alive, penguin logo, kernel up)
and was NOT looping back to the bootstrap. But it never reached a usable
prompt — the screen filled with:

```
init: can't log to /dev/tty5
process '-/bin/sh' (pid 62) exited. Scheduling for restart.
init: can't log to /dev/tty5
process '-/bin/sh' (pid 63) exited. Scheduling for restart.
...
```

That is **BusyBox `init` (PID 1)** running with **no `/etc/inittab`**, so it
uses its compiled-in default: respawn a login shell on the console. The
shell has no working controlling tty, reads EOF, exits instantly, and init
respawns it forever.

## Cause

The old `zImage-full` (built before kexec ever worked, so never actually
exercised) had two settings that fought each other:

- `CONFIG_INITRAMFS_SOURCE=".../initramfs-minimal-v2.cpio.gz"` — an embedded
  initramfs. The kernel always runs the initramfs's `/init` and ignores
  `root=`.
- `CONFIG_CMDLINE_FORCE=y` with `CONFIG_CMDLINE="console=tty0"` — which
  stripped the `root=/dev/mtdblock2` we passed via kexec.

Net effect: stage 2 never mounted the real stage-2 rootfs (`nand-root` on
home) and never ran our real stage-2 init (`nand-root/init`: usb0 + dropbear
+ zsh login shell). Instead `/sbin/init` (busybox) ran with no inittab and
looped. Our intended init's banner (`=== zaurus-refresh full system (stage
2) ===`) never appeared — proof it wasn't running.

## Fix

Rebuild `zImage-full` to mount home as its root and run our init directly:

```
# stage-2 kernel .config
CONFIG_INITRAMFS_SOURCE=""          # no embedded initramfs
CONFIG_CMDLINE_FORCE=y
CONFIG_CMDLINE="console=tty0 root=/dev/mtdblock2 rootfstype=jffs2 rw rootwait user_debug=31 init=/init"
```

- `root=/dev/mtdblock2 rootfstype=jffs2` — mount home (this kernel's own
  numbering: mtd0=smf, mtd1=root, mtd2=home) as the real root. Requires (all
  present in the stage-2 config) `MTD_NAND_SHARPSL`, `MTD_SHARPSL_PARTS`,
  `MTD_BLOCK`, `JFFS2_FS`.
- `init=/init` — run `nand-root/init` (our script) as PID 1, NOT `/sbin/init`
  (busybox). For a real root fs the kernel would otherwise default to
  `/sbin/init`. Requires `BINFMT_SCRIPT=y` (present) since `/init` is a
  `#!/bin/sh` script.
- No embedded initramfs — `nand-root` on home already IS the root; embedding
  it in the kernel too was redundant and was the source of the confusion.
- `rootwait` — insurance against a root-device readiness race (the sharpsl
  NAND driver is built-in/synchronous, so it's belt-and-suspenders).
- `user_debug=31` + `CONFIG_DEBUG_USER=y` kept, so any stage-2 *userspace*
  fault (shell, dropbear) is legible instead of silent.

Removing the embedded initramfs also shrank `zImage-full` from 1,781,656 to
1,504,736 bytes (no budget pressure — it lives on the 68 MB home partition,
not the tight smf slot).

## Known follow-ups (not blockers for reaching a prompt)

- **usb0 / SSH:** `CONFIG_USB_ETH=m` (g_ether is a module, not built in), so
  `nand-root/init`'s `ifconfig usb0 ...` will fail until the module is loaded
  (`/lib/modules/.../g_ether.ko` on home + a `modprobe` in init, or rebuild
  with `CONFIG_USB_ETH=y`). dropbear starts regardless but has no interface
  to listen on yet. The local console shell still comes up.
- If busybox `init` is ever wanted instead of the script, it needs a real
  `/etc/inittab` with a getty/shell entry on the *working* console
  (`console`/`tty0`), not the default that loops.

## Standing lesson

An embedded initramfs and `root=` are mutually exclusive boot paths: the
initramfs `/init` wins and `root=` is ignored. For "kernel + separate rootfs
on flash," either (a) no embedded initramfs + `root=...` + `init=/init`, or
(b) embed the real rootfs as the initramfs. Don't half-configure both.
