# The Matchbox desktop: building and deploying it

*Written 2026-07-31, the day the Zaurus first booted to a real graphical
desktop: themed window manager, app-folder desktop and panel, driven by
a working keyboard and touchscreen.*

This covers the desktop **on top of** X. If X itself or the touchscreen
misbehaves, that is a different layer -- see
`docs/HOWTO-X11-TOUCHSCREEN.md`.

For the panel's applets specifically -- what exists, the four upstream bugs
that had to be fixed, and why a two-applet panel is the default rather than
a fault -- see `docs/HOWTO-MATCHBOX-PANEL-APPLETS.md`.

---

## Why classic Matchbox and not matchbox-desktop-2

`matchbox-desktop-2` needs a full GTK+ stack: `gtk+-3.0` on master,
`gtk+-2.0` on its `gtk2` tag. That is ~18-20 cross-compiled packages
(glib, pango, cairo, gdk-pixbuf, atk, harfbuzz and eight more X libs)
before a single pixel appears, and GTK3's rendering path is a poor fit
for an unaccelerated 400MHz PXA255 with 64MB of RAM.

The classic Matchbox 0.9 suite needs exactly **one** library --
libmatchbox -- which itself needs only `x11` and `xext`:

    matchbox-desktop  -> libmb >= 1.5     (and nothing else)
    matchbox-panel    -> libmb >= 1.6     (and nothing else)
    matchbox-common   -> libmb >= 1.1     (and nothing else)

With libmatchbox's defaults (Xft on, PNG on) the real cost is six small
libraries. libmatchbox is also still maintained -- 1.14 is from 2025.

The submodule for `matchbox-desktop-2` is kept at
`userspace/src/matchbox-desktop` in case the GTK path is ever wanted.
Nothing builds it. The classic one is `matchbox-desktop-classic`.

---

## Build order

    tools/build-thirdparty-deps.sh      # zlib expat libpng freetype
                                        # fontconfig + DejaVu faces
    tools/setup-x11-src.sh              # verify submodules carry our commits
    tools/build-libiw.sh                # libiw, for mb-applet-wireless
    # then, in userspace/src/: libXrender, libXft, libmatchbox,
    # matchbox-window-manager, matchbox-desktop-classic,
    # matchbox-panel, matchbox-common

The last four are independent of each other once libmatchbox exists and
can be built in parallel -- but give each its own `DESTDIR`, because they
would otherwise race installing into one tree.

Common environment for every component:

    TC=<repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
    STAGE=<repo>/userspace/stage-target
    HOST=arm-unknown-linux-uclibcgnueabi
    export PATH="$TC:$PATH" CC="${HOST}-gcc" AR="${HOST}-ar" \
           RANLIB="${HOST}-ranlib" STRIP="${HOST}-strip"
    export PKG_CONFIG_SYSROOT_DIR="$STAGE"
    export PKG_CONFIG_LIBDIR="$STAGE/usr/lib/pkgconfig:$STAGE/usr/share/pkgconfig:/usr/share/pkgconfig"
    export PKG_CONFIG_PATH=
    export CPPFLAGS="-I$STAGE/usr/include"
    export LDFLAGS="-L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib"

`-rpath-link` is not optional: without it the cross-linker cannot resolve
*indirect* dependencies and fails with e.g. "libz.so.1, needed by
libfreetype.so, not found" even though `-L` points straight at it.

### Per-component configure lines

**libXrender** must be **0.9.7**, not current. 0.9.11+ requires
`x11 >= 1.6` and libX11 here is pinned at 1.4.4; 0.9.7 (2012) is the
contemporary release that asks only for plain `x11`.

**matchbox-window-manager** -- do **not** pass `--enable-standalone`.
That was needed before libmatchbox existed, and its own configure warns
it "does not support theming. It will be ugly."

    ./configure --host=$HOST --build=x86_64-pc-linux-gnu --prefix=/usr \
        --x-includes="$STAGE/usr/include" --x-libraries="$STAGE/usr/lib" \
        --disable-composite --disable-startup-notification \
        --disable-gconf --disable-session

Confirm the summary says `Building Standalone: no`. `--x-includes` /
`--x-libraries` are required because this package finds X with the old
`AC_PATH_X` macro (hardcoded search paths), not pkg-config.

**matchbox-desktop-classic** -- two non-obvious flags:

    ./configure --host=$HOST --build=x86_64-pc-linux-gnu \
        --prefix=/usr --sysconfdir=/etc --disable-static
    make CPPFLAGS="-I$STAGE/usr/include -I$STAGE/usr/include/libmb -DUSE_XSETTINGS"

- `--sysconfdir=/etc`, or `MBCONFDIR` is baked in as `/usr/etc/matchbox`.
- **`-DUSE_XSETTINGS` matters.** configure decides XSettings support by
  grepping `pkg-config --libs libmb` for the string "xsettings", which
  `libmb.pc` does not contain -- so it silently auto-disables. The
  detection is simply wrong: libmatchbox compiles `xsettings-client.c`
  unconditionally and `libmb.so.1` exports the symbols. With XSettings
  off, `mb->theme_name` stays NULL forever and there is no `--theme`
  option, i.e. **matchbox-desktop can never pick up a theme or font at
  all.** The extra `-I.../include/libmb` is needed because `mbdesktop.h`
  includes `<xsettings-client.h>` unqualified while libmb installs it
  under `include/libmb/`.

**matchbox-panel** needs `--enable-proc-apm`, or you get no battery
applet (and `tools/build-libiw.sh` run first, or no wireless one):

    ./configure --host=$HOST --build=x86_64-pc-linux-gnu --prefix=/usr \
        --enable-proc-apm

Confirm the summary says `Building mb-applet-battery: yes, with
/proc/apm`. That flag is ours (a commit on the `matchbox-panel` fork,
`github.com/sugarkrap/matchbox-panel`) -- see "Panel applets" below for why
neither upstream backend works here.

**matchbox-common** needs no special flags. It is
architecture-independent -- no `.c` files anywhere, no ELF output;
`--host` only makes configure complete.

Consider `--enable-pda-folders` for matchbox-common: it swaps the
11-folder desktop menu layout for a 5-folder handheld one, which suits a
640x480 clamshell. Two upstream bugs in that variant if you use it:
`vfolders-pda/Root.directory` carries a stray `Match=PIM`, and
`Applications.directory` is installed but absent from `Root.order` and
has no `Match=`, so it is inert.

---

## Deploying

    tools/build-matchbox-payload.sh --deploy --adapter wlan0 root@<ip>

It collects the four `DESTDIR`s plus the libraries and fonts, strips
everything, drops `.la`/headers/`.pc`, verifies every `DT_NEEDED` is
satisfied and every ELF is ARM, then ships **one tar** and unpacks it on
the device with our own `untar` (`userspace/src/untar.c`). One archive
rather than ~100 transfers because the link is flaky, and `untar` because
the device's busybox has no `tar`.

Stripping and pruning takes the payload from **13MB to 3.5MB**.

Start it with:

    DISPLAY=:0 matchbox-session &

`matchbox-session` (from matchbox-common) prefers
`$HOME/.matchbox/session`, then `/etc/matchbox/session`, then its own
built-in trio of `matchbox-desktop`, `matchbox-panel --orientation south`
and `matchbox-window-manager`. The payload ships
`modules/x11/matchbox-session` as `/etc/matchbox/session`, so the applet
list is tracked source -- see `docs/HOWTO-MATCHBOX-PANEL-APPLETS.md`.

---

## Panel applets

Full detail -- what exists, the four upstream bugs, and why a two-applet
panel is the default -- lives in `docs/HOWTO-MATCHBOX-PANEL-APPLETS.md`.
What matters for *building*:

- `matchbox-panel` needs `--enable-proc-apm` or there is no battery applet.
  Confirm `Building mb-applet-battery: yes, with /proc/apm`.
- `tools/build-libiw.sh` must run before configuring, or there is no
  wireless applet. Confirm `Building mb-applet-wireless: yes`.
- The applet list is `modules/x11/matchbox-session`, shipped to
  `/etc/matchbox/session`. `build-matchbox-payload.sh` fails if it names an
  applet that is not in the payload.
- Do not drop `--no-session` from that file. A stale
  `$HOME/.matchbox/mbdock.session` silently overrides `--default-apps`, and
  the panel rewrites it on every clean exit.

---

## Fonts are mandatory

The device ships with **no fonts and no `/etc/fonts`**. Matchbox themes
ask for `"Sans bold 16px"` -- a generic fontconfig family -- so with
nothing installed every themed widget renders blank. This is also the
real reason Xfbdev logs `Could not init font path element`.

`tools/build-thirdparty-deps.sh` installs DejaVu Sans regular + bold
(~1.4MB) into `/usr/share/fonts/truetype/dejavu`, already inside the
`<dir>` that `fonts.conf` searches. The full DejaVu family is ~10MB and
the rest is never referenced.

fontconfig must be configured with `--sysconfdir=/etc
--localstatedir=/var`. With a bare `--prefix=/usr`, autoconf derives
those as `/usr/etc` and `/usr/var`, and fontconfig bakes in
`/usr/etc/fonts` as its config location.

Expect the **first** launch to stall while fontconfig builds its cache;
`fc-cache` is not shipped, so it happens in-process.

---

## Known rough edges

- **matchbox-desktop hard-depends on matchbox-common, fatally.**
  `mbdesktop_set_scroll_buttons()` calls `exit(1)` with "is
  matchbox-common installed ? Cannot continue." if `mbup.png`/`mbdown.png`
  are missing, and `dotdesktop.so` fails to load without
  `/usr/share/matchbox/vfolders/Root.directory` -- which is the module
  that draws the app icons, so you get an empty desktop.
- **matchbox-common ships no `.desktop` launchers**, only vfolder
  *category* definitions that sort other packages' entries by
  `Categories=`. An empty-looking menu is expected until real apps are
  installed.
- `Root.directory` references `gnome-folder.png`, which nothing ships;
  that one folder icon fails to load. One-line fix to `mbfolder.png`.
- **Latent segfault:** `mb-applet-system-monitor` (and the battery
  applet) call `mb_tray_app_new()` and use the result with no NULL check,
  unlike clock/launcher. If the panel starts applets before X is ready
  they crash. One-line fix each.
- **The battery applet is buildable with no new libraries.** It only
  wants libapm because upstream's `read_apm()` calls `apm_read()`; this
  kernel has `CONFIG_APM_EMULATION=y` so `/proc/apm` exists and can be
  read directly. Not currently applied.
- Panel applets built: clock, launcher, menu-launcher, system-monitor.
  Not built: battery (no `apm.h`), wireless (no `iwlib.h`). Note
  `configure.ac`'s `AC_CHECK_LIB(iw, ..., yes, yes)` sets *yes* in both
  branches -- harmless only because the `iwlib.h` check also has to pass.
