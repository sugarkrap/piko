# The Settings window: one door for every configuration app

*Written 2026-08-03, when `piko-settings` was added.*

`piko-settings` is the ROM's settings manager: every configuration app in
one categorised, touch-friendly list — icon, name, one line of description,
under a category heading. The shape is XFCE's settings manager, scaled down
to a 400MHz PXA255 and a panel that is 640x480 open and 480x640 swivelled.

It is an FLTK app (`userspace/src/piko-settings.cxx`), built and shipped
exactly the way `mb-wallpaper-picker` and `pikostore` are — see
`docs/HOWTO-FLTK.md`. It links no `libmb`.

For the desktop and panel this sits on, see
`docs/HOWTO-MATCHBOX-DESKTOP.md`.

---

## Why it exists

The desktop is a **flat** launcher. `matchbox-desktop-classic`'s
`modules/dotdesktop.c` deliberately stopped bucketing applications into
vfolders (see the long comment in `dotdesktop_init()`): on a device with a
couple of dozen applications, opening folders one at a time to find one is
strictly more work than showing all of them.

That is the right call for applications and the wrong one for *settings*. A
handful of one-purpose configuration apps scattered alphabetically among the
games and the file browser is how you fail to find the one you want — and
each one you add pushes the actual applications further down the list.

So the settings apps come off the desktop and go behind one icon.

---

## Nothing here is hardcoded

`piko-settings` builds its list by scanning `.desktop` files, the same way
the desktop and the panel menu do. **Adding a settings app later means
shipping its `.desktop` file and nothing else** — no edit to
`piko-settings.cxx`, no rebuild of it.

An entry qualifies when it has all of:

```ini
Type=Application
Categories=Settings          # the word the panel menu's Settings vfolder
                             # already matches on
Name=...
Exec=...
```

and it is filed under the heading named by:

```ini
X-Piko-Settings-Group=Display    # missing => "Other"
```

Headings are ordered `Display, Input, Sound, Power, Network, System`, then
anything unrecognised alphabetically, then `Other` last (`GROUP_ORDER[]` in
the source). An app with a group nobody has heard of still appears — at the
end, rather than not at all.

The scan covers the same directories `dotdesktop_init()` does, **including
the SD card** (`/mnt/card/.zaurus/usr/share/applications`), so a settings
app installed onto the card rather than into the ~68 MiB NAND root shows up
too.

`piko-settings` does not list itself: it skips the entry whose `Exec`
basename is its own (`SELF_BINARY` in the source). Matching on the binary
rather than the filename is deliberate — the filename is a packaging
detail, the binary is the identity.

---

## The three consumers, and which key each one reads

This is the part worth getting right, because one `.desktop` file is now
read by three different programs and **each reads a different key**:

| Program | Reads | Effect |
|---|---|---|
| `piko-settings` | `Categories=Settings`, `X-Piko-Settings-Group` | lists the entry, under that heading |
| `piko-settings` | `X-Piko-Settings-Panel` | opens it **inside** its own window instead of launching it |
| `mb-applet-menu-launcher` (matchbox-panel) | `Categories` | files it in the panel menu's "Desktop Preferences" folder |
| `matchbox-desktop` (`modules/dotdesktop.c`) | `X-Piko-NoDesktop` | **skips** the entry — no desktop icon |

So an embeddable settings app carries all four lines:

```ini
Categories=Settings
X-Piko-Settings-Group=Display
X-Piko-Settings-Panel=wallpaper
X-Piko-NoDesktop=true
```

and ends up openable inside the Settings window, launchable from the panel
menu, and absent from the desktop. Drop the `Panel` line and it is still
all of those things except embedded — it gets launched instead.

### `X-Piko-NoDesktop`, and why not `NoDisplay`

`NoDisplay=true` is the freedesktop key that sounds like this and is not: it
means "hide from menus", which would take the panel menu away too. The spec
has nothing for "hide from the desktop, keep in the menu" — an icon view is
not a concept it has — and reserves the `X-` prefix for exactly this sort of
extension, so other implementations are required to ignore it. Same shape as
`X-Piko-Heavy`, which is already read out of these files by the same
function.

The check lives in `add_a_dotdesktop_item()`, right after the existing
`Categories=Action` early-return.

### `piko-settings` itself is the exception

`piko-settings.desktop` carries `Categories=Settings;` and **no**
`X-Piko-NoDesktop`. It is the one Settings entry that *is* a desktop icon —
it is the door to the other two. It also appears in the panel menu's
Desktop Preferences folder alongside them, not instead of them.

### `X-Piko-Heavy` works from here too

`piko-settings` applies the same `matchbox-fbrun` rewrite `dotdesktop.c`
does, so a settings app that needs the framebuffer to itself is launched
correctly from the Settings window and not straight under X, where it would
render nothing and receive no input. Nothing currently needs this; it is
there so that launching from the Settings window and launching from the
desktop cannot diverge.

---

## One panel, two ways in

XFCE's settings manager can run each settings dialog on its own *or* show
it inside the manager window. This does the same thing, and the mechanism
is the interesting part.

**A panel is an `Fl_Group` subclass holding one settings app's entire UI
and logic.** The standalone binary is a thin `main()` that puts it in a
window; `piko-settings` constructs the same class inside its own window
when the row is tapped. `mb-wallpaper-picker.cxx` is now about fifteen
lines; everything that used to be in it lives in
`userspace/src/panels/panel-wallpaper.cxx`.

```
                 panels/panel-wallpaper.cxx     <- the UI and the logic,
                    class WallpaperPanel           written exactly once
                            |
            +---------------+---------------+
            |                               |
  mb-wallpaper-picker.cxx           piko-settings.cxx
  main() { new WallpaperPanel }     PANELS[] { "wallpaper", ... }
  own window, own process           inside the settings window
```

Both binaries compile that same source, so each carries a copy of the
panel's object code. That is the deliberate trade: **one copy of the
source**, at the cost of a few tens of KB of duplicated text — against 68
MB of NAND — and in exchange no `dlopen`, no plugin ABI to keep stable,
and no second process at runtime.

### Why not XEmbed, which is what XFCE actually does

XFCE passes `--socket-id` and the dialog reparents itself into a
`GtkSocket`. It shares code for free, because both modes are the same
binary. We do not do that, for reasons that are specific to this device:

- **FLTK 1.3 implements no part of XEmbed.** There is no `GtkPlug`
  equivalent. The `_XEMBED` client protocol, focus handover and key
  forwarding would all be hand-rolled.
- **It costs a second process per open panel** — another resident
  `libfltk` + `libX11` + `libstdc++` — on a machine with 52 MB of RAM and
  a 400 MHz CPU, to display a widget the host could have drawn itself.

Embedding a widget rather than a window gets the same result for a
constructor call. The one thing XEmbed would buy — a settings app shipped
by an unrelated package, embedding without being compiled in — is not a
case this ROM has.

### Writing a panel

1. Subclass `Fl_Group`. Lay out inside the `(X,Y,W,H)` you are handed and
   never assume it is the whole window — embedded, you sit below
   `piko-settings`' 52px header.
2. Override `resize()` and re-lay-out if the width changed. `flipd`
   rotates this screen live.
3. Expose a `PikoPanelFactory` (see `panels/piko-panel.H`).
4. Add the source to `PANEL_SRCS` in `tools/build-fltk.sh`, a line to
   `PANELS[]` in `piko-settings.cxx`, and `X-Piko-Settings-Panel=<name>`
   to the `.desktop` file.

**An unknown panel name falls back to launching the program.** A
`.desktop` file naming a panel this build does not have — one from a newer
package, say — must never produce a row that does nothing when tapped.

### What cannot be a panel

`pikalibrate` cannot, and no amount of work would change that:

- it is **SDL**, not FLTK — a different toolkit, built by a different
  script (`tools/build-sdl.sh`), linking `libSDL-1.2.so.0`;
- it needs the **whole 640x480 panel**, because it puts its calibration
  crosses at the physical corners. Inside a sub-rectangle the numbers it
  produces would be wrong;
- it **suspends the X server** while it runs — it writes `SUSPEND` to
  `/tmp/.pikalibrate-ctl` and Xfbdev does `KdSuspend()`. A panel cannot be
  hosted inside a window drawn by the server it switches off.

It has no `X-Piko-Settings-Panel` line and is launched. That is not a gap
to be closed later; the "some embed, some launch" split is XFCE's model
too, and this is why the fallback is the default rather than an error.

---

## What is in it today

Two entries, both under **Display**:

| Entry | Group | Tapping it |
|---|---|---|
| Calibrate touchscreen (`pikalibrate`) | Display | **launches** it — see "What cannot be a panel" |
| Set Wallpaper (`mb-wallpaper-picker`) | Display | **opens inside** the Settings window, Back returns to the list |

`pikalibrate` is the only one whose `Categories` changed when the settings
window was introduced (`System` → `Settings`). Recalibrating the panel is a
setting, not a system tool; the only thing lost is the old folder placement
in the panel menu.

Both are still perfectly usable on their own: `mb-wallpaper-picker` is a
real binary and its `.desktop` `Exec=` is untouched, so the panel menu and a
shell reach it exactly as before.

---

## Adding a settings app

1. Write it. If it is a GUI app, FLTK — `docs/HOWTO-FLTK.md`.
2. Add a build block to `tools/build-fltk.sh` (copy the `piko-settings`
   one; it links `-lfltk_images -lpng -lz` ahead of `-lfltk` because it
   decodes icons, and asserts both libraries land in `DT_NEEDED`).
3. Ship the binary: one entry in `BINS` in
   `tools/build-matchbox-payload.sh`. `/usr/local/bin` is correct —
   `/etc/init.d/xsession` puts it first on `PATH` for the graphical
   session, so a bare `Exec=` resolves.
4. Add `userspace/desktop/<app>.desktop` and a 32x32
   `userspace/desktop/<app>.png`, with the keys above.
5. Add the name to `LAUNCHERS` in `tools/build-matchbox-payload.sh`.
6. **If it should also open inside the Settings window**, write it as a
   panel from the start — see "Writing a panel". Retrofitting is only
   moving code out of `main()`, but doing it up front is free.

Step 5 is the one that is easy to forget and fails quietly in the most
confusing direction: the binary ships, the app runs fine from a shell, and
it is in neither the Settings window nor the menu — because
`/usr/share/applications` is what all three consumers read, and nothing put
the file there.

---

## Deliberately not there

**No search box.** XFCE's has one; two entries do not need one, and this
keyboard cannot type many characters at all (see `AGENTS.md`). Worth
revisiting if the list ever outgrows a screen.

**No Close button.** Matchbox draws a titlebar with a close box and this
matches `pikostore` and `mb-wallpaper-picker`, neither of which has one
either.

**The window stays open after launching something.** Matchbox shows one app
at a time, so the launched app covers the Settings window and Home (or the
task menu) comes back to it — which is what you want when changing two
settings in a row.

**Rebuilt, not resized, on rotation.** `flipd` rotates the desktop live when
the lid is folded (`docs/HOWTO-SCREEN-ROTATION.md`). FLTK's proportional
child resizing would scale row heights and icon gutters along with the
width, so `SettingsWindow::resize()` rebuilds the list instead, keeping every
vertical metric fixed. It is a few dozen widgets.

---

## Verification status

- **Cross-compiles clean** for ARM against the staged FLTK — `-Wall
  -Wextra`, no warnings; ELF flags `0x5000200` (Version5 EABI, soft-float);
  `DT_NEEDED` carries both `libfltk.so.1.3` and `libfltk_images.so.1.3`,
  and every library it needs is already in the payload. No new runtime
  dependency is added to the ROM by this app.
- **Rendered on the host**, from this same source, and checked by eye at
  640x480 and 480x640 and with a synthetic 11-entry set: grouping order,
  the alphabetical fallback, `Other` last, the scrollbar appearing only
  when the list overflows, and the missing-icon placeholder. See below for
  how.
- **Driven, not just rendered.** The embedding path was exercised with real
  clicks (XTEST, see below): tap "Set Wallpaper" → the picker appears
  inside the window with the header retitled and Back showing → tap a mode
  button → tap Back → the list returns. Three consecutive runs produced
  byte-identical captures at every step, so it is deterministic rather than
  lucky.
- **The focus indicator draws.** Previously flagged as unconfirmed; the
  capture after Back shows the focus rectangle on the row that was tapped.
- **The unknown-panel fallback works, and was confirmed by accident** — a
  stale `.desktop` without `X-Piko-Settings-Panel` in the test sysroot made
  the row launch `mb-wallpaper-picker` as a process instead of embedding
  it, which is exactly the intended behaviour for an entry whose panel this
  build does not have.
- **Not yet run on real hardware, and not yet run as an ARM binary.** On
  this project that distinction is never pedantry:
  `docs/DEADLETTER-AUDIO-I2S-SILENT.md` is the write-up of a sound card
  that registered perfectly and played nothing.

### Rendering it on the host for layout work

Cross-building proves it links; it does not show you the layout. The cheap
way to see the layout is to build **the same `.cxx`** natively against a
host build of the *same pinned FLTK* (`userspace/src/fltk`, `release-1.3.11`
— `git archive HEAD` it into a scratch directory and `./configure` there, so
the cross build's artefacts are untouched), then run it with only the ROM's
launchers visible:

```sh
# a full X screen inside one window, so the capture holds only this app
WAYLAND_DISPLAY=wayland-1 Xwayland :7 -geometry 640x480 -retro &

unshare -r -m sh -c '
  mount --bind '"$SYSROOT"'/usr/share/applications /usr/share/applications
  mount --bind '"$SYSROOT"'/usr/share/pixmaps      /usr/share/pixmaps
  DISPLAY=:7 exec ./piko-settings-host'

DISPLAY=:7 import -window root shot.png
```

The mount namespace is the point: without it the scan picks up the host's
own `Categories=Settings` entries and you are looking at the wrong list.
Bind `usr/share/backgrounds` too, or the wallpaper panel opens on an empty
grid. A host `configure` will also enable Xinerama/Xfixes/Xcursor that the
device build does not have, so add `-lXinerama -lXfixes -lXcursor` when
linking.

### Driving it, to test the embedding

Layout you can see in a screenshot; embedding you have to *do*. There is no
`xdotool` here, but XTEST is enough — about 20 lines against `libXtst`:
`XWarpPointer` to a coordinate, then `XTestFakeButtonEvent` press and
release. That drives the row tap and the Back tap, capturing between each.

Two things to know before believing a result:

- **Compare capture hashes, not sizes.** The list before and after a visit
  to a panel differ only by the focus rectangle left on the tapped row.
- **It is flaky at the first click.** One run in four or so, the first
  faked click after the window maps does not register and the flow silently
  does nothing. That is the harness, not the app — three back-to-back runs
  of the identical sequence produced identical captures at every step. If a
  single run "fails", repeat it before believing it.

**`qemu-arm` was tried first and is not worth it.** The ARM binary loads and
stays alive under `qemu-arm -L $SYSROOT` (qemu-user resolves `open()`
through the `-L` prefix as well as the ELF interpreter, so the scanned
`/usr/share/applications` is the sysroot's), but it never got as far as
mapping a window within 2 minutes — fontconfig start-up under emulation is
where it goes. Two traps if you try anyway: copy the libraries with
`cp -aL`, not `cp -a`, or a preserved-but-dangling symlink fails as
`Could not open '/lib/ld-uClibc.so.0'`; and the sysroot needs `etc/fonts`
plus the DejaVu faces or FLTK aborts with `Unable to find fonts`, exactly
as it would on a device with no fonts installed.
