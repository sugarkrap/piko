# FLTK: building it, shipping it, and proving it works

*Written 2026-07-31, when FLTK was added to the ROM and confirmed drawing
on real hardware. Companion to `docs/HOWTO-MATCHBOX-DESKTOP.md`, which
covers the X11 stack FLTK sits on top of; this file is only about FLTK.*

The short version:

    tools/build-fltk.sh                    # cross-build, into stage-target
    tools/build-matchbox-payload.sh --deploy --adapter wlan0 root@<ip>
    # then, on the device:
    DISPLAY=:0 fltktest

FLTK is here so we can **write our own GUI apps** for this device instead
of only running other people's. Matchbox gives us a desktop; FLTK gives us
a toolkit to put things on it. It is a normal X11 client — it talks to the
same `Xfbdev` everything else does, and needs no kernel support of its own.

---

## What you get

Built by `tools/build-fltk.sh` into `userspace/stage-target`, shipped to
the device by `tools/build-matchbox-payload.sh`:

| File | On device | Stripped | What it is |
|---|---|---|---|
| `libfltk.so.1.3` | `/lib` | 847K | the toolkit: widgets, events, drawing |
| `libfltk_images.so.1.3` | `/lib` | 143K | PNG/JPEG/GIF/BMP image loaders |
| `libfltk_forms.so.1.3` | `/lib` | 22K | XForms compatibility shim, legacy |
| `libstdc++.so.6` | `/lib` | 1.6M | **new to the ROM** — see below |
| `fltktest` | `/usr/local/bin` | 10K | the smoke test |

Headers (`FL/*.H`), the static `.a` archives and `fltk-config` also land in
`userspace/stage-target`, but are **host-side only** — they are for
cross-linking your own apps and the payload deliberately prunes them.

**`libstdc++.so.6` is the real cost of FLTK.** FLTK is the only C++
component in the entire stack, and every `libfltk*.so` has libstdc++ in its
`DT_NEEDED`. At 1.6MB stripped it is the largest single item in the payload
after `Xfbdev` and `libX11` — bigger than all three FLTK libraries put
together. Nothing is wrong with that, but if flash ever gets tight, this is
the line item to know about.

---

## Why FLTK 1.3 and not 1.4 or 1.5

`userspace/src/fltk` is pinned at **`release-1.3.11`** (`702172a`), the last
release of the autotools + C++98 + X11-only series. That is the same shape
every other component in this tree has, and it matters:

- **1.4 makes CMake the primary build system.** Every other package here is
  `./configure --host=...`; adopting 1.4 would mean carrying a CMake
  toolchain file for one outlier.
- **1.4 wants C++11**, and defaults to a **Wayland backend** with
  Pango/Cairo font handling. There is no Wayland here and never will be,
  and Pango/Cairo would be a whole new dependency chain for a PXA255 with
  64MB of RAM.
- **1.3.11 is not stale.** It is a 2025 maintenance release, and it builds
  clean under this tree's GCC 13.4 cross-compiler with no patches at all.

That last point is worth stating plainly: **FLTK is the only X-adjacent
submodule here that needed no local fork and no patches.** It is pristine
upstream, which is why it is not listed in `tools/setup-x11-src.sh`.

---

## Building it

    tools/build-fltk.sh [--force]

Prerequisites it checks for and fails loudly on, rather than silently
skipping:

- the uClibc cross toolchain (and specifically a **C++** compiler —
  `arm-unknown-linux-uclibcgnueabi-g++`)
- an X11 stack already staged in `userspace/stage-target`, detected via
  `usr/lib/pkgconfig/xft.pc`. FLTK links libX11/libXft/libXrender/libXext/
  fontconfig/freetype out of there and is useless without them.
- `autoconf` on the host — the submodule is a git checkout, so it ships
  `configure.ac` but no generated `configure`.

Env overrides: `CROSS_HOST`, `TOOLCHAIN_BIN_DIR`, `STAGE`, `JOBS`.

It is **idempotent** — it skips everything if `libfltk.so.1.3` is already
staged — so `tools/build-userspace.sh` (step 6) and
`tools/build-and-deploy.sh` call it unconditionally. `--force` does a
`make distclean` and rebuilds from scratch; that is about 17 seconds with
`-j` on a modern desktop, so there is no reason to avoid it.

`tools/build-userspace.sh --skip-fltk` opts out. On a checkout that hasn't
done the X11 bring-up yet, step 6 **skips without failing**, exactly like
`st` does, so you still get a complete ALSA/MPlayer/SDL build.

### The configure choices that are not guessable

All four are in the script's header too, but they are the whole reason this
script exists rather than a line in a doc:

**`--x-includes` / `--x-libraries` are mandatory.** FLTK finds X through
`AC_PATH_XTRA`, which searches a hardcoded list of *host* paths and knows
nothing about a cross sysroot. Without these it either finds nothing or —
much worse — finds the build machine's own `/usr/include/X11` and produces
a library that compiles and links and cannot possibly run. Same trap
`matchbox-window-manager` has.

**`--enable-xft`, explicitly, not just left at its default `yes`.** This
board has **no core X bitmap fonts at all**; the only fonts in the payload
are the DejaVu TTFs that fontconfig serves (see "Fonts are mandatory" in
`docs/HOWTO-MATCHBOX-DESKTOP.md`). Without Xft, FLTK falls back to core X
fonts and every single label renders blank. Passing the flag explicitly
makes configure **abort** when Xft is missing, instead of quietly building
that unusable library and letting you discover it on the device.

**`--disable-localzlib --disable-localpng`, but `--enable-localjpeg`.**
FLTK vendors copies of all three. zlib and libpng are already cross-built
and staged for the rest of the desktop, so using the bundled ones would
ship a second copy of each inside `libfltk_images` — pointless on a
flash-constrained device. Nothing here stages a libjpeg, so *that* one
stays bundled and is linked in **statically** (there is no `libjpeg` in any
`DT_NEEDED`; configure compiles it `-fPIC` because `--enable-shared` is on,
so a static archive inside a shared object is fine).

**Only the image dirs and `src/` are built.** Upstream's default `make`
also builds `fluid/`, `test/` and `documentation/`. `fluid` is FLTK's UI
designer and is a **target** binary — it cannot run on the build host —
`test/` needs to *run* it to turn `.fl` files into `.cxx`, and
`documentation/` needs doxygen. None of that ships to the device. The
script reads `IMAGEDIRS` back out of the generated `makeinclude` rather
than hardcoding `jpeg`, so flipping one of the local-lib flags above
cannot silently leave a bundled library unbuilt.

### What the build proves before it exits 0

Because "it compiled" has burned this project before, `build-fltk.sh` does
not treat a successful `make` as success:

- **ELF flags are `0x5000200`** — Version5 EABI, soft-float — matching
  `Xfbdev`/`libX11`. A toolchain mix-up fails here, not on the device.
- **`SONAME` is `libfltk.so.1.3`.** FLTK versions its shared libraries
  *major.minor*, not major, so the real filename and the SONAME are the
  same string. `build-matchbox-payload.sh` ships the file under its own
  name and relies on that; a mismatch would deploy a library the dynamic
  linker can never find.
- **`fltktest` actually links against `libfltk.so.1.3`**, so a build that
  quietly went static or picked up a different library is caught.

Then `build-matchbox-payload.sh` re-checks, for the whole payload, that
every `DT_NEEDED` is satisfied and every ELF is ARM. That check is what
catches a forgotten `libstdc++.so.6`.

---

## Testing it on the ROM

### On the host, before deploying

    arm-unknown-linux-uclibcgnueabi-readelf -d \
        userspace/stage-target/usr/lib/libfltk.so.1.3 | grep -E 'NEEDED|SONAME'

Expect `libXrender`, `libXext`, `libXft`, `libfontconfig`, `libX11`,
`libstdc++.so.6`, `libgcc_s.so.1`, `libc.so.0`, and
`SONAME libfltk.so.1.3`.

### On the device

`fltktest` ships to `/usr/local/bin/fltktest`. Run it from `st`, or over
SSH with `DISPLAY` set:

    DISPLAY=:0 fltktest

It is deliberately **keyboard-free** — the Zaurus keyboard cannot type many
characters (see `AGENTS.md`), so it quits with a click on its Quit button
or the window's close box, never a typed command.

A healthy run prints exactly this:

    fltktest: FLTK 1.3.11
    XOpenIM() failed
    fltktest: window shown, entering event loop

and shows a small window with the bold label "FLTK is alive", a box with a
drawn X and border, and a Quit button.

**`XOpenIM() failed` is benign and expected.** There is no X input method
server on this device and nothing here needs one; FLTK warns once and
carries on. It is not a symptom of a broken build.

The two `printf`s are positioned on purpose: the version line prints
**before** anything touches the display, so a loader failure and an X
failure look completely different. If you see no output at all, the problem
is the dynamic loader; if you see the version line and then nothing, the
problem is X.

To stop it — remember this device's busybox has **no `kill`, `killall`,
`pkill` or `nohup`**:

    pkillx fltktest

To leave it running after you disconnect, detach it the way everything else
here does:

    (trap "" HUP; DISPLAY=:0 fltktest > /tmp/fltktest.log 2>&1 &)

### Verified, on hardware

On 2026-07-31 this was run on the real board (`10.208.47.2`) under a live
Matchbox session: `fltktest` printed the three lines above and its window
rendered text and graphics on the panel — **confirmed by eye, not inferred
from an exit code.** That distinction is not pedantry on this project:
`docs/DEADLETTER-AUDIO-I2S-SILENT.md` is the write-up of a sound card that
registered perfectly and played nothing.

---

## Writing your own FLTK app

Build it against the staging tree, not against anything on your host. This
exact line is verified to compile and link:

    TC=<repo>/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin
    STAGE=<repo>/userspace/stage-target

    $TC/arm-unknown-linux-uclibcgnueabi-g++ -O2 \
        -isystem $STAGE/usr/include \
        -o myapp myapp.cxx \
        -L$STAGE/usr/lib -Wl,-rpath-link=$STAGE/usr/lib \
        -lfltk -lXrender -lXext -lXft -lfontconfig -lpthread -lX11

Add image loading (`Fl_PNG_Image`, `Fl_JPEG_Image`, ...) by putting these
**before** `-lfltk`:

        -lfltk_images -lpng -lz

Notes on that line:

- **`-isystem`, not `-I`.** FLTK 1.3's own headers emit hundreds of
  `-Wunused-parameter` warnings that will bury every real warning in your
  own code.
- **The library list is not arbitrary** — it is what FLTK's configure
  settled on, readable at any time as `LDLIBS` in
  `userspace/src/fltk/makeinclude`. `build-fltk.sh` reads it from there
  rather than re-deriving it, and so should you if you script this.
- **`-rpath-link`, not `-rpath`.** It lets the cross-linker resolve
  *indirect* dependencies (libXft needs libfreetype needs libz) at link
  time without baking a host path into the binary. The device resolves
  these from `/lib` at runtime.
- `fltk-config` **is** staged at `$STAGE/usr/bin/fltk-config`, but it emits
  `-I/usr/include -L/usr/lib` — the *device's* paths, since the build is
  `--prefix=/usr`. Do not use it to cross-link on the host.

`userspace/src/fltktest.cxx` is a working, deliberately small example: a
window, a styled label, a custom `draw()` override, and a callback.

To ship your app, add it to `tools/build-matchbox-payload.sh` the way
`fltktest` is — one entry in the `for spec in ...` list. If it needs a
desktop menu entry, `userspace/desktop/st.desktop` is the pattern.

---

## What is deliberately turned off

| Off | Why |
|---|---|
| OpenGL (`--disable-gl`) | no GPU, no Mesa. `Fl_Gl_Window` does not exist in this build. |
| Xinerama, Xfixes, Xcursor | not staged, and meaningless here (one screen, no accelerated cursor). Passed explicitly so a build host that *has* them installed cannot change what gets built. |
| Cairo (`--disable-cairo`) | another dependency chain for drawing we do not do. |
| `fluid` | FLTK's UI designer. It is a target binary; it cannot run on the build host and there is no reason to run a GUI designer on a 640x480 clamshell. Design on the host with a host-installed FLTK if you want it, and cross-build the generated `.cxx`. |
| C++ exceptions | FLTK's own objects are compiled `-fno-exceptions` (that is upstream's default `OPTIM`, not our choice). Your own code can still use exceptions, but do not expect one to propagate *through* an FLTK callback. |

`libfltk_forms.so.1.3` is the XForms compatibility layer and nothing here
uses it. It ships anyway, at 22K, because `src/` builds it unconditionally
and shipping every library the build produces is less surprising than a
`-lfltk_forms` that links on the host and is missing on the device.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| `fltktest: not found` (but the file is there) | the dynamic loader cannot resolve something. Check `/lib/libstdc++.so.6` and `/lib/libfltk.so.1.3` exist. This is the ROM's classic misleading error — it means the *interpreter or a library* is missing, not the binary. |
| Version line prints, then nothing | X problem, not a loader problem. Is `Xfbdev` running? Is `DISPLAY=:0` set? |
| Window appears, all labels blank | the Xft path is broken. Check `/etc/fonts` and the DejaVu faces are present — with no core X fonts on this board, no fontconfig means no text at all. |
| `XOpenIM() failed` | benign, always printed here. Not a fault. |
| `Text file busy` during deploy | you are overwriting a running binary. `build-matchbox-payload.sh` already handles this by renaming the busy file aside and retrying. |
| configure aborts on Xft | the X11 stack isn't staged, or `xft.pc` isn't in `stage-target`. Build the X11 stack first. This abort is intentional. |
| `configure` runs native and fails on "cannot run C compiled programs" | `--host` didn't reach configure. Use `build-fltk.sh`; don't invoke `autogen.sh` by hand, it runs configure itself unless `NOCONFIGURE=1` is set. |

---

## Where things live

    userspace/src/fltk/           the submodule, pinned at release-1.3.11
    userspace/src/fltktest.cxx    the smoke-test app (tracked source)
    tools/build-fltk.sh           the cross-build
    userspace/stage-target/       where it installs (gitignored artifact)
      usr/lib/libfltk*.so.1.3       the libraries
      usr/include/FL/               headers, for cross-linking on the host
      usr/bin/fltk-config           host-side helper, never shipped
      usr/bin/fltktest              the built smoke test

Ships to the device via `tools/build-matchbox-payload.sh` (which
`tools/build-and-deploy.sh` repacks, and `tools/chunked-deploy.sh`
section 9 transfers). There is no separate FLTK deploy step and there
should not be one.
