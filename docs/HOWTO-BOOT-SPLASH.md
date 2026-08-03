# The bootstrap boot splash

*Written 2026-08-03. The stage-1 bootstrap used to boot to a wall of kernel
and shell text. It now paints a white screen with the piko artwork centred
on it, as the first thing that touches the panel, and holds that picture
until stage 2 takes the display over.*

The splash is drawn by the **bootstrap** (mtd1), not by stage 2. That is the
whole point — it is the earliest moment anything can be shown, and it is the
part of the boot that used to look broken. It also means changing it is a
NAND operation: see "Shipping it" at the bottom before you get attached to
an idea.

## How it works

| Piece | Where |
|---|---|
| Master artwork | `modules/initramfs/splash-src.png` |
| Rendered asset | `modules/initramfs/splash.ppm.gz` (tracked, pre-rendered) |
| Renderer | `tools/make-splash.py` (run by hand, needs Pillow) |
| Drawn by | `modules/initramfs/init`, first command after `devtmpfs` |
| Drawing tool | busybox `fbsplash` (`CONFIG_FBSPLASH=y` in `modules/initramfs/busybox.config`) |
| Installed by | `tools/build-initramfs.sh` |
| Console quieting | `CONFIG_CMDLINE` in `kernel.config-corgi-7.1.4-minimal` |

`init` mounts `devtmpfs` *alone* first — `/dev/fb0` has to exist before
anything can be drawn — then immediately runs:

```sh
/bin/fbsplash -s /splash.ppm 2>/dev/null || true
```

`/proc` and `/sys` are mounted afterwards. Nothing above them needs them,
and every line of setup that runs before the blit is a line of time the
screen spends blank.

The call is non-fatal on purpose. A bootstrap that cannot draw a picture
must still boot; if `/dev/fb0` is missing we lose the splash, not the board.

## Why the asset is a whole pre-composited screen

`fbsplash` blits a plain binary PPM at `IMG_LEFT`/`IMG_TOP` — **it does not
scale, and it does not clear the screen first.** So `splash.ppm` is not the
artwork, it is the entire 640x480 panel: white field, artwork already
centred in it. One blit at 0,0 paints everything, and the white background
comes along for free.

It also has to be stored *uncompressed* inside the initramfs.
`tools/build-initramfs.sh` gunzips the tracked `.gz` on the way in, because
the cpio is gzipped as a whole immediately afterwards — storing it
compressed would be gzip-on-gzip, which is both bigger and unreadable to
this busybox (`CONFIG_FEATURE_SEAMLESS_GZ` is off).

## The size budget is the entire design

The initramfs is linked *into* the bootstrap kernel
(`CONFIG_INITRAMFS_SOURCE`), and that kernel has to fit the mtd1 NAND slot:
**1294336 bytes**, enforced in `flash/kernel-flash.sh`,
`flash/run-stage2-smf-update.sh`, `userspace/src/piko-update.c` and in CI.
The bootstrap was already ~1.24 MB, so the splash had roughly **55 kB** to
live in — asset and `fbsplash` applet together.

Measured on this toolchain, initramfs with the splash minus initramfs
without it:

```
without splash : 178264 bytes
with splash    : 216403 bytes   (+38139)
```

And the bootstrap kernel that comes out the other side, built from a
pristine `tools/setup-kernel-src.sh` tree with this initramfs linked in:

```
bootstrap zImage : 1277224 bytes
mtd1 slot budget : 1294336 bytes
headroom left    :   17112 bytes
```

That 17 kB is the whole remaining margin for anything else the bootstrap
ever wants to carry. Treat it as spent.

Both the cpio and the zImage are gzipped, so what costs flash is the
*compressed* size of the asset. That is why `--colors` in
`tools/make-splash.py` matters far more than it looks:

| logo height | truecolor | 64 colours |
|---|---|---|
| 418 (native) | 87 kB | 40 kB |
| 360 (default) | 72 kB | **33 kB** |
| 300 | 52 kB | 25 kB |

Palettising to 64 colours is very close to free visually — the panel is
RGB565, so it cannot display 8-bit-per-channel precision anyway — and it
roughly halves the cost. **Dithering is deliberately off**: it destroys the
flat runs gzip depends on and can double the compressed size for a picture
that looks no better at this size.

If you need headroom back, `--logo-height` is the knob with the best
size-to-regret ratio. Drop to 300 and you get ~8 kB back.

## Keeping the picture on screen

Two things used to scribble over it.

**Kernel printk.** `CONFIG_CMDLINE` is now
`"console=tty0 quiet vt.global_cursor_default=0"` (`CONFIG_CMDLINE_FORCE=y`,
so this is the whole cmdline — the Sharp bootloader does not get a say).
`quiet` drops console loglevel to 4, which still lets errors, alerts and
panics through; it hides the routine chatter, not the things you would
actually want to see. `vt.global_cursor_default=0` stops a cursor blinking
in the corner of the splash.

**init's own output.** This is the one that needs care.  `init` used to do
`exec > /dev/tty0 2>&1` so that PID 1's output was visible at all (PID 1
starts with no controlling TTY, so stdio is fully buffered and nothing
appears until exec time). That reasoning still holds — only the destination
changed. Normal-path output now goes to a buffer file, and:

```sh
console_fallback() {
	exec > /dev/tty0 2>&1
	cat "$BOOTLOG" 2>/dev/null
}
```

is called on *every* path that gives up — failed `jffs2` mount, failed
`kexec -l` — before dropping to the bootstrap shell. So a boot that works
shows a picture, and a boot that *fails* shows the console exactly as it did
before the splash existed, scrollback included. That matters more than the
splash does: this console is the only diagnostic channel the board has.

One case is genuinely less visible than before, and it is worth knowing:
the loop that waits for `/mnt/home/boot/zImage-full` and
`/mnt/home/sbin/kexec` to become readable after the JFFS2 mount polls
forever. It never reaches a failure branch, so it never calls
`console_fallback` — a board stuck there now shows the splash indefinitely,
where it used to show the banner and then nothing. It was already a silent
infinite loop; the splash only changes what is painted over it. Adding a
timeout would be the fix, but note that this busybox is built with
`CONFIG_FEATURE_SH_MATH` off, so there is no `$(( ))` to count seconds
with — it needs a counter built some other way, not a one-line change.

The persistent `/mnt/home/debug.log` is untouched and still gets the kexec
output on every boot.

## Changing the artwork

Replace `modules/initramfs/splash-src.png`, then:

```sh
tools/make-splash.py                 # writes modules/initramfs/splash.ppm.gz
tools/build-initramfs.sh             # folds it into the initramfs
```

`make-splash.py` prints the compressed size and **fails** if it exceeds
`--budget` (default 40000). That guard is deliberately tighter than the real
limit so an oversized asset is caught in a second rather than a kernel build
later. The real check is still CI weighing the finished zImage.

The script is the only thing here that wants Pillow, and nothing in the
normal build path calls it — the build side only gunzips a tracked file, so
CI needs no image library at all.

## Shipping it

**This is a bootstrap change, so it is a NAND change.** Everything above
lands in mtd1, which is the partition that cannot be recovered without an SD
card and a lot of patience. The ordering rules in
[`HOWTO-OFFLINE-UPDATE.md`](HOWTO-OFFLINE-UPDATE.md) and
[`FLASH-MTD1-MTD3-SAFE.md`](FLASH-MTD1-MTD3-SAFE.md) apply in full.

- **Iterating**, device on WiFi: `flash/run-stage2-smf-update.sh --apply
  --image <zImage>` pushes a bootstrap straight at `piko-smf-write` without
  building a package. This is the loop to use while tuning the picture.
- **Shipping**: the bootstrap rides in a normal update package as
  `boot/zImage-smf`. `piko-update` compares it, writes `/boot/smf-pending`,
  and reboots; the NAND write only happens when you then run `smfcommit` on
  a board that has demonstrably come back up. Do not skip that reboot — it
  is the checkpoint that proves the new kernel boots while the old bootstrap
  is still intact.

Before either, boot it under QEMU (`flash/qemu-smoke-test.sh`,
[`HOWTO-QEMU-SMOKE-TEST.md`](HOWTO-QEMU-SMOKE-TEST.md)). Note that QEMU's
`-M spitz` has no W100 chip, so it will *not* show you the splash — it
confirms the kernel still boots and the initramfs still unpacks, nothing
more. The picture itself only proves out on real hardware.

## The loader

Not built yet. The mechanism is already in the applet: `fbsplash -f FIFO`
keeps the process alive reading percentages (`"NN\n"`) from a control pipe,
and an `-i` ini file positions the bar (`BAR_LEFT`, `BAR_TOP`, `BAR_WIDTH`,
`BAR_HEIGHT`, `BAR_R/G/B`). The bootstrap has natural milestones to feed it
— jffs2 mounted, payload files visible, `kexec -l` returned, jumping — so
the work is wiring those to a FIFO and making sure the background `fbsplash`
cannot outlive the `kexec -e`.
