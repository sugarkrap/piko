# Screen rotation on the swivel hinge

*Written 2026-08-01, when the tablet-mode switch was first wired to the
display.*

The C7x0 lid swivels 180 degrees and folds flat over the keyboard. In that
posture the panel is the same landscape rectangle in the same place, upside
down — so the desktop has to be turned around to stay readable, and the
touchscreen has to be turned around with it.

**The rotation is free.** It is a register field in the w100's display
controller, applied during scanout. Nothing re-renders, no pixel moves, and
the cost does not depend on what is on screen. Everything below exists to
decide *when* to write that register, and to keep the touchscreen pointing
at the same place the user is looking.

---

## The short version

| | |
|---|---|
| Automatic | `flipd` watches the switch and does it. Started from `rcS`. |
| By hand | `flip` (toggle), `flip on`, `flip off`, `flip status` |
| Config | `/etc/piko/rotation.cfg` — `enabled`, `switch_invert` |
| The register | `/sys/devices/platform/w100fb/flip` |
| Turn it off | `enabled=0` in the config, or `pkillx flipd` |

If the screen comes out upside down in the **clamshell** posture and
correct when swivelled, the switch reads the other way round on this board
than assumed: set `switch_invert=1`. That is a one-line config change, no
rebuild. See *Polarity* below — it is the one thing here that could not be
verified on hardware when this was written.

---

## Why this is free, and why the obvious alternative is not

The w100 rotates during scanout. `w100_set_dispregs()` (in
`modules/w100/w100fb_patched.c`) programs `graphic_ctrl.portrait_mode` with
one of four rotations, and **this board already depends on it**: the panel
is physically 480x640 portrait, and the 640x480 landscape framebuffer
everything draws into is landscape *only because* the platform data asks
for `INIT_MODE_ROTATED`, i.e. `portrait_mode = 1` (90 degrees).

So rotation here is not a feature to add. It is a field already in use,
with the other half of it exposed as a sysfs file:

```sh
cat /sys/devices/platform/w100fb/flip        # 0 or 1
```

With the framebuffer in its rotated (landscape) geometry:

| `flip` | `portrait_mode` | What you see |
|---|---|---|
| 0 | 1 — 90° | Clamshell: normal |
| 1 | 2 — 270° | Tablet: turned around |

90 and 270 are 180 apart, which is exactly the correction the swivel needs.
Writing the file costs one register write plus a mode-register refresh.

**The alternative is much worse, and it is the one you will reach for.**
Xfbdev *can* rotate: `xrandr -o inverted` sets `scrpriv->randr`, and
`fbdevSetShadow()` (`hw/kdrive/fbdev/fbdev.c`) then swaps
`shadowUpdatePacked` for `shadowUpdateRotate16_180`. That turns every
damage flush from a per-row `memcpy` into a reversed per-pixel copy — on a
400MHz PXA255 driving a panel that already only manages ~26Hz — in exchange
for a result the CRTC gives away for nothing. Do not "simplify" `flipd`
into an `xrandr` call.

(That server already runs with a shadow buffer, because the fork's
double-buffered page flipping needs one. So software rotation would not
even be paying for a *new* buffer — it would be paying, forever, to make
the copy that already happens several times slower.)

---

## Why 180 and not true portrait

180 is the whole correction for this hinge: the swivel does not change
which edge is long.

A true 480x640 portrait desktop is a **different operation**, and a much
more expensive one. It means `FBIOPUT_VSCREENINFO` with `xres`/`yres`
swapped, which changes the screen's dimensions — and kdrive cannot resize a
live screen, so it would mean restarting X and losing the session.

The driver side of that is already there: `w100fb_get_mode()` deliberately
matches a requested geometry against `corgi_fb_modes[]` **with the axes
swapped as well as straight**, and `w100fb_set_par()` picks `portrait_mode`
by comparing `par->xres` against the matched mode's — which is exactly how
the current landscape geometry ends up rotated in the first place. So
asking for 480x640 would give an unrotated portrait screen (with a
different pixel clock divider: `pixclk_divider` 2 rather than
`pixclk_divider_rotated` 6), still for free in the CRTC. The whole cost is
on the X side. Not implemented. If it ever is, it belongs behind an
explicit user action, not on a hinge switch.

One thing worth knowing before anyone tries: that divider difference means
the *rotated* mode we run in today is clocking the panel a third as fast,
which is consistent with the ~39ms frame (~26Hz) measured in
`DEADLETTER-W100-VSYNC.md` rather than the ~60Hz the panel is nominally
capable of. Whether a portrait desktop would actually refresh faster is an
inference from the divider, not something anyone has measured — but it is
the reason to measure rather than assume portrait is only a cost.

---

## The parts

### The signal — already in the kernel

`CORGI_GPIO_SWB` is declared as an `EV_SW` / `SW_TABLET_MODE` button in
`corgi_gpio_keys[]` (`modules/mach-pxa/corgi_patched.c`), driven by
`gpio-keys-polled` at a 250ms poll interval. `CONFIG_KEYBOARD_GPIO_POLLED=y`
and `CONFIG_INPUT_EVDEV=y` are already set. It lands on the same node
`brightd` reads for `SW_LID` — today `/dev/input/event0`.

Check it directly:

```sh
grep -A5 gpio-keys /proc/bus/input/devices     # want SW= with bit 1 set
```

`flipd` does **not** hardcode `event0`. It scans the event nodes and picks
the one whose `EV_SW` bits include `SW_TABLET_MODE`, because the node
number is a property of driver probe order, and guessing wrong fails
silently — it would just sit on a device that never reports the switch.

### The daemon — `flipd`

`userspace/src/flipd.c`. Static, libc only, same shape as `brightd`:

- Reads the switch's **current state** at startup with `EVIOCGSW`, not just
  edges — a machine booted with the lid already swivelled comes up the
  right way round instead of waiting to be swivelled twice.
- Blocks in `read()` the rest of the time. An idle machine pays nothing.
- Never writes a value that is already set (see *One stale frame* below).
- Re-reads its config on every switch event, so an edit takes effect on the
  next swivel — no restart, no signal (this rootfs's BusyBox has no `kill`).

It is a separate process from `brightd`, which reads the same evdev node,
deliberately: the two share no state, and rotation has to keep working
while `brightd` is stopped or inhibited. Two readers of one evdev node is
not a conflict — every open gets its own queue, and only `EVIOCGRAB` is
exclusive.

### The touchscreen — inside the X server

This is the half that is *not* free, and the half that will bite you if it
is missing. The panel and digitiser are physically bonded, so turning the
picture around turns the digitiser's axes around relative to what the user
now sees. A display that looks right and taps 180 degrees away is worse
than one that is upside down — at least an upside-down screen is obviously
wrong.

Xfbdev holds an `EVIOCGRAB` on the touchscreen, so nothing outside the
server can correct this. Our fork inverts both axes in
`EvdevPtrAbsolute()` (`hw/kdrive/linux/evdev.c`), after calibration
scaling, when the panel is flipped.

**The state lives in sysfs, not in X.** The server reads
`/sys/devices/platform/w100fb/flip` back rather than being told a value, so
there is no second copy to drift out of step. It reads it:

- at device-enable time (so a server started while swivelled is correct
  from the first tap),
- on `RESUME`,
- and whenever it is sent `ROTATE` on its control FIFO.

`flipd` and `flip` both send `ROTATE` after writing the register. If X is
not running there is nothing to notify and the write is skipped.

### The control FIFO

`/tmp/.pikalibrate-ctl` (`hw/kdrive/linux/linux.c`). Named for
`pikalibrate`, which has the path compiled in, but it is now the server's
general control channel. Commands are matched as substrings:

| Command | Effect |
|---|---|
| `SUSPEND` | Let go of the input devices |
| `RESUME` | Take them back; re-read calibration **and** orientation |
| `ROTATE` | Re-read orientation from sysfs; aim the touchscreen to match |

**Opening a FIFO for writing blocks until a reader appears.** `flipd` uses
`O_NONBLOCK` (no reader is `ENXIO`, no FIFO is `ENOENT`, both normal on a
console-only boot). The `flip` shell script cannot, so it checks that the X
socket exists and the FIFO is really a FIFO before writing — without those
guards it would hang forever on a machine with no X, and there is no
timeout, no job control and no `kill` on this device to escape with.

---

## Polarity

`SW_TABLET_MODE` comes from `CORGI_GPIO_SWB` with no `.active_low`, so the
reported value follows the raw GPIO level. **Whether that level is high in
the swivelled posture or in the clamshell one was not verified on hardware
when this was written** — the board was not reachable from the session that
built it.

`switch_invert` exists for exactly that reason, and is a config line rather
than a rebuild because it is the one unknown here. If the screen is upside
down in the clamshell and correct when swivelled:

```sh
# on the device
flip status                       # which way does it think it is?
```

then set `switch_invert=1` in `/etc/piko/rotation.cfg` — and **commit it**
in `rootfs/etc/piko/rotation.cfg`, because that file is appliance policy
and is overwritten on every deploy (same treatment as
`power-management.cfg`, deliberately unlike `touchscreen.cfg`, which holds
measured user calibration and is left alone).

To watch what it is deciding:

```sh
pkillx flipd
flipd -v                          # runs in the foreground, logs each decision
```

---

## One stale frame

Writing `flip` makes w100fb reprogram the graphics controller, and
`w100_set_dispregs()` ends by writing `mmGRAPHIC_OFFSET` — which resets the
scanout base to the front buffer. With the fork's double-buffered page
flipping running, X may briefly believe a different buffer is on screen
than actually is, costing up to one frame of stale or torn image until X's
next paint pans the scanout back where it belongs.

It is self-correcting (X's pan is the authority, and it re-pans on the next
damage flush), and it is why both `flipd` and `flip` read the current value
first and skip the write when it already matches. Do not "helpfully" write
the register unconditionally.

---

## What was NOT done

- **True portrait (480x640).** Needs an X restart. See above.
- **Rotating the console.** `fbcon` is unaffected by the flip in the sense
  that it does not need to know: the CRTC rotates whatever is in the
  framebuffer, so the console turns around with everything else. There is
  no separate console handling and none is needed.
- **Anything with `xrandr`.** There is no `xrandr` on this rootfs, and the
  RandR path is the slow one anyway.
- **Verification on real hardware.** Every claim about the w100 registers
  here is read out of the driver and cross-checked against the vsync work
  in `docs/DEADLETTER-W100-VSYNC.md` (which confirmed `portrait_mode=1` on
  this board by sampling registers through `/dev/mem`); the daemon's logic
  was tested on the host against a synthetic `SW_TABLET_MODE` device via
  uinput. What has not been observed is the panel actually turning around,
  and the switch's polarity.

---

## Related

- [`HOWTO-X11-TOUCHSCREEN.md`](HOWTO-X11-TOUCHSCREEN.md) — the calibration
  the inversion composes with. **Calibrate in the clamshell posture**, or
  `pikalibrate`'s corner taps will be fighting the inversion.
- [`HOWTO-BRIGHTNESS.md`](HOWTO-BRIGHTNESS.md) — `brightd`, the other
  daemon reading this evdev node.
- [`DEADLETTER-W100-VSYNC.md`](DEADLETTER-W100-VSYNC.md) — where the
  `portrait_mode=1` / scanout-offset behaviour was measured.
