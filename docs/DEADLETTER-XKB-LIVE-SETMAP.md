# Dead Letter — Xfbdev rejects any newly-added key in a live xkbcomp upload

*Written 2026-08-01, while wiring the Zaurus on/off button to trigger
suspend. Read this before adding another key to `userspace/xkb/symbols/
zaurus`, or before assuming a symbols-file change that compiles cleanly
offline will actually take effect on the device.*

---

## Summary

`xkbcomp -I... /etc/X11/zaurus.xkb :0` — the exact command `xsession`
runs after Xfbdev comes up, to apply this project's custom keymap to the
live server — fails with a reproducible X protocol error every time the
symbols file defines a keycode that had **no** keysym before:

```
X Error:  BadValue
  Request Major code 137 (XKEYBOARD)
  Request Minor code 9 ()
  Value 0x16450005
  Error Serial #93
  Current Serial #98
```

Same request, same value, same serial number (#93 of #98), on every
attempt. This is not a race, not a corrupted file, and not about which
key: three separate additions were tried — `<I213> { [ XF86Suspend ] }`,
then `<I213> { [ F13 ] }`, then `<I208> { [ F13 ] }` (different keycode,
different keysym) — and all three hit the identical error at the
identical serial.

The failure is **non-fatal** to the rest of boot: `xsession` only echoes
a warning and continues (`|| echo "warning: could not apply $KEYMAP..."`),
so the graphical session comes up normally; the new key simply never
takes effect, silently, unless something downstream (like
matchbox-window-manager trying to resolve it via `XKeysymToKeycode`)
happens to say so.

## What this looked like from the consumer side

matchbox-window-manager, reading `kbdconfig`'s new
`XF86Suspend=!/usr/sbin/suspend` line at its own startup (which runs
*after* `xsession`'s xkbcomp step), logged:

```
matchbox: Cant find a keycode for keysym 269025191
matchbox: ignoring key shortcut XF86Suspend=!/usr/sbin/suspend
```

269025191 = `0x1008ffa7` = `XF86Suspend`. `XKeysymToKeycode` correctly
reported failure, because the live keymap genuinely never got the
keycode -- confirmed independently by dumping the running server's
keymap (`xkbcomp :0 -`) and finding no `key <I213>` line in it at all,
even right after a boot that produced no *other* errors.

## Why existing key remaps in this same file are unaffected

Every custom binding already in `symbols/zaurus` --- the Fn-key
`ISO_Level3_Shift` remap, the digit row's Fn-level symbols, the X/C/V
Control-modifier remaps --- **replaces** the keysym(s) on a keycode that
the matrix-keypad already reports events for during Xfbdev's own
startup device probing, i.e. a keycode the server's own default keymap
already allocated *some* non-zero width for. Those live-uploads only
ever change *values already present*.

`<I213>`/`KEY_SUSPEND` is different in kind, not just in identity: it is
a keycode the server's own initial keymap apparently allocated **zero**
keysyms for, so getting even one keysym onto it via a live upload means
*growing* that keycode's width from 0, not replacing existing content.
That growth path is exactly what fails, independent of which keycode or
keysym is involved — matching the identical, content-independent error
above.

Whether this is a genuine bug in this xserver fork's kdrive XKB `SetMap`
handling (`xkb.c`'s `ProcXkbSetMap` or equivalent), or a limitation
inherited from the underlying keyboard-device setup (Xfbdev never
learning that the matrix-keypad *can* report `KEY_SUSPEND` in the first
place, so never allocating room for it) was not determined. `Xfbdev
-help` offers no flag to load a full custom keymap at server start (only
`-ardelay`/`-arinterval` mention XKB at all), which would have been the
obvious workaround -- give the server that width from its very first
keymap, never grow it live -- and isn't available here.

## What was NOT wrong

- The symbols-file grammar: `xkbcomp -xkb ... file.xkm` (an **offline**
  compile, no live server involved) succeeds cleanly for every variant
  tried, producing a `.xkm` that correctly contains `key <I213> { [
  XF86Suspend ] };`. The file is valid XKB; the live protocol upload is
  where this breaks.
- The keysym name/value: ruled out by the `F13` substitution test.
- The specific keycode: ruled out by the `<I208>` substitution test.
- `fork_exec()`'s shell invocation, `kbdconfig`'s parsing, or
  `XStringToKeysym("XF86Suspend")` resolving the name at all -- none of
  these were ever reached; the problem is upstream of all of them, in
  getting the keysym onto a keycode in the first place.

## How to actually wire this button, if it matters enough to pursue

Two real paths, both bigger than a symbols-file edit:

1. **Patch the xserver fork's kdrive XKB `SetMap` handling** to accept a
   width increase for a previously-zero-width keycode. Requires reading
   the actual server-side handler this build ships (not xkbcomp, which
   is a client) and understanding why it rejects growth specifically.
2. **Skip XKB for this one key.** The kernel already reports
   `KEY_SUSPEND` as a plain evdev event on `/dev/input/event1` (the same
   matrix-keypad node Xfbdev EVIOCGRABs while running -- see
   `docs/xfbdev-grabs-evdev-nodes` context in memory/HOWTO docs). A
   small daemon reading that node directly and shelling out to
   `/usr/sbin/suspend` on `KEY_SUSPEND` press would work independent of
   X or XKB entirely -- but note it would need `EVIOCGRAB` contention
   with Xfbdev sorted out, or to run only while Xfbdev does not hold
   that specific key (grabs are whole-device, not per-key, so this needs
   real thought, not a quick daemon).

Neither was attempted here. `/usr/sbin/suspend` and the **Suspend**,
**Reboot**, and **Go to TTY** menu entries all ship and work regardless
-- they run from `matchbox-desktop`/`mb-applet-menu-launcher` launching a
process directly, no XKB or key grab involved at all. Only the *physical
button* shortcut is blocked on this.
