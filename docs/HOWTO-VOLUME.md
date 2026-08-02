# Volume on the Zaurus C7x0

The companion to [`HOWTO-BRIGHTNESS.md`](HOWTO-BRIGHTNESS.md), and
deliberately built the same way. Read that one first if you are touching
the hotkey path — it works through *why* Fn chords cannot be handled the
obvious way on this device, and everything here inherits that reasoning
rather than repeating it.

Four pieces:

| Thing | What it is | Where it comes from |
|---|---|---|
| `mb-volume` | the panel applet — owns ALSA, the config file, the slider bubble **and the OSD** | `userspace/src/mb-volume` (submodule) |
| `/usr/sbin/vol` | one-word CLI: `vol up` / `vol down` / `vol mute` | `userspace/src/vol.c`, built by `tools/build-userspace.sh` |
| Fn+5 / Fn+6 | the media keys | `HotkeyChord()` in `userspace/src/xserver/hw/kdrive/linux/evdev.c` |
| `/tmp/mb-volume.fifo` | the control channel joining the three | created by `mb-volume` |

## Using it

**Fn+6 louder, Fn+5 quieter.** Each press steps 5% and pops the OSD up
at the top of the screen for 1.5s. Holding a key autorepeats and ramps,
and the OSD stays up until 1.5s after the *last* step.

From a shell (or over SSH):

```
vol              show the current level
vol up           one step louder
vol down         one step quieter
vol mute         toggle mute
vol show         pop the OSD up without changing anything
```

Tapping the panel's speaker icon still opens the slider bubble, exactly
as before — drag it, or use the Mute box.

## The OSD

A bar pinned 10px in from the top and both side edges, the same height
the panel gives itself (36px on this screen). Left to right: the applet's
own icon, a progress bar, the percentage. It is styled from the theme's
`PanelBgColor`/`PanelFgColor` — the keys `matchbox-panel` paints *itself*
with — so it tracks the panel's appearance rather than the yellow message
balloon the slider bubble uses.

It takes no input at all: override-redirect, no grabs, `ExposureMask`
only. You cannot click it and it cannot steal focus from what is
underneath it.

**It appears on a media key, and deliberately not when the bubble is
driving the volume.** The bubble already shows the level and the mute box
right under the finger; a second read-out at the far end of the screen
would be noise. If a media key arrives while the bubble happens to be
open, the bubble updates and the OSD stays down — same rule.

## Why the applet, and not a second daemon

`mb-volume` already owns the ALSA mixer, `/etc/zaurus/volumed`, and the
only other UI that shows the level. Putting the media keys anywhere else
would mean two processes writing the same mixer and the same config file,
and the applet's slider would then show a stale number until it was next
reopened.

So the OSD lives *inside* `mb-volume`, and everything else asks it to
act. That is the same one-owner split `bright`/`brightd` already use, for
the same reason.

## The control FIFO

`/tmp/mb-volume.fifo`, one byte per message, created by `mb-volume` at
startup:

| byte | meaning |
|---|---|
| `u` | volume up one step |
| `d` | volume down one step |
| `m` | toggle mute |
| `s` | show the OSD, change nothing |

Unknown bytes are ignored rather than guessed at, so the protocol can
grow without breaking an older applet — the rule `brightd`'s FIFO already
follows. (It is also what makes `echo u > /tmp/mb-volume.fifo` work
despite the trailing newline.)

A FIFO rather than an X `ClientMessage` because it is what every writer
can already reach: the X server's evdev layer is not an X client, `vol`
is a 100-line C program with no Xlib, and a bare `echo` over SSH is the
only practical way to test any of this on a board whose keyboard is the
thing under test.

The read end is wired into the tray app's own `select()` via
`mb_tray_app_set_poll_fd()`, so there is no polling and no added latency.
The timeout callback is armed **only while the OSD is up** and cleared
when it hides, so an idle applet still blocks in `select()` costing
nothing — this device is on a battery.

### The libmatchbox one-liner

`mb_tray_app_set_poll_fd()`, the `MBTrayApp` field behind it and the
`select()` that honours it have all been in `libmb/mbtray.c` since
forever — **only the declaration was missing from `mbtray.h`**, which
made the feature unreachable from outside the library. Adding it is the
whole libmatchbox change. Confirmed the symbol was already exported from
the shipped `libmb.so` (`nm -D` finds it at `000082cc T`), so this is a
visibility fix with no ABI change and nothing else needed rebuilding to
link against it.

## Why not a matchbox keybinding, or XGrabKey

Same answer as brightness, and
[`HOWTO-BRIGHTNESS.md`](HOWTO-BRIGHTNESS.md) has the long version.
Briefly:

* `userspace/xkb/symbols/zaurus` declares `ISO_Level3_Shift` on `<FK03>`
  with **no `modifier_map` entry**, so which real modifier bit Fn sets —
  if any — is undefined. A grab with an explicit mask either never fires
  on the chord, or fires on the bare digit and makes it untypable. Both
  answers are broken.
* While `Xfbdev` is up it holds an `EVIOCGRAB` on the keyboard node, so
  no other process can read the chord from evdev either.

That leaves exactly one place that can see it: the X server's own evdev
reader. `HotkeyChord()` (formerly `BrightdKey()`) handles the raw
keycodes there, consumes the chord so the digit is not typed into
whatever has focus, and writes one byte to the relevant FIFO.

Volume and brightness use **separate FIFOs and separate fds** on purpose.
The two daemons are unrelated and either can be absent; multiplexing them
would mean `brightd` had to be running for the volume keys to work.

## Which keys, and how to change them

**Fn+5 = quieter, Fn+6 = louder — and unlike Fn+3/Fn+4, that is not what
the keycaps say.** This keyboard has no volume keys and no volume icon
anywhere on it; the digit row's Fn legends are zoom, brightness and IME.
The pair had to be *chosen* rather than read off the hardware, and 5/6
were taken because they sit immediately beside the brightness pair, which
makes the two ladders adjacent and consistent to reach.

To retarget them, change two `KEY_n` constants:

```c
/* userspace/src/xserver/hw/kdrive/linux/evdev.c, HotkeyChord() */
case KEY_6: if (value != 0) VolumedSend ('u'); return TRUE;
case KEY_5: if (value != 0) VolumedSend ('d'); return TRUE;
```

and the two matching lines in `userspace/xkb/symbols/zaurus`. Adding a
mute chord is one more `case` there sending `'m'`, which `mb-volume`
already understands.

The keysyms in the layout (`XF86AudioRaiseVolume`/`XF86AudioLowerVolume`)
are documentary, not load bearing — the chord is consumed at the evdev
layer and never reaches an X client. They are set anyway so anything that
inspects the layout sees something meaningful, exactly as Fn+3/Fn+4 do.
Nothing grabs them, so there is no double-stepping.

## The trap: `vol` cannot be a shell script

`echo u > /tmp/mb-volume.fifo` **blocks forever** when `mb-volume` is not
running. Opening a FIFO for writing waits for a reader — and this
device's `/tmp` is on the root jffs2, **not** a tmpfs, so a FIFO left
behind by a dead session is still sitting there after a reboot looking
exactly like a live one.

A shell that hits that is stuck: there is no `^C` on the framebuffer
console, and this busybox has no `kill`/`killall`/`pkill` applet to
rescue it with (`pkillx` exists precisely because of that hole, but you
need a *second* shell to run it from).

`O_NONBLOCK` is the fix — on a FIFO it makes an `O_WRONLY` open with no
reader fail immediately with `ENXIO` instead of waiting. There is no way
to ask busybox ash for that flag, and this busybox has no `timeout`
applet to bound the wait from outside either. Hence `vol` is a real
program. The X server's media-key path opens the same FIFO the same way,
for the same reason.

`mb-volume` also holds its **own write end** of the FIFO open and never
writes to it. A FIFO whose last writer closes goes to permanent EOF —
`read()` returns 0 and `select()` reports it readable forever — which
would spin the applet's main loop at 100% CPU the moment the X server
exited. `brightd` holds its own FIFO open for exactly the same reason.

## Building and deploying

`vol` is built by `tools/build-userspace.sh` and shipped to
`/usr/sbin/vol` by `tools/chunked-deploy.sh`, both guarded the same way
`pkillx` and `cardswap` are — a checkout that has not built it yet simply
has no shell path to the volume, and nothing else breaks.

`mb-volume` and `libmatchbox` are submodules; the X server change is in
the `userspace/src/xserver` fork. All three need their pointers bumped in
this repo after their own commits land, which is the normal loop here.

## Verification status

Built, not yet run on hardware. What has actually been checked:

* `mb-volume.c` cross-compiles and **links** to an ARM binary against the
  staged `libmb` and `libasound`, with no new warnings.
* `evdev.c` compiles clean for ARM against the configured xserver tree —
  the only warning it adds is the same `-Wlogical-op` on
  `errno != EAGAIN && errno != EWOULDBLOCK` that the existing
  `BrightdSend()` already produces, i.e. a faithful copy of the house
  idiom.
* `vol` builds static for ARM and was exercised natively: with no FIFO,
  with a **stale FIFO and no reader** (returns `mb-volume is not running`
  and exits 1 rather than hanging — this is the trap above, tested), and
  with a reader attached (`u`, `d`, `m`, `s` arrive as single bytes).
* `userspace/xkb/zaurus.xkb` compiles with `xkbcomp` and both
  `XF86Audio*` keysyms land on `<AE05>`/`<AE06>` at levels 3 and 4.

**Not yet verified on hardware**, and worth watching on the first run:

* that the OSD's colours and 36px height really do read as "the panel" on
  the device rather than just in the arithmetic;
* that the chord is consumed cleanly — pressing Fn+5/Fn+6 must not type
  `5`/`6` into whatever has focus;
* that the X server reconnects to the FIFO after a session restart (the
  applet is recreated, so its FIFO reader goes away and comes back —
  `VolumedSend()` drops the fd on `EPIPE` and `VolumedEnsureOpen()`
  retries, but the round trip has not been watched).

Per `docs/DEADLETTER-XKB-LIVE-SETMAP.md`, note that both keys changed
here are **replacements** of keycodes the server already allocated width
for, not additions — which is the case that works. Do not add a
previously-undefined keycode to that file.
