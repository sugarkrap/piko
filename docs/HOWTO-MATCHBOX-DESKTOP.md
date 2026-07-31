# The Matchbox desktop: building and deploying it

*Written 2026-07-31, the day the Zaurus first booted to a real graphical
desktop: themed window manager, app-folder desktop and panel, driven by
a working keyboard and touchscreen.*

This covers the desktop **on top of** X. If X itself or the touchscreen
misbehaves, that is a different layer -- see
`docs/HOWTO-X11-TOUCHSCREEN.md`.

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

As of 2026-07-31 this is automated end to end:

    tools/build-thirdparty-deps.sh      # zlib expat libpng freetype
                                        # fontconfig + DejaVu faces
    tools/build-x11-stack.sh            # everything below, in order

`tools/build-x11-stack.sh` applies `tools/setup-x11-src.sh`'s patches
itself, then builds xtrans through xserver/xkbcomp/xev, then libmatchbox
and the four Matchbox apps, idempotently (skips anything already built --
safe to re-run, and cheap once everything exists). It is wired into
`tools/build-and-deploy.sh` (the live-SSH redeploy path) and
`flash/build-mtd3-jffs2.sh` (the SD-card flash-image path), so under
normal use nothing below needs to be run by hand at all.

**What follows is the reference this script was built from and is
verified against** -- read it when a single component needs debugging,
not as a competing set of instructions. If this document and the script
ever disagree about what a component needs, that is a bug in the script,
not a reason to trust it over this.

    tools/setup-x11-src.sh              # local patches into X submodules
    tools/build-libiw.sh                # libiw, for mb-applet-wireless
    # then, in userspace/src/: libXrender, libXft, libmatchbox,
    # matchbox-window-manager, matchbox-desktop-classic,
    # matchbox-panel, matchbox-common

The last four are independent of each other once libmatchbox exists and
*could* be built in parallel -- `tools/build-x11-stack.sh` builds them
sequentially for determinism, but give each its own `DESTDIR` regardless,
because they would otherwise race installing into one tree.

One thing worth knowing if you ever touch the automation itself: X.Org's
per-component `autogen.sh` scripts are not uniform. Roughly half honour
`NOCONFIGURE=1` and skip their own `configure` call; the rest
(`libX11`, `xserver`, `libXext`, `libXrender`, every `matchbox-*`) run
`configure` themselves regardless of `NOCONFIGURE`. Both families forward
`"$@"` to that internal call, though, so `build-x11-stack.sh` passes the
real cross-compile flags straight through `autogen.sh` itself on a
from-scratch run rather than trying to separate "generate configure" from
"run configure" into two steps -- the first version of the script did
try that, and it silently dropped `--host` for every component in the
second family, configuring them *native* instead of cross.

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
/proc/apm`. That flag is ours (`modules/x11/matchbox-panel-battery-proc-apm.patch`,
applied by `tools/setup-x11-src.sh`) -- see "Panel applets" below for why
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

Two ways to get the desktop onto the device, not just one anymore:

- **A device that's already running:** live over SSH --

      tools/build-matchbox-payload.sh --deploy --adapter wlan0 root@<ip>

- **A device being flashed from scratch:** `flash/build-mtd3-jffs2.sh`
  now stages this same payload straight into the image it builds, so
  `piko.zip` (see `.github/workflows/build-piko-zip.yml`) boots to the
  desktop on the very first boot. No separate deploy step needed
  afterward.

`build-matchbox-payload.sh` collects the four `DESTDIR`s plus the libraries and fonts, strips
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
list is tracked source -- see below.

---

## Panel applets

`matchbox-panel` does not contain its applets; each is a separate binary
that docks itself into the panel over XEMBED. The panel starts them, and
**which** it starts is the part that bites.

The tree has six applets. Four build with no extra work:

| Applet | Notes |
|---|---|
| `mb-applet-menu-launcher` | the app menu |
| `mb-applet-clock` | clock |
| `mb-applet-system-monitor` | CPU + memory bars |
| `mb-applet-launcher` | generic launcher button; instantiate once per app as `-l <icon.png> <command>` |

`mb-applet-battery` needs `--enable-proc-apm` (see above). Upstream offers
two backends and this board can use neither: `HAVE_APM_H` wants `apm.h`
plus `-lapm` from Debian's apmd, which we do not cross-build, and
`USE_ACPI_LINUX` reads `/proc/acpi`, which does not exist here. Without
the flag, configure just drops the applet from `bin_PROGRAMS` and says
`Building mb-applet-battery: no ( enable ACPI? )` -- easy to miss. The
same patch fixes the `.desktop` install, which upstream gates on
`WANT_APM` alone, so the ACPI backend never installed a menu entry
either.

Expect a percentage and no time estimate: the provider is `sharpsl_pm`,
whose `apm_get_power_status` fills in AC status, battery status/flag and
percentage but leaves `time`/`units` at the kernel's `-1` and `"?"`.

`mb-applet-wireless` needs libiw in the staging tree first:

    tools/build-libiw.sh

That cross-builds the one source file out of the already-vendored
`userspace/wireless_tools.29` and installs `libiw.a` + `iwlib.h` +
`wireless.h` into `userspace/stage-target`. Three things about it are not
obvious:

- It does **not** reuse the `libiw.a` sitting in that vendored tree. Those
  ARM binaries were built in July 2026 with a different toolchain and
  statically linked; mixing objects from another libc into a uclibc link
  invites subtle breakage.
- It builds against wireless-tools' **own** `wireless.h` (WE21), not the
  sysroot's `linux/wireless.h` (WE22). `iwlib.c` uses the `IW_MODUL_*`
  modulation constants, which are a wireless-tools addition the kernel uapi
  header has never carried — against the sysroot copy the build dies on
  ~15 undeclared identifiers. The version gap is harmless: the 32-bit ioctl
  structs are unchanged, iwlib re-checks `we_version_compiled` at runtime,
  and the `iwconfig` already on the device was built from this same header
  against this same kernel.
- It compiles `-DWE_NOLIBM`, which swaps libiw's two frequency helpers off
  `floor`/`log10`/`pow`. Otherwise libiw drags in libm.

Then configure must say `Building mb-applet-wireless: yes`. The applet
links `libiw` statically and gains **no** new `DT_NEEDED` — `-lm` resolves
inside libc on this uclibc toolchain, which ships only a static `libm.a`.

The applet needed three fixes of its own before it would build or run at
all (`modules/x11/matchbox-panel-wireless-applet.patch`) — see that patch
and `setup-x11-src.sh`. The headline one: `find_iwface()` passed
`Mwd.iface`, assigned only at the end of that same function, so on the
first wireless interface it was still NULL and iwlib's
`strncpy(ifr_name, NULL, IFNAMSIZ)` segfaulted. Upstream crashes during
startup enumeration on any machine that actually has a wireless
interface.

`mb-launcher-term.desktop` is installed but not started -- its wrapper
execs `rxvt` or `xterm` and the payload ships neither.

### Two applets is the default, not a bug

If the panel only shows a menu and a clock, nothing is broken. With no
`--default-apps`, `matchbox-panel` uses its compiled-in `DEFAULT_SESSIONS`
= `"mb-applet-menu-launcher,mb-applet-clock"` (`src/session.c:3`). The
other applets are installed and working, just never launched.

`/etc/matchbox/session` passes the list we want. It also passes
**`--no-session`, which is not optional**: if `$HOME/.matchbox/mbdock.session`
exists, the panel reads it and ignores `--default-apps` entirely
(`src/session.c:73-99`) -- and the panel *writes* that file on every clean
exit. So any device that has ever run the panel has a stale two-applet
list on disk that would silently win. `--no-session` also stops it being
rewritten, which is what we want on an appliance.

All `--default-apps` entries dock at the same gravity (the right of a
south-oriented panel) in list order. Per-applet left/right placement needs
the `mbdock.session` file format instead, which is not worth
reintroducing the stale-state problem for.

`tools/build-matchbox-payload.sh` fails if the session file names an
applet that is not in the payload -- otherwise the panel just logs a
session timeout per missing one and comes up looking half-broken.

---

## Wallpaper: modes, formats, and why it's cached raw

`matchbox-desktop` already had a background system (`--bg`, XSettings,
theme `DesktopBgSpec=`); this extends it rather than replacing it.

**Modes**, passed as `mode:filename` (or via the picker, see below):

    img-mosaic:<filename>       tiles the image (alias: img-tiled:)
    img-centered:<filename>     centers it, no scaling
    img-stretched:<filename>    scales to fill, distorts aspect ratio
    img-filled:<filename>       scales to cover, crops overflow,
                                 keeps aspect ratio (alias: img-fill:)

**Formats**: PNG, JPEG (if libmb was built `USE_JPG` -- the payload
here is not, see below), XPM, and now **BMP** -- uncompressed 24/32bpp
and 8bpp-paletted, decoded by a small loader added directly to
`libmatchbox/libmb/mbpixbuf.c` with no new library dependency. This
matters more than it looks: this build's `libmb.so.1` is linked
against `libpng16` only, not libjpeg, so BMP is the easiest format to
hand this device a wallpaper in without cross-building libjpeg too.

**Why the result is cached, not just the source image.** Decoding a
JPEG/PNG and scaling it to 480x640 is real CPU on an unaccelerated
PXA255, and the original code paid that cost on *every launch* --
including every boot, for a wallpaper that never changes between
boots. `mbdesktop_view_init_bg()` now bakes the fully composited,
already-scaled/cropped/tiled, already-pixel-format-converted result
(RGB565 on this display) to `~/.matchbox/wallpaper.cache` as a raw
byte dump the first time a wallpaper is resolved, and every later
launch is just an `fread()` straight into the image buffer -- no
decode, no scale, no color conversion. The cache is invalidated
automatically if the desktop size, the mode+filename, or the source
file's mtime changes; the write is temp-file-then-`rename()` so a
power loss mid-boot (this device is fragile) can't leave a half
written cache behind. Solid colors and gradients are pure in-memory
math already and are never cached.

**Setting it.** `mb-wallpaper-picker` is a small standalone touch app
(built alongside `matchbox-desktop`, same `libmb` dependency, nothing
extra) that scans `/usr/share/backgrounds` and
`$HOME/.matchbox/backgrounds` for `.png`/`.jpg`/`.jpeg`/`.bmp` files,
shows a thumbnail grid with a 4-way mode selector, and on tap:
writes `$HOME/.matchbox/wallpaper` (the file `matchbox-desktop` reads
at startup, so the choice survives a reboot) and sets the
`_MB_WALLPAPER_SPEC` property on the root window so an *already
running* desktop updates immediately. The property exists because
this device's busybox has no `kill`/`killall`/`pkill` at all -- there
is no way to signal the running process, so an X property it already
watches via `PropertyNotify` (the same mechanism `_MB_THEME_NAME`
already used for live theme switches) is the only avenue. It ships a
`.desktop` launcher with `Categories=Settings`, so it shows up in the
desktop's Settings folder automatically via matchbox-common's
vfolders -- no wiring needed beyond installing the file.

Precedence at startup: `--bg` on the command line wins outright;
otherwise a live `_MB_WALLPAPER_SPEC` root property (set by the
picker in the current session) beats the persisted
`$HOME/.matchbox/wallpaper` file, which beats the theme's
`DesktopBgSpec=` default.

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
