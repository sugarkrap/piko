# Screen rotation on the swivel hinge

*Written 2026-08-01. Rewritten the same day, when it turned out the first
version solved the wrong problem — see "The 180° detour" at the bottom.*

The C7x0 lid swivels and folds flat over the keyboard. That posture is
**portrait**: a real 480x640 desktop, which is what the tablet-mode switch
means on this machine.

**Portrait is the cheap direction.** That is the one thing to understand
here, because every instinct says otherwise.

---

## The short version

| | |
|---|---|
| Automatic | `flipd` watches the switch and does it. Started from `rcS`. |
| Config | `/etc/piko/rotation.cfg` — `enabled`, `switch_invert`, `portrait_invert` |
| Turn it off | `enabled=0`, or `pkillx flipd` |
| Separately | `flip` turns whichever orientation you are in **upside down** (180°). Orthogonal; `flipd` never touches it. |

If the screen goes portrait when you *open* the clamshell and landscape
when you swivel it flat, set `switch_invert=1`. No rebuild.

---

## Why portrait costs nothing

The panel is **physically 480x640 portrait**. The 640x480 landscape
desktop this machine normally runs is itself produced by the w100's CRTC
rotating during scanout — `graphic_ctrl.portrait_mode = 1`, from
`INIT_MODE_ROTATED` in the board file, programmed by
`w100_set_dispregs()` (`modules/w100/w100fb_patched.c`).

So portrait is not "landscape plus a rotation". It is **one transform
fewer**:

| | framebuffer | CRTC `portrait_mode` | pixel clock divider |
|---|---|---|---|
| landscape | 640x480 | 1 — 90° | `pixclk_divider_rotated` = 6 |
| portrait | 480x640 | 0 — none | `pixclk_divider` = 2 |

Switching orientation is a framebuffer mode change; the CRTC picks up or
puts down its rotation for free. **No pixel is moved by the CPU in either
orientation.**

Note the divider. Portrait clocks the panel three times faster, because
unrotated scanout reads memory linearly. The ~39ms frame (~26Hz) measured
in `DEADLETTER-W100-VSYNC.md` was in the *rotated* mode — so portrait may
well refresh faster than landscape. Nobody has measured it; that is worth
doing before assuming portrait is the expensive posture.

### Why not `xrandr`

Xfbdev *can* rotate. `xrandr -o left` sets `scrpriv->randr`, and
`fbdevSetShadow()` then swaps `shadowUpdatePacked` for
`shadowUpdateRotate16_90` — turning every damage flush from a per-row
`memcpy` into a reversed per-pixel copy, on a 400MHz PXA255, forever, to
reproduce what the CRTC gives away. (There is no `xrandr` on this rootfs
anyway, and no libXrandr.) Do not "simplify" `flipd` into an `xrandr`
call.

---

## How the live resize works

Nothing here is new machinery. Every piece already existed; the patch
routes them.

`fbdevSetOrientation()` in the xserver fork runs
`fbdevRandRSetConfig()`'s sequence, with a hardware mode change swapped in
for the rotation and `scrpriv->randr` deliberately left at `RR_Rotate_0`
so the shadow stays a plain packed copy:

```
KdDisableScreen        tear down the root clip
fbdevUnmapFramebuffer  release the shadow
fbdevSetHwGeometry     FBIOPUT_VSCREENINFO; re-read var AND fix
                       (line_length changes -- 1280 <-> 960 bytes)
fbdevMapFramebuffer    re-does page flipping at the new size
fbdevSetShadow         plain shadowUpdatePacked, not a rotator
fbdevSetScreenSizes    pScreen->width/height from the new var
KdEnableScreen         KdSetRootClip resizes the ROOT WINDOW and
                       ResizeChildrenWinSize()s everything under it
RRScreenSizeNotify     ConfigureNotify on root -> the clients
```

That last event is the one the desktop cares about, and **Matchbox has
handled it since 2005**: the `ConfigureNotify` case in
`matchbox-window-manager/src/wm.c` is commented `/* screen rotation */`
and walks the client stack adjusting apps, dialogs and each panel's docked
edge by the size delta. We are not teaching it a new trick.

On any failure the old geometry is restored and the screen re-enabled —
the alternative on this board is a black display with no serial console
and no USB to recover through.

### Who drives it

`flipd` writes `PORTRAIT` or `LANDSCAPE` to the server's control FIFO
(`/tmp/.pikalibrate-ctl`, the channel `pikalibrate` already uses).

**X does the mode change whenever X is running.** Setting the mode behind
its back would leave the whole desktop drawing at the wrong size. With no
X — before the session starts, or a console boot — `flipd` sets the mode
itself and the console rotates with it.

---

## The touchscreen

The digitiser does not resize. It is bonded to the panel and keeps
reporting in the panel's own axes, so when the CRTC stops rotating, the
raw-to-screen mapping has to pick up the 90° it put down: **the axes
swap**. Raw X was measured running along the landscape screen's width —
i.e. along the panel's 640-pixel axis — so in portrait raw X drives screen
Y and raw Y drives screen X.

A 90° rotation is a swap plus exactly **one** reversal. Which one depends
on the direction the CRTC was rotating, and that has not been checked on
hardware, so it is a config key:

```
portrait_invert=x     reverse screen X   (default)
portrait_invert=y     reverse screen Y
```

Read that key when taps in portrait land **mirrored** along one axis (tap
top-left, get bottom-left). If they land **transposed** instead (tap
top-right, get bottom-left), the swap itself is wrong and that is a code
change in `EvdevPtrAbsolute()`, not a config line.

Each raw axis is scaled against the screen extent it now *drives*, not the
one it used to — scaling first and swapping afterwards would squash the
taller axis into the shorter one's range.

**Calibrate in landscape.** `pikalibrate`'s corner taps assume the
unswapped mapping.

---

## The 180° flip is a separate thing

`/usr/sbin/flip` (`flip`, `flip on|off|status`) writes
`/sys/devices/platform/w100fb/flip`, which moves the CRTC between
`portrait_mode` 1 and 2 in landscape — 180° apart. It turns whichever
orientation you are in upside down. `flipd` never touches it. You can be
in portrait, flipped, both or neither.

X follows it via the `ROTATE` command on the same FIFO, re-reading the
sysfs file rather than being told a value.

---

## Control FIFO summary

`/tmp/.pikalibrate-ctl` — named for `pikalibrate`, which has the path
compiled in, but now the server's general control channel. Commands match
as substrings:

| Command | Effect |
|---|---|
| `SUSPEND` | Let go of the input devices |
| `RESUME` | Take them back; re-read calibration and flip state |
| `ROTATE` | Re-read the 180° flip state from sysfs |
| `PORTRAIT` / `LANDSCAPE` | Live orientation change (this document) |

**Opening a FIFO for writing blocks until a reader appears.** `flipd` uses
`O_NONBLOCK`; the `flip` shell script checks the X socket exists first.
Without those guards either would hang forever on a console-only machine,
and there is no timeout, job control or `kill` on this device to escape
with.

---

## ⚠️ Not verified on hardware

None of this has run on the board. Three things are unknown:

1. **Switch polarity.** `CORGI_GPIO_SWB` has no `.active_low`; nobody has
   checked which posture reads high. → `switch_invert`.
2. **Which portrait axis reverses.** → `portrait_invert`.
3. **Whether the live resize is clean in practice.** The X-side rollback
   covers a failed mode set, but not, say, Matchbox mis-placing the panel.

What *was* verified: the whole X patch cross-compiles clean for ARM;
`flipd` was tested on the host against a synthetic `SW_TABLET_MODE` uinput
device (startup state via `EVIOCGSW`, both polarities, `enabled=0`, the
X-present path sending `PORTRAIT` on the FIFO, and the no-X path setting
the mode directly); and every register/geometry claim above is read out of
the driver source and cross-checked against `DEADLETTER-W100-VSYNC.md`,
which confirmed `portrait_mode=1` on this board by sampling registers
through `/dev/mem`.

---

## The 180° detour

The first implementation of this feature rotated the landscape desktop by
180° and called it done, on the reasoning that the swivel puts the panel
upside down in the same rectangle. That is a real transform the hardware
supports, and it is what `/usr/sbin/flip` still does — but it is **not
what the tablet switch is for**. The switch means portrait.

The mistake worth not repeating: that version's own documentation listed
"true portrait" under *What was NOT done*, with the reasoning "kdrive
cannot resize a live screen, so it would mean restarting X". That was
wrong on the facts. `fbdevRandRSetConfig()` resizes a live screen today,
`KdSetRootClip()` resizes the root window, and Matchbox has handled the
resulting event for twenty years. The capability was three functions away
the whole time and got written off in a sentence.

---

## Related

- [`HOWTO-X11-TOUCHSCREEN.md`](HOWTO-X11-TOUCHSCREEN.md) — the calibration
  the portrait transform composes with.
- [`HOWTO-BRIGHTNESS.md`](HOWTO-BRIGHTNESS.md) — `brightd`, the other
  daemon reading this evdev node.
- [`DEADLETTER-W100-VSYNC.md`](DEADLETTER-W100-VSYNC.md) — where
  `portrait_mode` and the scanout offset were measured on hardware.
