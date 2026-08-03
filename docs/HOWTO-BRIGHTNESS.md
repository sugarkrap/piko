# Backlight control on the Zaurus C7x0

Three pieces, all shipped by `tools/chunked-deploy.sh`:

| Thing | What it is | Where it comes from |
|---|---|---|
| `/usr/sbin/bright` | one-word CLI, the single definition of the step ladder | `rootfs/usr/sbin/bright` (tracked) |
| `/usr/sbin/brightd` | policy daemon: Fn hotkeys, idle dim, lid blank, idle screensaver | `userspace/src/brightd.c`, built by `tools/build-userspace.sh` |
| `/usr/local/bin/toasters` | the idle screensaver `brightd` launches — see "The screensaver" below | `userspace/src/toasters.c`, built by `tools/build-toasters.sh` (needs the X11 stack staged; part of the Matchbox payload, not the plain userspace build) |

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

**Fn+4 brightens, Fn+3 dims** — both on the console and under X. Xfbdev
holds an exclusive grab on the keyboard and touchscreen, which would
normally starve `brightd` of every event; our fork now forwards what it
sees instead. See "The big one" below for why that was needed and how
it works.

Defaults: dim to level 5 after 60s idle, show the toasters screensaver
after 120s, blank after 300s, restore on any key or touch. Closing the
lid blanks immediately.

**Watching a video?** `touch /tmp/brightd.inhibit` suspends dimming and
blanking (hotkeys and the lid still work); delete it afterwards. MPlayer
produces no input events, so without this the screen dims mid-film.

To stop the daemon: `pkillx brightd`. There is no `kill` on this rootfs.

## The on-screen display

`mb-brightness` (`userspace/src/mb-brightness`) draws a bar at the top of
the screen showing the backlight level: the `video-display` monitor icon
on the left, a progress bar, the percentage on the right. Styled from the
theme's `PanelBgColor`/`PanelFgColor` and given the same height the panel
gives itself, so it reads as a piece of the panel that has floated to the
top of the screen. It sits 10px in from the top and both side edges and
hides itself 1.5s after the last change.

**It is an applet that docks nothing.** `mb_tray_app_hide()` is called
*before* `mb_tray_app_main()`, which makes `_init_docking()` early-return
— so no tray window is ever created and the panel reserves no space for
it. Same trick `mb-applet-card` uses to start with an empty slot; the
difference is that this one never unhides. It is in the panel's applet
list (`modules/x11/matchbox-session`) purely so it starts and stops with
the session.

That has one consequence worth knowing before changing anything in it:

> **A hidden applet receives no X events.**
> `mb_tray_handle_xevent()` guards the whole dispatch on
> `win_tray != None && !is_hidden`, so `xevent_cb` — and with it every
> Expose, ConfigureNotify and theme change — is never delivered. Only the
> poll callback still runs, because `get_xevent_timed()` sits below that
> guard. Everything in `mb-brightness` is therefore driven from
> `poll_callback()` and nothing waits on an X event.

**It owns none of the backlight.** `brightd` pokes it with one byte and it
then reads `/sys/class/backlight/corgi_bl` itself, so it cannot disagree
with what actually happened. Same one-owner rule `bright`/`brightd`
already follow — and it reads `brightness`, never `actual_brightness`, for
the measured reason in "The two traps" below.

```
/tmp/mb-brightness.fifo
  's'  show the OSD, re-reading the level from sysfs
  'h'  hide it now — sent by mb-volume's OSD when it wants the space
```

**Only explicit changes pop it up.** The poke lives in `run_bright()`,
which is called from exactly the two hotkey paths (evdev directly on the
console, and `'u'`/`'d'` over `/tmp/brightd.fifo` from the X server).
Idle dimming, lid blanking and the DPMS path call
`go_dim()`/`go_blank()`/`go_active()`, which write sysfs directly and
never go through `run_bright()` — an OSD lighting the screen up as it dims
to save power would be both absurd and self-defeating.

**`bright up` typed at a shell does not show it.** `bright` is a shell
script and cannot safely write to a FIFO (see the same trap under `vol` in
[`HOWTO-VOLUME.md`](HOWTO-VOLUME.md) — the open blocks forever when
nothing is reading, and `/tmp` here is jffs2 so a stale FIFO survives a
reboot). Only the hotkeys and anything that goes through `brightd` pop the
OSD.

`mb-volume` draws an identical bar in the identical place for volume, so
before showing, each writes `'h'` to the other's FIFO. One non-blocking
open that fails harmlessly when the other applet is not running, it cannot
ping-pong (hiding never notifies anybody), and it needs no shared state.
An X selection would be the textbook answer, but `SelectionClear` is an X
event — see the box above.

### Verified on hardware 2026-08-02, and one thing that went wrong

Working, confirmed by framebuffer grab: with the backlight at 24 of 47,
the OSD read **51%** — which is `24/47`, and the proof it read the right
sysfs file. Had it read `actual_brightness` (which the driver inflates to
40 at that setting) it would have shown 85%. Stepping via
`echo u > /tmp/brightd.fifo` — exactly what the X server sends on Fn+4 —
moved the level and popped the OSD each time.

**Then the whole X session died.** Every client went down at once with

```
XIO: fatal IO error 11 (Resource temporarily unavailable) on X server ":0"
```

including `matchbox-panel` and `matchbox-desktop`, neither of which this
work had touched. `xfbdev.log` recorded nothing at all.

**The likely cause is another process taking the VT, and it is almost
certainly the same thing behind the unexplained deaths recorded under
"The screensaver: toasters" below.** It happened a second time during the
same session, and that time the evidence was still on the box:

```
Fatal server error: LinuxInit: VT_WAITACTIVE failed      # /tmp/xfbdev.log

4188 root  matchbox-fbrun -y -n GMenuNX -- ./gmenunx     # ps
4206 root  ./gmenunx
```

`gmenunx` is an SDL program drawing straight to the framebuffer, so it
takes the VT. Its PIDs are *higher* than the session that had just been
restarted, i.e. it started afterwards and pulled the VT out from under a
running X server — which kills every client at once with exactly the
`XIO` above, and then stops the next `Xfbdev` from starting at all.

**This board is shared between working sessions.** Before concluding that
an X death is a bug in whatever you just deployed, run `ps` and look for
another framebuffer program. Note the shape of the evidence: *every*
client dying simultaneously, including ones you never touched, points at
the server or the VT, not at your client.

One change was made anyway, as the cheapest suspect to remove before the
VT contention was spotted: the first version of `mb-brightness` repainted
the OSD on every 150ms tick, to stand in for the Expose a hidden applet
can never receive. That meant a full image rebuild, an Xft text render and
an `XCopyArea` about seven times per appearance for a picture that had not
changed — a steady stream of requests at an unaccelerated server, bought
for nothing. It now paints only when the level changes. On the evidence
above that was **not** the cause; it was pointless traffic and is better
gone regardless.

Recovery. If `Xfbdev` survived, the session comes back without a reboot:

```
DISPLAY=:0 HOME=/root PATH=/usr/local/bin:$PATH setsid /etc/matchbox/session &
```

If it did not, check for the VT holder first — starting X under it just
fails with `VT_WAITACTIVE` again. And check the backlight: a board left
with no X and no input looks dead but is usually just blanked, so
`bright 20` before assuming anything worse.

## Configuration

`/etc/zaurus/power-management.cfg`, if present, overrides the compiled
defaults with plain `key=value` lines (same format as matchbox's
`kbdconfig` — `#` for comments, no quoting):

```
dim_secs=60
toast_secs=120
blank_secs=300
dim_level=5
suspend_on_lid=yes
```

A commented-out copy with the defaults spelled out ships at
`rootfs/etc/zaurus/power-management.cfg`. `tools/chunked-deploy.sh` sends
it **only if the device doesn't already have one** — same rule as
`/etc/piko/touchscreen.cfg` — so a redeploy never clobbers something
you've edited on the device.

**No restart needed.** `brightd` stat()s the file every time through its
main loop and reloads whenever the mtime changes — there is no reload
signal, because this device's BusyBox has no `kill`/`killall`/`pkill` to
send one with (see "No signal protocol" above). Save the file, wait a
couple of seconds, done.

`-d`/`-t`/`-b`/`-l` passed on `brightd`'s command line (the line in
`rootfs/etc/init.d/rcS`) always win over the config file, even across a
reload — a manual `brightd -d 5` for testing is never silently
overridden by whatever the file says. `suspend_on_lid` has no
command-line equivalent; it's config-only.

### `suspend_on_lid` — real system suspend, off by default

Setting `suspend_on_lid=yes` makes closing the lid do an actual
`echo mem > /sys/power/state`, not just blank the backlight. This board's
`cpu_pm_fns.valid` is `suspend_valid_only_mem`
(`modules/mach-pxa/pxa25x_patched.c`), so `mem` is the only state the
kernel will accept — this isn't a guess.

**The default is off, and that's deliberate, not caution theatre.** Per
`AGENTS.md`, this project's board is the *last spare one*, with no serial
console and no USB — the only way in is WiFi → SSH. A suspend that goes
to sleep but doesn't come back is not a "power-cycle and try again"
situation the way it would be on ordinary hardware: it's the one
remote-access path gone, indefinitely, with nobody local to press
anything.

**Before setting this to `yes`**, confirm resume actually works, by hand,
repeatedly, over SSH:

```
echo mem > /sys/power/state
```

Only flip it on once that has come back reliably every time.

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
touchscreen at all** through evdev. Our xserver fork works around this by
forwarding what it sees itself — see "The fix" below.

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

### The fix: X becomes a pure event source

X is the only process that can see input while it holds the grab, so X
has to be the event source. `userspace/src/xserver` is our own fork;
`hw/kdrive/linux/evdev.c` now forwards what it sees to `brightd`.
**`brightd` keeps ownership of all policy** — X makes no backlight
decisions of its own and just reports what it saw.

The channel is a FIFO, `/tmp/brightd.fifo`, created by `brightd`. One
byte per message:

| byte | meaning |
|---|---|
| `h` | heartbeat / hello — "I am alive and feeding you events". **Not** activity. Send on open, then every few seconds regardless of input. |
| `a` | input activity (any key or touch). Rate-limit it; this is a wake-up, not an event log. |
| `u` | brightness up (Fn+4) |
| `d` | brightness down (Fn+3) |
| `s` | screen saver ON — sent by `fbdevDPMS()`, see "Wiring X11 DPMS" below. Not activity. |
| `w` | screen saver OFF — same sender. Counts as activity. |

Unknown bytes are ignored rather than guessed at, so the protocol can
grow without breaking an older daemon.

**The heartbeat is the load-bearing part.** Without it, `brightd` cannot
distinguish "X is grabbing input and forwarding it, and the user really
is idle" (dim!) from "X is grabbing input and telling us nothing" (do not
dim — we are blind). Absence of `a` means both. A recent `h` separates
them: while a heartbeat is live, a grab is no longer a reason to suspend
idle policy.

`brightd` also holds a write descriptor on the FIFO itself. A FIFO whose
last writer closes goes to permanent EOF — `read()` returns 0 and
`select()` reports it readable forever — which would spin the loop at
100% CPU the moment X exited.

**Implemented and verified end to end on hardware**, both directions:

* the receiving half, by simulating the X side from the shell
  (`echo h > /tmp/brightd.fifo` etc.) with X really holding the grab:
  heartbeats alone → idle timer trusted despite the grab (dimmed 31→5,
  then blanked); `a` → restored, unblanked; `u`,`u`,`d` → 31→33→47→33
* the sending half, in `hw/kdrive/linux/evdev.c`: X connects
  (`brightd: connected to /tmp/brightd.fifo` / `X event source
  connected`), and with real activity forwarded, `brightd` correctly
  dims and restores under the live grab. Fn+3/Fn+4, lid blanking, and
  suspend/resume confirmed by hand at the keyboard.

Two bugs only showed up at this stage, both fixed in the same commit:

* **The heartbeat timer stopped after the first session restart.** The
  server resets whenever its last client disconnects — routine here,
  since matchbox restarts do exactly that — and a reset re-runs
  `OsInit()` → `TimerInit()`, which frees every timer. Guarding re-arm on
  a non-NULL static `OsTimerPtr` saw the now-dangling pointer and treated
  it as still armed, so the heartbeat went silent from generation 2
  onward. Caught via an `ErrorF` trail in the X log: the timer armed
  once, a reset happened, then nothing. Fixed by tracking
  `serverGeneration` and re-arming whenever it changes.
* **`tools/build-x11-stack.sh`'s `ACLOCAL_PATH`** covered `xorg-macros`
  but not `$STAGE/usr/share/aclocal`, where `xtrans` installs the
  `xtrans.m4` that `configure.ac` needs for `XTRANS_CONNECTION_FLAGS`.
  Invisible normally (a pre-generated `configure` already has the macro
  expanded), it broke the moment a `--force` rebuild of `xserver` ran
  `autogen.sh` fresh. Fixed in that script.

Removing the grab instead is the other obvious option and is deliberately
**not** taken: the grab is what stops keystrokes reaching the kernel VT
layer underneath X, and this keymap has `KEY_SYSRQ` on it.

## Wiring X11 DPMS through brightd

The goal past this point was real screensaver *content* (animated hacks,
not just a blanked panel) — see "The screensaver: toasters" below for
what that turned into. That needs an X client using the core screensaver
machinery, and that machinery had to actually do something first. Before
this, it didn't: `hw/kdrive/fbdev/fbdev.c`'s
`fbdevDPMS()` called `FBIOPUT_POWERMODE`/`FBIOBLANK`, which reach
`w100fb_blank()` (`modules/w100/w100fb.c`) — and that function
deliberately only tracks a `blanked` flag and skips the real
`corgi_lcd` suspend/resume, because those SPI calls stall the W100 bus
when triggered from this path (the same freeze this project already
routes around elsewhere). So `xset dpms force off`, or X's own
`-b`-style idle timeout if one were ever configured, would report
success and change nothing on the panel — worse than doing nothing,
because `KdSaveScreen` returning `TRUE` also means the DIX screensaver
core never falls back to painting anything of its own either.

`fbdevDPMS()` now sends `s` / `w` on `brightd`'s FIFO instead (its own
writer fd — DPMS transitions are unrelated to the input stream and rare,
so it does not share `evdev.c`'s connection). `brightd` remains the
single owner of `bl_power`; X only asks. `s` reuses the same `go_blank()`
the idle timer and lid switch already use, and `w` reuses `go_active()`,
so a DPMS-driven blank restores to the level the user had, exactly like
every other path in.

This makes DPMS a real, working backend rather than a silent no-op.
`brightd`'s own evdev-driven idle detection is unchanged and still the
primary path; this only makes the X-side entry point that used to be a
dead end actually reach the panel — useful on its own for anything that
calls `XForceScreenSaver`/`DPMSForceLevel` directly, independent of the
toasters screensaver below (which does not use this path at all — see
why in that section).

**Not yet verified on hardware** — implemented and reasoned through
against the same `bl_power` mechanism the rest of this document already
proved out, but the DPMS call path itself (`xset dpms force off/on`
against the patched server) has not been run on the device yet.

## The screensaver: toasters

`brightd` launches `/usr/local/bin/toasters` (`userspace/src/
toasters.c`) after `toast_secs` of idle (default 120s, between the 60s
dim and the 300s blank) and kills it with `SIGTERM` the moment there is
activity or it is time to actually blank — see `start_toaster()`/
`stop_toaster()` and the "SCREENSAVER CONTENT" header comment in
`brightd.c`.

**Why brightd launches it, not X's screensaver machinery.** The DPMS
wiring above exists and works, but toasters does not go through it.
`brightd` already has the reliable, hardware-proven idle clock (see WHY
NOT X at the top of this document); routing screensaver *activation*
through X as well would mean either giving X its own idle timer (the
exact dependency chain WHY NOT X avoids: no `xset`, no libXss) or having
`brightd` link Xlib just to call `XActivateScreenSaver()`, which it
deliberately does not do anywhere else. Simplest is `brightd` forking a
binary the same way `run_bright()` already forks `/usr/sbin/bright` —
one `fork()`/`execl()` to start, one `kill(pid, SIGTERM)` to stop,
gated on `/tmp/.X11-unix/X0` existing so an idle *console* (no X running
at all) does not fork a doomed child every `TICK_SECS`.

**Where the artwork comes from.** The sprites are
[torunar/flying-toasters-xscreensaver](https://github.com/torunar/flying-toasters-xscreensaver)
(MIT, Copyright © 2023 Mikhail Shchekotov), vendored as the submodule
`userspace/src/flying-toasters`. `toasters.c` `#include`s its two XPM
sheets directly — `img/toaster.xpm` is a six-frame wing flap, 64×64,
15 colours; `img/toast.xpm` is the single toast frame — and decodes them
with `XpmCreateImageFromData()`. That is the **only** reason `libXpm` is
in the X11 stack and the payload.

Upstream's *code* is not compiled, only its data. Its renderer draws into
the real root window through `vroot.h`, or into a hardcoded 1920×1080
window with `-windowed`, and animates at 60fps — none of which fits a
480×640 panel on a 400MHz part, and the root-window path is actively
wrong here (it leaves the desktop visible around the sprites and gives
nothing to grab input on, so a stray tap reaches the desktop underneath
instead of dismissing the screensaver). What is reused, besides the
artwork, is the diagonal down-and-left drift.

**Why plain Xlib, not a GL hack.** This board's w100 has no DRM/DRI
driver at all — see the "no hardware acceleration path" story on
`w100fb_blank()` in `modules/w100/w100fb.c` — so xscreensaver's *own*
`flyingtoasters`, which is a GL hack, cannot run here regardless of how
it is invoked. (This is worth stating precisely, because it was once
taken as a reason not to vendor the repo above either — but that project
is plain Xlib and `-lXpm`, with no GL and no xscreensaver libraries, so
the objection never applied to it.) `toasters.c` `XPutImage`s each sprite
through its shape mask onto an offscreen `Pixmap` and `XCopyArea()`s the
result to a fullscreen override-redirect window once per frame.

**~10fps, not 60.** Every frame copies the whole 480×640×16bpp panel
across an unaccelerated framebuffer; at upstream's 60fps that is roughly
35MB/s of blitting on a part that cannot do it, and the animation would
tear while eating the CPU an idle machine is supposed to be saving.
Sprites move further per frame to compensate, so the apparent speed is
unchanged. Same "budget for the actual hardware" instinct as `bright`'s
no-fades rule above.

It also grabs the keyboard and pointer and exits on its own if either
sees input, purely as a safety net for running it by hand over SSH
(`/usr/local/bin/toasters`) — a manual preview, not the normal way it
starts. `userspace/desktop/toasters.desktop`/`Icon=toasters.png` used to
give this a menu entry too, but that invited launching it by hand as a
matter of course rather than as a deliberate preview, so
`tools/build-matchbox-payload.sh`'s `LAUNCHERS` list no longer ships it —
the binary still does, since `brightd` execs it directly with no
`.desktop` involved. The intended dismissal path is still `brightd`
noticing activity and sending `SIGTERM`.

**Verified on hardware, 2026-08-01.** Watched running on the device under
the normal Matchbox session: the XPM sheets decode, the shape masks
composite cleanly against black, and the toasters and toast drift
down-and-left as intended, with the wing flap cycling. Captured with
`userspace/src/fbgrab.c` and decoded with `tools/decode-fb.py` —
successive grabs differ, so this is real motion rather than one painted
frame. (`fbgrab` is the only way to get the pixels: this panel's
`/dev/fb0` does not support `read(2)`, so `dd` returns zero bytes and
mmap is the sole path off the board. That is the same constraint
`fbgrab.c`'s header comment already records.)

Two things worth knowing before running it by hand over SSH, both
consequences of the preview being a path the design does not otherwise
use:

* **`brightd` keeps arming its own copy underneath you.** `toast_secs` is
  120s and SSH is not evdev input, so a board being driven entirely over
  the network is *idle* as far as `brightd` is concerned — it forks its
  own toasters on schedule while a manually launched one is already up
  and holding the keyboard and pointer grabs, and the two then contend
  for the same grabs. `touch /tmp/brightd.inhibit` (or `pkillx brightd`)
  before a long manual preview; the inhibit file is the gentler of the
  two and is what the top of this document already recommends.
* **With no window manager running it exits immediately** — status 0, in
  under about three seconds, nothing on stderr. Under the normal session
  it stays up and animates until something stops it. The input-grab
  safety net above is the obvious suspect (an override-redirect window
  mapping onto a bare `-retro` root with no WM is exactly where a
  synthetic enter/motion event would come from), but this was **not**
  root-caused, and the normal path — a full session, dismissed by
  `brightd`'s `SIGTERM` — is unaffected either way.

Also seen during the same session and **not** explained: the X session
twice ended up dead after a manual launch, leaving `Xfbdev` a zombie and
tty1 back at a console login, with `Can't deallocate console 2 Device or
resource busy` closing out `/tmp/xfbdev.log`. One of the two followed a
`pkillx` of my own, and the board rebooted somewhere in the middle, so
this is recorded as something to watch on the next session rather than as
a known toasters bug.

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
* With X up and aggressive timers and no heartbeat, the panel does
  **not** dim or blank.
* With X up and heartbeats arriving on the FIFO, it does dim and blank,
  and `a`/`u`/`d` restore and step correctly.
* With the patched X server actually running: it connects to the FIFO,
  brightd dims/restores correctly under the live grab, and Fn+3/Fn+4,
  lid blanking, and suspend/resume were confirmed by hand at the
  keyboard.

Outstanding: the xserver commit
(`kdrive-brightd-event-source`, `userspace/src/xserver`) is not yet
pushed to its fork, so the piko submodule pointer has not been bumped to
it. Until it is, a fresh clone still gets the pre-brightd X server and
the hotkeys/idle-dim work console-only, exactly as described in earlier
revisions of this document.
