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

## The panel does not come on by itself

This is the part that is not obvious, and it cost a flash to find out.

Lighting the panel used to be **stage 2's** job. Stage 1 had `FB_W100=y`, so
`/dev/fb0` existed and `fbsplash` would happily map it, write 921600 bytes,
and exit 0 — into a framebuffer behind a panel nobody had turned on. No
error, no splash. *A registered framebuffer device is not a lit display.*

What was actually missing was **`CONFIG_SPI_PXA2XX`**. `corgi_lcd` is an SPI
*device*: it drives the Sharp LCDTG and registers the `corgi_bl` backlight
(`drivers/video/backlight/corgi_lcd.c`, `module_spi_driver`). With no PXA2xx
SPI master there is no bus for it to probe on, so the timing generator is
never programmed and the backlight never comes up. `LCD_CORGI` and
`BACKLIGHT_CLASS_DEVICE` were *already* `=y` in the minimal config — the
driver was compiled in the whole time and simply never probed.

Enabling it pulls in `PXA_SSP` as well, and costs about 11 kB of a budget
that did not have much to give. That is the price of drawing anything at all
in stage 1, and it is why the artwork is palettised as hard as it is.

Note the SSP port is shared — `ads7846` (touchscreen), `corgi-lcd`
(backlight) and `max1111` (battery ADC) all hang off it, and
`tools/setup-kernel-src.sh` already patches `spi-pxa2xx-platform.c` for a
double-`pxa_ssp_request()` bug. Stage 1 only needs the LCD half.

The backlight is then set explicitly from `init`, reading the panel's own
`max_brightness` (47 on this hardware) rather than hardcoding it. Stage 2
takes brightness policy over within seconds — see
[`HOWTO-BRIGHTNESS.md`](HOWTO-BRIGHTNESS.md) — so this only governs the
splash itself.

## How it is drawn

`init` mounts `devtmpfs` and `sysfs` first — `/dev/fb0` has to exist before
anything can be drawn, and `/sys` before the backlight can be raised — then
runs:

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
with splash    : 211071 bytes   (+32807)
```

And the bootstrap kernel that comes out the other side, built from a
pristine `tools/setup-kernel-src.sh` tree with this initramfs linked in —
including `CONFIG_SPI_PXA2XX` and `PXA_SSP`, without which none of it is
visible:

```
bootstrap zImage : 1282392 bytes
mtd1 slot budget : 1294336 bytes
headroom left    :   11944 bytes
```

Roughly a third of that went on the SPI master alone. The ~12 kB left is the
whole remaining margin for anything the bootstrap ever wants to carry —
treat it as close to spent, and re-measure before adding anything.

Both the cpio and the zImage are gzipped, so what costs flash is the
*compressed* size of the asset. That is why `--colors` in
`tools/make-splash.py` matters far more than it looks:

| logo height | truecolor | 64 colours |
|---|---|---|
| 418 (native) | 87 kB | 40 kB |
| 360 (default) | 72 kB | **33 kB** |
| 300 | 52 kB | 25 kB |

Palettising is very close to free visually — the panel is RGB565, so it
cannot display 8-bit-per-channel precision anyway. The default is **32
colours**, which on this flat-shaded artwork is indistinguishable from 64 at
panel resolution and 6.5 kB cheaper; on a partition with single-digit kB of
headroom that is worth having. **Dithering is deliberately off**: it
destroys the flat runs gzip depends on and can double the compressed size
for a picture that looks no better at this size.

Measured options, if you need to trade (headroom is what is left of the
1294336-byte slot after the whole kernel):

| logo height | colours | asset | headroom |
|---|---|---|---|
| 360 | 64 | 33231 | 5920 *(measured)* |
| 360 | **32** | **26688** | **11944** *(measured, shipped)* |
| 320 | 64 | 27496 | ~11700 |
| 300 | 64 | 25038 | ~14100 |
| 300 | 48 | 22813 | ~16300 |

The two measured rows come from real kernel builds; the rest are projected
from the asset delta, which tracks the zImage delta to within a few hundred
bytes (both are already gzipped, so the asset passes through the outer
compression roughly 1:1).

`--colors` is the knob to reach for first — it costs nothing you can see.
`--logo-height` is the one that actually changes the design.

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
