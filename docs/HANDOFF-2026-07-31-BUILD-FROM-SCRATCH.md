# Handoff: building this project from scratch on another machine

*Written 2026-07-31, the day the Zaurus first booted straight to a
graphical desktop.*

Read `AGENTS.md` first -- it has the hard constraints (no USB cable, no
serial cable, last spare board) that explain why a lot of this is shaped
the way it is.

---

## What currently works

A Sharp Zaurus SL-C760 (PXA255, ARMv5TE, 64MB RAM) running mainline
Linux 7.1.4, booting **straight to a Matchbox desktop**: themed window
manager, app-folder desktop, panel, with a working built-in keyboard and
touchscreen.

    two-stage kexec boot  ->  Xfbdev (kdrive)  ->  matchbox-session
                                                     |- matchbox-desktop
                                                     |- matchbox-panel
                                                     '- matchbox-window-manager

Working: framebuffer X, keyboard (with a custom XKB layout for the
Zaurus Fn symbol row), touchscreen as an absolute pointer, WiFi, SSH,
audio, SD card, MPlayer, SDL 1.2 (fbcon backend, confirmed drawing to
the panel via `sdltest` -- see `tools/build-sdl.sh`).

---

## READ THIS BEFORE YOU START: what is *not* reproducible yet

Be realistic about the state of things. Three gaps stand between a fresh
clone and a working build, and none of them are one-liners.

### 1. The toolchain must be rebuilt (long, but now automated)

`toolchain/` is gitignored -- it is a multi-GB machine-specific build
output, not source -- so a fresh clone has no compiler. This was the
biggest gap in the project until 2026-07-31; it is now closed:

    tools/build-uclibc-toolchain.sh

builds it from source with crosstool-NG, driven by the tracked
`tools/uclibc-toolchain.config` (lifted verbatim out of the known-good
toolchain's own preserved `ct-ng.config.bz2`, with the two
machine-specific absolute paths replaced by a placeholder). It exits
immediately if a working toolchain is already present.

What it produces, and what everything else assumes:

    crosstool-NG    1.28.0
    target triplet  arm-unknown-linux-uclibcgnueabi
    gcc             13.4.0
    libc            uClibc-ng
    ABI             EABI5, **soft-float**, ARMv5TE

Expect tens of minutes to hours and several GB of disk. It is a one-off
per machine. The script sanity-checks the resulting ABI afterwards,
because a hard-float or ARMv7 toolchain will build everything happily and
then produce binaries this board cannot execute.

Everything lands at `toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/`;
every script honours `TOOLCHAIN_BIN_DIR` / `CROSS_HOST` if yours differs.

Note `tools/oabi-toolchain.config` + `tools/build-oabi-toolchain.sh` are a
*separate* OABI toolchain for the NAND flash helpers. The two are not
interchangeable.

### 2. Device-side bootstrap is a chicken-and-egg

`tools/chunked-deploy.sh` ships the X11 stack as one tar and unpacks it
with `/usr/local/bin/untar` **on the device**. On a freshly-flashed
device that binary does not exist yet, and this rootfs's busybox has
neither `tar` nor `kill`. Build and push both by hand first:

    $GCC -march=armv5te -O2 -static -o untar userspace/src/untar.c
    $GCC -march=armv5te -O2 -static -o kill  userspace/src/kill.c
    # then, per-file over ssh:
    ssh root@DEV 'cat > /usr/local/bin/untar && chmod 755 /usr/local/bin/untar' < untar
    ssh root@DEV 'cat > /usr/local/bin/kill  && chmod 755 /usr/local/bin/kill'  < kill

`chunked-deploy.sh` refuses with a clear message if `untar` is missing.

### 3. The X component builds are not automated

`tools/build-thirdparty-deps.sh` handles the non-X.Org libraries and
`tools/setup-x11-src.sh` applies our local patches, but nothing yet runs
`configure && make` for the X.Org submodules or the Matchbox components.
Those steps are still manual, per
`docs/HOWTO-MATCHBOX-DESKTOP.md`. Automating that into a
`tools/build-x11-stack.sh` is the obvious next piece of work.

---

## Host prerequisites

    git git-lfs curl build-essential autoconf automake libtool pkg-config
    bison flex gettext python3 xz-utils bzip2
    qemu-user            # optional but very useful: smoke-test ARM binaries
    xkbcomp              # optional: validate the XKB layout on the host
    xorgproto            # arch-independent protocol headers, read from
                         # /usr/share/pkgconfig by the X submodule builds

`flash/mtd3.jffs2` is tracked via **Git LFS** -- `git lfs install` before
cloning or you get a pointer file.

---

## From zero to a running desktop

    git clone --recurse-submodules <repo> piko && cd piko
    # (or: git submodule update --init --recursive)

**1. Toolchain.** One-off, slow, automated:

    tools/build-uclibc-toolchain.sh

Verify, and check the ABI is really ARMv5TE soft-float:

    toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin/*-gcc --version

**2. Kernel.** Reconstructs a pristine kernel.org tree and applies every
tracked patch under `modules/`:

    tools/setup-kernel-src.sh --force

**3. Third-party libraries + fonts + XKB data.** Each pinned by version
and verified by SHA-256 before use:

    tools/build-thirdparty-deps.sh

**4. Apply our X patches**, then build the X.Org submodules and Matchbox
components by hand:

    tools/setup-x11-src.sh

Build order and the exact configure lines are in
`docs/HOWTO-MATCHBOX-DESKTOP.md`. Several are **not** guessable:

- `libXrender` must be **0.9.7** (0.9.11+ needs `x11 >= 1.6`; libX11 is
  pinned at 1.4.4)
- `matchbox-window-manager` must **not** get `--enable-standalone`
- `matchbox-desktop` needs `--sysconfdir=/etc` **and** a forced
  `-DUSE_XSETTINGS` plus `-I<stage>/usr/include/libmb`
- `xserver` needs `--enable-kdrive-evdev --enable-kdrive-kbd
  --enable-kdrive-mouse` and `CWARNFLAGS='-Wall -Wno-error'`

Install each component to its **own** `DESTDIR`
(`/tmp/mbwm-stage`, `/tmp/mb-stage-desktop`, `/tmp/mb-stage-panel`,
`/tmp/mb-stage-common`) -- they are built independently and would
otherwise race installing into one tree.

**5. Pack and deploy.**

    tools/build-matchbox-payload.sh                 # inspect the tar first
    tools/build-and-deploy.sh --adapter <iface> root@<device-ip>

`build-and-deploy.sh` builds the kernel + modules, repacks the X payload,
and hands off to `chunked-deploy.sh`, which ships everything and unpacks
X with `untar`. Useful flags: `--kernel-only`, `--skip-x11`,
`--no-userspace`, `--force-kernel-src`.

**6. Reboot.** It should come up at the desktop. If X fails,
`/etc/init.d/xsession` drops you to a console login on tty1 instead --
by design, since inittab respawns it.

---

## Verifying it actually works

Do not trust "it started". This project has repeatedly had components
enumerate fine and do nothing.

    ssh root@DEV 'dmesg | grep -E "pxa2xx-spi|ads7846"'   # want "registered host spi1"
                                                          # and "touchscreen, irq 117"
    ssh root@DEV 'grep ads7846 /proc/interrupts'          # count must RISE when you tap
    ssh root@DEV 'dd if=/dev/input/event2 of=/tmp/ts.bin bs=16 count=8'  # tap while it runs
    tools/decode-input-events.py ts.bin                   # want BTN_TOUCH/ABS_X/ABS_Y

`/proc/interrupts` is the single most useful diagnostic on this board.

---

## Known issues, roughly by severity

1. **`hostap` refcount use-after-free.** Two `WARNING: lib/refcount.c`
   backtraces in `prism2_interrupt` on *every* boot -- "addition on 0"
   and "underflow", both use-after-free. This is the most likely cause of
   a hard freeze seen this session. Untouched; a real bug in a live
   interrupt handler.
2. **Fn/Level3 keymap layer is untested.** The layout is built and
   loaded, but nobody has actually pressed Fn since it was written.
   Re-calibration and re-measurement steps are in
   `docs/HOWTO-X11-TOUCHSCREEN.md`.
3. **`corgi-lcd` binds but logs `No GPIO consumer BL_ON found`** --
   backlight control probably still absent.
4. `mb-applet-system-monitor` (and the battery applet) use
   `mb_tray_app_new()`'s result with no NULL check and segfault if the
   panel starts them before X is up. One line each.
5. The battery applet is buildable with **no new libraries** -- the
   kernel has `CONFIG_APM_EMULATION=y` so `/proc/apm` exists. A patch was
   drafted this session but deliberately not applied.
6. `Root.directory` references `gnome-folder.png`, which nothing ships.
7. `tools/deploy-x11.sh` predates `build-matchbox-payload.sh` +
   `chunked-deploy.sh` section 9 and now overlaps them. Decide which
   survives rather than letting both drift.

---

## Where things live

    modules/                kernel patches, applied by setup-kernel-src.sh
      spi/                  the SPI double-request + PIO fix
      x11/                  patches into the X submodules (setup-x11-src.sh)
    userspace/src/          X.Org + Matchbox submodules, and our own .c
                            (untar.c, kill.c, md5sum.c)
    userspace/xkb/          the hand-written Zaurus XKB layout
    userspace/stage-target/ ARM sysroot everything is built against (gitignored)
    rootfs/etc/             tracked device config: inittab, init.d/rcS,
                            init.d/xsession
    tools/                  build + deploy scripts
    docs/                   HOWTOs and DEADLETTER-* post-mortems

**Read the relevant `DEADLETTER-*.md` before touching kexec, flashing, or
the NAND layout.** They document mistakes that cost a board.

---

## Two things I got wrong this session, recorded so they are not redone

- **The SPI `.id` is 1, not 0.** `drivers/soc/pxa/ssp.c` does
  `port_id = pdev->id + 1` and `pxa25x_device_ssp` is `.id = 0`. Setting
  it to 0 produces a *different* failure ("invalid resource (null)") that
  looks like progress but is not.
- **There is no SSP ownership conflict between audio and SPI.** Corgi
  audio uses I2S (`snd_soc_pxa2xx_i2s`); `lsmod` shows `ssp`'s refcount
  held solely by `spi_pxa2xx_platform`. I asserted this conflict before
  checking, and it sent the investigation sideways.

The real SPI bug was that `spi-pxa2xx-platform.c` requests the SSP port
*twice* and the second request always fails -- and even after fixing
that, transfers only work in **PIO**, not DMA.
