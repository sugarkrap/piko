# The Matchbox panel and its applets

*Written 2026-07-31, after taking the panel from two applets to five and
fixing four upstream bugs to get there. Companion to
`docs/HOWTO-MATCHBOX-DESKTOP.md`, which covers building the desktop as a
whole; this file is only about the panel and the things that dock into it.*

The short version: **`matchbox-panel` showing only a clock and a menu is
not a fault.** That is its compiled-in default. Everything else here is
about what exists, what it took to make each piece work, and which
"failures" were really the panel telling the truth.

---

## What actually exists

`matchbox-panel` contains no applets. Each is a separate binary that docks
itself into the panel over XEMBED; the panel launches them and they draw
their own tray icon. The tree ships six plus a wrapper script:

| Applet | State | Notes |
|---|---|---|
| `mb-applet-menu-launcher` | shipped | the app menu |
| `mb-applet-clock` | shipped | clock |
| `mb-applet-system-monitor` | shipped | CPU + memory bars |
| `mb-applet-battery` | shipped | needed a new backend, see below |
| `mb-applet-wireless` | shipped | needed three fixes to even run |
| `mb-applet-tasks` | shipped | the taskbar, ours -- see below |
| `mb-applet-launcher` | built, unused | generic button: `-l <icon.png> <command>` |
| `mb-applet-xterm-wrapper.sh` | installed, unused | execs `rxvt` or `xterm` |

Plus one of our own, in its own repo rather than in matchbox-panel:

| Applet | Notes |
|---|---|
| `mb-applet-card` | SD/CF eject, like XP's *Safely Remove Hardware*. Submodule `userspace/src/mb-applet-card`, from `github.com/sugarkrap/mb-applet-card`. Self-hiding: the icon appears only while a card is inserted. Built by `tools/build-x11-stack.sh` like every other Matchbox app, into `D_CARD` (`/tmp/mb-stage-card`) -- but with plain `make`, not autotools: it is one source file against one `pkg-config` module and ships a hand-written `Makefile`, so it is the one package in that script with no `configure` step. See that repo's README. |

Until 2026-08-01 `mb-applet-card` was *not* in `build-x11-stack.sh`'s
package list, even though `tools/build-matchbox-payload.sh` requires
`D_CARD` to exist and the session file below names the applet. Nobody's
build had that DESTDIR unless they had built the repo by hand once, so the
payload step failed with `missing component DESTDIR: /tmp/mb-stage-card`
on every automated run -- and because `tools/build-and-deploy.sh` treats a
payload failure as non-fatal, the visible symptom was a deploy that
quietly shipped no desktop at all rather than an error.

`mb-launcher-term.desktop` wires the last two together. It is installed but
**deliberately not started**: the wrapper execs `rxvt` or `xterm` and the
payload ships neither, so it would be a dead button. Add it to the session
once a terminal is in the image.

---

## Why only two applets loaded

Two independent causes, and neither was a bug in what we deployed.

`matchbox-session` (from matchbox-common) prefers `$HOME/.matchbox/session`,
then `/etc/matchbox/session`, and otherwise falls back to a built-in trio
that starts `matchbox-panel` **with no arguments**. With no
`--default-apps`, the panel uses its compiled-in `DEFAULT_SESSIONS`, which
is literally:

    #define DEFAULT_SESSIONS "mb-applet-menu-launcher,mb-applet-clock"

(`src/session.c:3`). `mb-applet-system-monitor` was installed and working
the whole time and simply never launched.

The fix is `modules/x11/matchbox-session`, shipped to
`/etc/matchbox/session` by `tools/build-matchbox-payload.sh`. It names the
applets we want, so the list is tracked source rather than a compiled-in
constant.

### `--no-session` in that file is load-bearing

Do not remove it. If `$HOME/.matchbox/mbdock.session` exists, the panel
reads that file and **ignores `--default-apps` entirely**
(`src/session.c:73-99`) -- and the panel *writes* that file on every clean
exit. So any device that has ever run the panel has a stale two-applet list
on disk that silently wins over whatever the session file asks for.
`--no-session` also stops it being rewritten, which is what you want on an
appliance: the applet list is identical on every boot.

Consequence worth knowing: all `--default-apps` entries dock at the same
gravity (the right of a south-oriented panel), in list order. Per-applet
left/right placement needs the `mbdock.session` format instead, which is not
worth reintroducing the stale-state problem for.

`build-matchbox-payload.sh` fails the build if the session file names an
applet that is not in the payload. Without that check the panel logs a
session timeout per missing applet and comes up looking half-broken.

---

## mb-applet-tasks: the taskbar

*Added 2026-08-01, replacing the "Active Tasks" folder in
matchbox-desktop.* One button per running **application** -- not per
window -- with its icon, its name, and a count when it has more than one
window open. Classic Windows / GNOME 2 behaviour:

- click an application that is not on screen -> raise it;
- click the one that *is* on screen -> minimise it;
- click an application with several windows -> a menu of its windows,
  each by title, minimised ones in `[brackets]`;
- more applications than fit -> the tail goes behind a `>>` button rather
  than being silently dropped.

It lives in `matchbox-panel/applets/` rather than in a repo of its own
(the way `mb-applet-card` does) because it is generic panel
functionality, not piko hardware support -- so it builds, installs and
lands in the payload with the rest of the panel, with nothing new to
wire up.

### It does not need matchbox-desktop, and does not need a patched WM

Everything it reads is plain EWMH on the root window: `_NET_CLIENT_LIST`,
`_NET_ACTIVE_WINDOW`, plus per-window `_NET_WM_NAME`, `_NET_WM_STATE`,
`_NET_WM_ICON`, `WM_CLASS` and `_NET_WM_PID`. It drives the window
manager back with `_NET_ACTIVE_WINDOW` and `_NET_WM_STATE` client
messages. Against an unpatched matchbox the minimise half simply does
nothing and everything else still works.

Two matchbox-specific touches, both optional:

- `_MB_CURRENT_APP_WINDOW` is preferred over `_NET_ACTIVE_WINDOW` for
  deciding which button is pressed in. Under matchbox the active window
  is the *dialog* whenever an application has one open, and a dialog is
  never in the task list -- so without this no button lights up while a
  file chooser is on screen.
- `_NET_WM_STATE_HIDDEN` is what minimise sends, and matchbox-window-manager
  gained support for it for this (see below).

### Identity comes from /proc, not WM_CLASS

The grouping key is the basename of `/proc/<pid>/cmdline`'s argv[0],
found via `_NET_WM_PID`, and only then WM_CLASS.

That order is not stylistic. **Every FLTK program reports
`WM_CLASS = "FLTK", "FLTK"`** unless it has been told otherwise, so
grouping on WM_CLASS folds `pikalibrate`, `matchbox-fbrun` and every
future FLTK tool into one button labelled "FLTK". Verified on a live
display: the applet reported `key=piko-designer` for a window whose
WM_CLASS was `FLTK`/`FLTK`.

argv[0] is also exactly what `.desktop` `Exec=` names, so the same key
does double duty as the icon and display-name lookup -- which is why a
taskbar button shows the same icon and name as the launcher menu entry
it was started from. Falling back:

1. `.desktop` `Name=` / `Icon=` for that binary;
2. WM_CLASS `res_class`, but *only* when it is plainly the same word as
   the key (a prefix either way) -- `steam` for `steamwebhelper` yes,
   `FLTK` for `piko-designer` no;
3. the key itself;

and for the icon, `_NET_WM_ICON`, then the `WM_HINTS` icon pixmap, then
`mbnoapp.png` from matchbox-common.

### Do NOT make it self-hiding

This is the trap. `mb-applet-card` hides itself when there is no card,
and copying that pattern here was a mistake worth recording.

The panel starts its applets **one at a time and waits for each to dock**
before starting the next, giving up after `SESSION_TIMEOUT` (10 seconds,
`src/panel.h`). An applet that calls `mb_tray_app_hide()` before the main
loop never docks at all -- `_init_docking()` early-returns when
`is_hidden` is set -- so it burns that entire timeout and delays every
applet listed after it. Nothing is open at login, which is exactly when
this bites: measured on the test rig, the clock and everything to its
right appeared **10 seconds late on every boot**, with
`Session timeout on mb-applet-tasks` in the panel's stderr.

An empty taskbar asks for a 1px width instead, which disappears between
the panel's own margins.

**The same cost applies to `mb-applet-card` today** whenever no card is
inserted. Nobody has complained, but that is where a mystery 10-second
pause at boot would be coming from.

### How it knows how much room it has

The panel reparents every applet into itself and publishes the docked set
as `_NET_CLIENT_LIST` **on its own window** (`panel_update_client_list_prop`,
`src/panel.c`). The taskbar reads that, takes the geometry of the nearest
applet to its right, and sizes itself to the gap. So the applet list in
`/etc/matchbox/session` can change freely without anything here needing
adjustment.

Asking for more than that does not fail -- the panel just lets applets
overlap -- so getting it wrong would be silent.

Two panel behaviours to know before touching the sizing:

- **Never request width == height.** `panel_app_handle_configure_request`
  reads a square request as "this applet wants to be square" and
  overrides the width with the panel height.
- A resize is a *request*. `layout()` sizes buttons for the width asked
  for; the repaint that follows runs against the width granted **so
  far**. Painting a full row of buttons onto the 1px background of an
  applet the panel has not resized yet overran the heap, because libmb's
  compositor does not bounds-check its destination (`fill_rect()` in the
  applet does). Everything drawn is now clipped to the image actually in
  hand.

### `--dump`

`mb-applet-tasks --dump` scans once, prints the `.desktop` index, the
task list, the groups, their labels, computed geometry and whether an
icon was found, and exits. It needs a display connection but not a
system tray, so it runs alongside a live panel.

There is no debugger on the target and the panel swallows applet stderr,
so this is the only way to answer "what does it think is running, and why
did it pick that icon" without a camera pointed at the screen.

### What the window manager had to gain: minimise

matchbox could *already* iconise a window -- `main_client_iconize()` sets
`IconicState`, unmaps, and takes the client's dialogs with it -- but the
only way to reach it was the titlebar's minimise button, which most
themes do not draw, and nothing ever said so on the wire. Three gaps,
all closed on the fork:

- a `_NET_WM_STATE` client message adding or removing
  `_NET_WM_STATE_HIDDEN` now iconises or restores the window. Restoring
  goes through `wm_activate_client()`, not a flag clear: `main_client_show()`
  is what resets `IconicState`, remaps the frame and brings the dialogs
  back with it;
- iconised app clients carry that state on `_NET_WM_STATE`. **This is the
  only way a taskbar can tell a minimised window from one merely stacked
  below the visible app** -- under matchbox every main client but the top
  one is covered yet still mapped, so map state says nothing;
- `_NET_WM_STATE_HIDDEN` and `_NET_WM_ACTION_MINIMIZE` are advertised in
  `_NET_SUPPORTED` / `_NET_WM_ALLOWED_ACTIONS`.

`main_client_iconize()` also updates the root window lists
unconditionally now. It only reached them via `main_client_unmap()`'s
activation of a next client, so minimising the *last* window left the
lists still claiming it was up.

Deliberately **not** done: making "show desktop" work without a desktop
client. `wm_toggle_desktop()` returns early when `wm_get_desktop()` finds
nothing, so once matchbox-desktop is dropped that path dies -- but the
taskbar does not use it. Per-window `_NET_WM_STATE_HIDDEN` is both the
standard mechanism and the one that survives the deprecation.

One unrelated fix rode along: `_NET_CLIENT_LIST` was published with the
*stack's* item count over the *age list's* contents. The two are
maintained separately, so the moment they disagree that hands out
uninitialised heap as window IDs -- to exactly the pagers and taskbars
that read it.

---

## mb-applet-battery: neither upstream backend works here

The applet has two, and this board can use neither:

- `HAVE_APM_H` wants `apm.h` and `-lapm` from Debian's apmd, which a
  cross-build does not have.
- `USE_ACPI_LINUX` reads `/proc/acpi`, which this hardware does not have.

configure therefore printed `Building mb-applet-battery: no ( enable
ACPI? )` and dropped the applet from `bin_PROGRAMS`. Easy to miss in a long
configure summary, and the reason there was no battery binary anywhere in
the payload.

The kernel is built `CONFIG_APM_EMULATION=y` + `CONFIG_SHARPSL_PM=y`, so
`/proc/apm` carries real charge state. `--enable-proc-apm` (ours, a commit
on the fork) parses that file directly and needs no new library -- the link
line gained nothing.

Two details that will bite if forgotten:

**`TIME_LEFT` is in MINUTES** throughout this applet. The tooltip prints
`TIME_LEFT/60` as hours and `TIME_LEFT%60` as minutes, and
`time_left_alerts[]` is `{ 0, 2, 5, 10, 20 }`. A `/proc/apm` reporting
seconds must be scaled **down**, not up.

**Expect no time estimate.** `sharpsl_apm_get_power_status()` fills in
AC status, battery status/flag and percentage, and leaves `time` and
`units` at the kernel's `-1` and `"?"`. Every consumer of `TIME_LEFT` is
guarded on `> 0`, so this degrades cleanly rather than showing "-1 h".

Also fixed: the `.desktop` install was gated on `WANT_APM` alone, so the
pre-existing ACPI backend built a binary with no menu entry.

### The battery applet was not the battery bug

Getting the applet built produced a *device read error*, and that was a
kernel bug in a completely different place. `corgipm_init()` in
`arch/arm/mach-pxa/corgi_pm.c` gates on:

    if (!machine_is_corgi() && !machine_is_shepherd() && !machine_is_husky())
            return -ENODEV;

This board is none of the three. It boots as Sharp's legacy machine number
**196** (or the **19** the bootloader actually passes), matched by custom
`MACHINE_START` entries at the bottom of `corgi.c` -- see
`docs/DEADLETTER-MACHINE-ID-196.md`. So `corgipm_init()` bailed, the
`sharpsl-pm` platform device was never registered, `apm_get_power_status`
stayed NULL, and `/proc/apm` served only `proc_apm_show()`'s defaults:

    1.13 1.2 0x02 0xff 0xff 0xff -1% -1 ?

AC status, battery status and charge all "unknown". **The fastest way to
confirm this class of fault: look for the device in
`/sys/devices/platform/`.** An entry that is simply *missing* (rather than
present-but-unbound) means an init function bailed, not that a probe
failed. `corgi_pm_patched.c` now accepts the two custom machine numbers,
and after a rebuild `/proc/apm` reads `0x01 0x00 0x01 100%`.

Generalise this: **any mainline driver gated on `machine_is_*()` is
silently disabled on this board.** Others in the tree are unaudited.

---

## mb-applet-system-monitor: the memory bar was nonsense

The CPU bar was correct and the memory bar was not, which is a useful
tell -- they share no code path. `system_memory()` read `/proc/meminfo`
with positional `fscanf()` calls matching the field order of **2.6.0**:

    MemTotal, MemFree, Buffers, Cached, ..., SwapTotal, SwapFree

The kernel has inserted fields since. `MemAvailable` landed third in 3.14,
so every value after `MemFree` came out of the wrong line: "buffers" read
`MemAvailable`, "cached" read `Buffers`, and the two swap slots landed
nowhere near `SwapTotal`/`SwapFree`. Then

    cache_used + used - cached - buffers

on **unsigned** values underflowed into an enormous `mem_used` and a wild
percentage. With the device's real numbers (`MemTotal 52368`,
`MemAvailable 30996`) it should read 40%.

Now keyed off the labels, so field order cannot matter again. Prefer
`MemTotal - MemAvailable` (the kernel's own estimate of what is in use) and
fall back to excluding reclaimable pages by hand when `MemAvailable` is
absent. Swap is no longer folded into a percentage *of RAM*, which is what
let the bar exceed 100%, and a zero `SwapTotal` no longer divides by zero.

Watch for the `Cached:` / `SwapCached:` trap when editing this: a naive
match captures the wrong line.

---

## mb-applet-wireless: needed libiw, and had never run

### Building libiw

`tools/build-libiw.sh` cross-builds the one source file out of the
already-vendored `userspace/wireless_tools.29`. Three non-obvious choices:

- It does **not** reuse the `libiw.a` already sitting in that tree. Those
  ARM binaries were built with a different toolchain and statically linked;
  mixing another libc's objects into a uclibc link invites subtle breakage.
- It builds against wireless-tools' **own** `wireless.h` (WE21), not the
  sysroot's `linux/wireless.h` (WE22). `iwlib.c` uses the `IW_MODUL_*`
  modulation constants, which are a wireless-tools addition the kernel uapi
  header has never carried -- against the sysroot copy the build dies on
  ~15 undeclared identifiers. The version gap is harmless: the 32-bit ioctl
  structs are unchanged, iwlib re-checks `we_version_compiled` at runtime,
  and the `iwconfig` already on the device was built from this same header
  against this same kernel.
- It compiles `-DWE_NOLIBM`, swapping libiw's two frequency helpers off
  `floor`/`log10`/`pow`. Otherwise libiw drags in libm.

The applet links libiw **statically** and gains no new `DT_NEEDED`. `-lm`
resolves inside libc on this uclibc toolchain, which ships only a static
`libm.a`. Payload cost: about 40KB.

### The startup segfault

`find_iwface()` probes the interface being enumerated, but passed
`Mwd.iface` to `iw_get_range_info()` and `iw_get_stats()` instead of
`ifname`. `Mwd.iface` is assigned only at the *bottom* of that same
function, so on the **first** wireless interface it is still NULL, and
iwlib's `iw_get_ext()` does `strncpy(request->ifr_name, ifname, IFNAMSIZ)`
on it.

This is not theoretical. Built unpatched and run under `qemu-arm` on a host
with a wireless interface, it dies with SIGSEGV inside
`iw_enum_devices()` every time. That is almost certainly why this applet is
so rarely shipped by anyone.

### `217dBm`

Level and noise were printed with `%u` straight out of a raw `__u8`, so an
ordinary `-39dBm` displayed as `217dBm`. dBm is a signed 8-bit value stored
in that byte: anything `>= 64` needs `0x100` subtracted, selected with the
same test iwlib's own printer uses.

### wifi0 vs wlan0, and what hostap actually reports

The popup reports **`wlan0`**, and getting there is subtler than it looks.
`/proc/net/wireless` on this device:

     wifi0: 0000    0     0     0   ...
     wlan0: 0000   46.  -39.  -86.  ...

`wifi0` is enumerated **first**. The saving grace is that
`iw_get_stats()` **fails outright** on `wifi0` rather than returning zeros,
so `has_stats` stays 0, the guard in `find_iwface()` does not short-circuit,
and enumeration continues to `wlan0`. That is exactly what upstream's
cryptic "works round odd issues on Z with host AP" comment is about, and it
does work -- verified on hardware, not assumed.

**Do not generalise hostap's broken TX counters to its signal stats.** The
TX byte/packet counters always read zero (see
`docs/DEADLETTER-*` and the hostap notes), but quality/level/noise on
`wlan0` are real and live -- observed moving 44 → 62 and −22 to −42 dBm
across reads.

The popup also shows the interface's IPv4 address, read with `SIOCGIFADDR`,
and `None` when there is no address yet. That is a genuinely useful state to
be able to see: an afternoon went into hunting this device's IP after its
hotspot re-addressed its subnet.

---

## Deploying onto a running session

Two device constraints make this harder than it should be, and both are
easy to rediscover the hard way.

**This device's busybox cannot signal processes at all.** There is no
`kill`, `killall`, `pkill` or `nohup` applet -- `kill` is not even an ash
builtin (`ash: kill: not found`), and `busybox kill` answers `applet not
found`. `/sbin/reboot` and `/sbin/halt` do exist. So:

- Any plan beginning "stop the session, then..." is dead on arrival. Either
  reboot, or start components alongside what is already running.
- Backgrounded processes die when the SSH connection closes. The pure-shell
  equivalent of `nohup` works:

      ( trap "" HUP; DISPLAY=:0 cmd </dev/null >/tmp/log 2>&1 & )

- `command -v` does not exist either. Probe for a tool by *using* it.

**Unpacking over a running binary fails with ETXTBSY** and `untar`
(`open(O_TRUNC)`, no unlink first) stops at the first one, aborting the
deploy half-applied. ETXTBSY blocks *writing* a busy executable but not
*renaming* it, so `build-matchbox-payload.sh` moves the offending path
aside and retries, per file -- only ever touching a path that is both in
the payload and genuinely blocking. The `.replaced` leftovers cannot be
unlinked while their process lives, so they are swept on the next deploy.

**Verify content, not length.** This link once delivered a
byte-complete but corrupted tarball that the size check waved through;
`untar` died on "bad header checksum". Transfers are now md5-verified and
retried up to five times.

After any deploy, processes already running keep their **old inodes**. The
new binaries do not take effect until the session restarts.

---

## Where the source lives

Local changes are **commits on forks**, not patches. Each submodule points
at a fork under `github.com/sugarkrap` whose history is the same upstream
commit it was pinned at, plus our commits on top:

| Submodule | Fork | Our commits |
|---|---|---|
| `matchbox-panel` | `sugarkrap/matchbox-panel` | battery `/proc/apm`, meminfo parse, wireless fixes, `mb-applet-tasks`, `msg_set_timeout` prototype |
| `matchbox-window-manager` | `sugarkrap/matchbox-window-manager` | GConf m4 fallback, missing includes, `_NET_WM_STATE_HIDDEN` |
| `xserver` | `sugarkrap/xserver` | font-util compat m4, kdrive evdev absolute pointer |
| `libX11` | `sugarkrap/libx11` | cherry-picked upstream `XKBgeom.h` (`1f1ca086`), nls srcdir |
| `libfontenc` | `sugarkrap/libfontenc` | font-util compat m4 |

This replaced an earlier scheme where the submodules stayed pristine and
every edit lived as a patch under `modules/x11/`, applied by
`tools/setup-x11-src.sh`. That existed because committing inside a
submodule would have made the parent record a SHA that does not exist on
`gitlab.freedesktop.org` or `git.yoctoproject.org`, so a clone could not
fetch it. Forks remove that constraint, so
`git submodule update --init --recursive` is now sufficient.

`tools/setup-x11-src.sh` no longer applies anything. It **verifies** that
each submodule carries its expected change and fails loudly otherwise,
because a submodule quietly sitting on pristine upstream is exactly the
kind of thing that costs an afternoon. `modules/x11/` now holds only
`matchbox-session`.

---

## Verifying a change to any of this

The applets are graphical and mostly cannot be asserted on from a script,
so lean on these instead. All of them caught something real here:

- **Run the whole session headlessly, on the host.** This is the strongest
  of these and was not available until 2026-08-01. The `xserver` submodule
  builds a native **Xvfb** (`--enable-xvfb`, plus native `xtrans` and
  `libXfont`; point `ACLOCAL_PATH` at `userspace/src/xorg-macros` or
  configure dies on `xorg-macros`/`XTRANS_CONNECTION_FLAGS`). Build
  libmatchbox, the WM and the panel natively into the same throwaway
  prefix, run `Xvfb :9 -screen 0 640x480x16` at the device's real
  geometry and depth, and you have the actual session to poke at:

      xprop -root _NET_CLIENT_LIST        # what the WM is publishing
      import -window root shot.png        # what it looks like
      XTestFakeButtonEvent via a 20-line  # real clicks, so menus and
        helper                            #   button hit-testing run

  Two Xvfb gotchas: it execs `xkbcomp` **from its own `--prefix`** (symlink
  the host one in, or it dies with "Failed to activate core devices"), and
  the default font path under a throwaway prefix is empty, which is
  harmless once Xft is finding fonts through fontconfig.

  Every bug in `mb-applet-tasks` that mattered was found this way and none
  of them could have been: a use-after-free that crashed it the instant it
  docked, a heap overflow when the applet grew from empty, a 10-second
  boot stall from self-hiding, and a font weight that measured 42% grey
  where it should have been black. It cross-compiled cleanly with zero
  warnings through all of them.

- **Measure the pixels, do not squint at them.** `magick <shot> -crop
  <region> -format %[min] info:` gives the darkest pixel in a region --
  which is how "the label looks a bit washed out" became "42% grey versus
  the clock's 0", i.e. a real defect rather than a matter of taste. Crop
  the panel strip and `-filter point -resize 300%` to actually see 24px
  icons and 12px text.

- **Extract the function under test and drive it.** `sed` the real function
  out of the patched source into a harness so the test cannot drift from
  the shipped code, then feed it known inputs. This is how the `/proc/apm`
  parser, the meminfo parser, `iface_address()` and the dBm conversion were
  checked -- including cases the device could not produce on demand
  (seconds-vs-minutes units, a 2.6.0 meminfo layout, zero swap).
- **Compare against ground truth on the device.** `/proc/net/wireless`,
  `ifconfig`, `/proc/meminfo` and `/proc/apm` are all readable over SSH;
  compute what the applet should display and check the arithmetic against
  them.
- **`qemu-arm` runs these binaries on the host.** The applets do their
  wireless enumeration *before* connecting to X, so a segfault in that path
  reproduces with no display at all. Note that `mb-applet-battery` and
  `mb-applet-system-monitor` also segfault after printing
  `Cannot open display:` -- that is matchbox's generic no-DISPLAY
  behaviour, not your bug. Establish that baseline before reading anything
  into a crash.
- **md5 the deployed binary against the built one.** Proves the device is
  running what you just built, which is not obvious given ETXTBSY,
  old inodes and a flaky link.
- **Ask.** Whether a bar or an icon *looks* right is not something any of
  the above establishes.
