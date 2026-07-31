# How the touchscreen works under X (and how to re-calibrate it)

*Written 2026-07-31, when the touchscreen moved an X cursor for the first
time on this hardware.*

The Zaurus touchscreen is an **ads7846** on the SPI1 bus. Getting it to
move a cursor in X needed fixes at three separate layers, and each one
looked like "the touchscreen is broken" from the outside. If it stops
working, figure out which layer failed before changing anything --
they fail in very similar-looking ways.

---

## Layer 1 -- the SPI bus itself (kernel)

For a long time this bus did not work at all, so `ads7846` never bound
and `/dev/input/event2` did not exist. Two independent kernel bugs, both
fixed in `modules/spi/spi_pxa2xx_platform_patched.c`:

1. **`spi-pxa2xx-platform.c` requested the SSP port twice.**
   `pxa_ssp_request()` only matches a port whose `use_count` is 0 and
   increments it on success, so the second request always returned NULL,
   probe fell back to a zeroed `ssp_device` with `irq == 0`, and
   `request_irq(0, ...)` failed:
   `pxa2xx-spi pxa2xx-spi.1: error -EINVAL: cannot get IRQ 0`.
2. **DMA never completed.** Even once the bus enumerated, every transfer
   timed out (103 `corgi-lcd: SPI transfer timed out` per boot). The bus
   now runs in **PIO**; everything on it is tiny and slow, so this costs
   nothing. Root cause of the DMA failure was never chased down.

**Check this layer:**

```sh
dmesg | grep -E 'pxa2xx-spi|ads7846'      # want "registered host spi1"
                                          # and "touchscreen, irq 117"
ls /dev/input/                            # want event2 (+ mouse0)
grep ads7846 /proc/interrupts             # count must RISE when you tap
```

`/proc/interrupts` is the single most useful check here: the pen-down
GPIO IRQ rising on every tap proves the panel, wiring and IRQ are fine
and isolates any remaining fault to software above it.

**Read the data path, not just enumeration.** The device node existing
proves nothing -- at one point `ads7846` enumerated fine and still
emitted zero events, because its ADC reads were timing out. To test the
data path directly (blocks until 8 events arrive, so tap while it runs):

```sh
dd if=/dev/input/event2 of=/tmp/ts.bin bs=16 count=8
```

128 bytes means 8 real events. Decode them on the host with
`tools/decode-input-events.py` (or any `struct input_event` reader): a
working panel reports `BTN_TOUCH`, `ABS_X`, `ABS_Y`, `ABS_PRESSURE`,
then `SYN_REPORT`.

---

## Layer 2 -- kdrive's evdev driver (X server)

Stock xorg-server 1.10.6 kdrive evdev **only drives relative pointers**.
Patched in `modules/x11/xserver-kdrive-evdev-absolute.patch`:

- `EvdevPtrMotion()` handled `EV_ABS` by `ErrorF()`-ing the values as
  debug and enqueueing *nothing*.
- `EvdevPtrBtn()` ignored `BTN_TOUCH` entirely: it only accepts
  `BTN_MOUSE..BTN_JOYSTICK` (`0x110..0x11f`), and `BTN_TOUCH` is `0x14a`.

So the server read every event and threw it away. The giveaway was the
server log filling with `abs 0=... 1=... 24=...` lines while the cursor
sat still.

The patch adds absolute support: coordinates are scaled to the screen and
one event is emitted per `EV_SYN` frame, with `BTN_TOUCH` mapped to
button 1. **Emitting per-frame matters** -- `ads7846` sends `BTN_TOUCH`
*before* the `ABS_X`/`ABS_Y` of the same frame, so acting on the button
immediately would report the press at the *previous* position.

Absolute events must be enqueued **without** `KD_MOUSE_DELTA`;
`KdEnqueuePointerEvent()` keys off that flag to pick `POINTER_RELATIVE`
vs `POINTER_ABSOLUTE`.

---

## Layer 3 -- telling X which device to use

kdrive's pointer auto-detection takes the **first `/dev/input/eventN`
that opens**, not the first one that is actually a pointer -- so it grabs
`event0` (`gpio-keys-polled`) and never looks at the touchscreen. Name
both devices explicitly:

```sh
Xfbdev -retro \
    -keybd evdev,,device=/dev/input/event1 \
    -mouse evdev,,device=/dev/input/event2
```

`event1` is `matrix-keypad` (the keyboard), `event2` is the touchscreen.
Confirm the server really opened both:

```sh
ls -l /proc/$(pidof Xfbdev)/fd | grep input
```

---

## Re-calibrating

Calibration lives as four constants at the top of the patched
`hw/kdrive/linux/evdev.c`:

```c
#define EVDEV_ABS_CAL_XMIN 221
#define EVDEV_ABS_CAL_XMAX 3807
#define EVDEV_ABS_CAL_YMIN 282
#define EVDEV_ABS_CAL_YMAX 3800
```

These are **measured**, not from `absinfo`. The driver advertises the
full 0..4095 ADC range, but the panel only reaches roughly 221..3807 /
282..3800, so scaling by `absinfo` would squash the usable area toward
the middle of the screen.

Both axes increase in the same direction as the screen (raw X
left->right, raw Y top->bottom) on an SL-C760, so no swap or inversion is
applied. **If a replacement panel reads inverted, that has to be added --
the current code assumes this orientation.**

To re-measure, temporarily restore the debug print in `EvdevPtrMotion()`
(one `ErrorF` over `ke->abs[]`), then:

1. Start `Xfbdev` as above with the log going to a file.
2. Note the current sample count:
   `grep -c '^abs ' /tmp/xfbdev.log`
3. Press hard into the **top-left** corner, then the **bottom-right**.
4. Take the new lines and compute min/max **ignoring any sample with
   pressure 0** -- pen-up samples carry stale coordinates and will skew
   the result.
5. Put the new numbers in the `#define`s, regenerate the patch
   (`git -C userspace/src/xserver diff hw/kdrive/linux/evdev.c >
   modules/x11/xserver-kdrive-evdev-absolute.patch`), rebuild, redeploy.

---

## Rebuilding and redeploying after a change

```sh
tools/setup-x11-src.sh          # re-applies every tracked X patch
cd userspace/src/xserver
make -j"$(nproc)" LDFLAGS="-L<repo>/userspace/stage-target/usr/lib" \
     CWARNFLAGS='-Wall -Wno-error' LIBS="-lz -lfontenc"
```

Two traps:

- `make` will happily exit 0 **without relinking `Xfbdev`** if it thinks
  it is up to date. Delete `hw/kdrive/fbdev/Xfbdev` and
  `hw/kdrive/linux/liblinux.la` to force it, and check the log really
  contains `CCLD Xfbdev`.
- `CWARNFLAGS='-Wall -Wno-error'` is required: GCC 13 fails this
  2011-era tree on `-Werror=array-bounds` false positives in
  `render/picture.c`.

Copying onto a device where X is running fails with **"Text file busy"**.
Stop it first -- this rootfs's busybox has no `kill` applet, so use the
one this project ships (`userspace/src/kill.c`, installed at
`/usr/local/bin/kill`); note there is no `awk` or `pkill` either, so read
the PIDs out of `ps` by eye.
