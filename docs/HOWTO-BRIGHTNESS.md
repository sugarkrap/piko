# Backlight control on the Zaurus C7x0

Two pieces, both shipped by `tools/chunked-deploy.sh`:

| Thing | What it is | Where it comes from |
|---|---|---|
| `/usr/sbin/bright` | one-word CLI, the single definition of the step ladder | `rootfs/usr/sbin/bright` (tracked) |
| `/usr/sbin/brightd` | policy daemon: Fn hotkeys, idle dim, lid blank | `userspace/src/brightd.c`, built by `tools/build-userspace.sh` |

## Using it

```
bright              show the current level
bright up / down    one step (the same ladder Fn+4 / Fn+3 use)
bright max / min    ends of the ladder
bright off / on     blank / unblank, keeping the level
bright 20           explicit 0..47
bright save         remember this level across reboots (-> /etc/brightness)
bright restore      re-apply it (rcS does this at boot)
```

**Fn+4 brightens, Fn+3 dims** — on the console. Under X they do **not**
work yet: Xfbdev holds an exclusive grab on the keyboard, so `brightd`
never sees the keys. See "The big one" below.

Defaults: dim to level 5 after 60s idle, blank after 300s, restore on any
key or touch. Closing the lid blanks immediately, and works under X too.
Change them by editing the `brightd` line in `rootfs/etc/init.d/rcS`
(`-d`, `-b`, `-l`, `-v`).

Idle dimming is automatically suspended while X is running, for the
reason in "The big one" below — so in practice, today, it applies to the
console session only.

**Watching a video?** `touch /tmp/brightd.inhibit` suspends dimming and
blanking (hotkeys and the lid still work); delete it afterwards. MPlayer
produces no input events, so without this the screen dims mid-film.

To stop the daemon: `pkillx brightd`. There is no `kill` on this rootfs.

## The two traps

### 1. `actual_brightness` lies — read `brightness`

`corgi_bl_set_intensity()` in `modules/lcd/corgi_lcd.c` does

```c
if (intensity > 0x10)
        intensity += 0x10;
```

and then stores that **adjusted** value in `lcd->intensity`, which is
exactly what `.get_brightness` returns — i.e. what `actual_brightness`
reports. Measured on hardware 2026-07-31:

| wrote | `brightness` | `actual_brightness` |
|---:|---:|---:|
| 16 | 16 | 16 |
| 17 | 17 | **33** |
| 31 | 31 | **47** (== `max_brightness`) |
| 47 | 47 | **63** (> `max_brightness`) |

The driver's own `default_intensity` is 31, so **on a freshly booted
machine `actual_brightness` already reads exactly `max_brightness` while
the panel sits at two-thirds.** Anything that reads it concludes there is
no headroom left. Read `brightness`.

This is upstream behaviour, not something this project introduced, and it
is left alone deliberately: `brightness` is correct, well-defined and
enough, and changing `.get_brightness` would be a mainline ABI change for
no gain here.

The 16→17 seam is not arbitrary either. Bit 5 of the value written to the
LCDTG `DUTYCTRL` register is split off to drive the `BL_CONT` GPIO, so the
hardware really has two overlapping ranges (duty 0–16 with `BL_CONT` low,
duty 1–31 with it high). The ladder in `bright` is spaced to straddle it
rather than pretend the scale is linear.

### 2. Every write costs an SSP transaction

`.../brightness` writes go over the SSP bus shared with the **touchscreen
and the battery ADC**, and `corgi_lcd`'s `kick_battery` hook additionally
pokes `sharpsl-pm` on each one. So: no fades, no animation, and never
write a value that is already set. `brightd` dims in a single write.

## The big one: X grabs the input nodes

**While Xfbdev is running, `brightd` cannot see the keyboard or the
touchscreen at all**, so under X the hotkeys and idle dimming do not work
yet. This is the open item; layers 1 and 2 (`bright`, persistence) and
lid blanking are unaffected.

kdrive calls `EVIOCGRAB` on both devices — `hw/kdrive/linux/evdev.c`, in
the enable path for the keyboard and for the pointer. An evdev grab is
taken on the input **device** (`input_grab_device()` sets `dev->grab`),
not on the evdev handler, so while it is held every other reader is
starved: other evdev clients and other handlers such as `mousedev` alike.
Reading `/dev/input/mice` instead does not dodge it.

Measured on hardware, reproducibly and across a reboot:

```
/dev/input/event0 [gpio-keys-polled]:   FREE
/dev/input/event1 [matrix-keypad]:      GRABBED (EBUSY)
/dev/input/event2 [ADS7846 Touchscreen]: GRABBED (EBUSY)
```

`event0` — the lid and the other `EV_SW` switches — is never opened by X,
which is why lid blanking works regardless.

### What this would have broken

A starved `brightd` receives no events, concludes the machine is idle,
and dims and then blanks the panel *while the user is typing*. Timer
tests do not catch this: the timers fire perfectly, they are just
measuring the wrong thing.

So `brightd` probes for the grab (attempting one is the only reliable
test) before acting on the idle timer, and **suspends idle policy
entirely whenever the keyboard is grabbed**, re-checking every 30s
because X can start and restart under it. If it is starved while already
dimmed or blanked it restores first — it will not hold the panel dark on
the strength of a timer it cannot trust. Verified: with X up and `-d 5
-b 10`, the panel stays put for 22s and the log reads
`keyboard grabbed (X) -- idle policy suspended`.

### The fix

X is the only process that can see input while it holds the grab, so X
has to be the event source. `userspace/src/xserver` is our own fork, so
the intended change is a small patch to kdrive's evdev reader making it
report activity (and the Fn+3/Fn+4 chord) to `brightd`, with `brightd`
keeping ownership of the actual policy. Not done yet.

Removing the grab instead is the other obvious option and is deliberately
**not** taken: the grab is what stops keystrokes reaching the kernel VT
layer underneath X, and this keymap has `KEY_SYSRQ` on it.

## Why the hotkeys are not matchbox keybindings

The obvious route is `XF86MonBrightnessUp=!bright up` in
`/etc/matchbox/kbdconfig`. It does not work here.

matchbox grabs a binding with an **explicit modifier mask**
(`keys_grab()`, `matchbox-window-manager/src/keys.c`), and
`userspace/xkb/symbols/zaurus` declares `ISO_Level3_Shift` on `<FK03>`
with **no `modifier_map` entry** — so which real modifier bit Fn sets, if
any, is not defined by the layout. Both possible answers break it:

* if Fn does set a real bit, a mask-0 grab never fires on Fn+4;
* if it sets none, that same grab fires on a **bare `4`** and the digit
  becomes untypable.

At the evdev layer Fn is just `KEY_F3` held down and the chord is
unambiguous, so `brightd` handles it there. That also buys console
support and independence from whether X is even running — which matters
on a machine whose only remote access is WiFi.

The layout still maps Fn+3/Fn+4 to `XF86MonBrightnessDown`/`Up` so the
keys report something meaningful to anything that looks at them. Nothing
grabs those keysyms, so there is no double-stepping.

## Verified on hardware (2026-07-31)

* Backlight visibly steps across the full ladder, confirmed by eye — not
  just by exit status.
* `bl_power=4` blanks and `bl_power=0` restores, with `brightness`
  surviving intact across the blank (which is why `brightd` blanks that
  way instead of writing 0).
* `brightd` dims 31→5 on the dim timer and blanks on the blank timer.
* `/tmp/brightd.inhibit` suppresses both; removing it resumes.
* `pkillx brightd` stops it.
* With X up, `event1`/`event2` refuse a grab with `EBUSY` and `event0`
  does not — reproduced across a reboot.
* With X up and aggressive timers, the panel does **not** dim or blank,
  and the daemon logs that it suspended idle policy.

Not yet verified, because it needs someone at the keyboard: that activity
actually restores from dim/blank, and the Fn+3/Fn+4 chord on the console.
Both are blocked from meaningful testing under X by the grab above.
