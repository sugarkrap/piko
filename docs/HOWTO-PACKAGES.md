# Packages: opkg on the Zaurus

Installing software after the fact, to the NAND root or to an SD card,
with Sharp-era packages refused until retro-compatibility actually
exists.

Everything below is verified on the real board (SL-C760, mainline 7.1.4)
unless a line says otherwise.

---

## Short version

On the device:

```
pkgadd                  list .ipk files it can find, numbered
pkgadd 1                install number 1 into the NAND root
pkgadd 1 card           install number 1 onto the SD card
pkglist                 what is installed, and where
pkgdel NAME             remove it
deskscan                force a desktop menu refresh (rarely needed)
```

On the build host:

```sh
tools/build-thirdparty-deps.sh libarchive   # opkg's one hard dependency
tools/build-opkg.sh                         # cross-build opkg itself
tools/test-opkg-gate.sh                     # prove the Sharp gate works
tools/make-ipk.sh --name foo --version 1.0 --root stage/
```

All the single-word device commands are deliberate: per `AGENTS.md` the
Zaurus keyboard cannot type `/` `:` `[` `]` `|`, which is also why
`pkgadd` takes a *number* from a listing instead of a path.

---

## Was opkg forked?

**No, and it did not need to be.** Both device-specific behaviours are
things stock opkg already does; what makes them work is configuration and
two small wrapper scripts.

| What we needed | Where it actually comes from |
|---|---|
| Install to NAND **or** SD card | Stock opkg. Multiple destinations are opkg's oldest feature, inherited from ipkg, which was written for exactly this situation — a handheld with a tiny internal flash and a removable card. |
| Per-card package database | Stock opkg. `pkg_dest_init()` gives every destination its own `status` file and `info/` directory under its own root, so a card carries its package list with it. |
| Refuse Sharp-era `.ipk` | Stock opkg's `arch` list — see the gate section below. |
| Desktop notices card apps | **Not opkg's business at all.** This is a change to `matchbox-desktop`, described further down. |

The one thing that *is* a code change lives where it belongs: in
`matchbox-desktop-classic`, not in the package manager.

### opkg build notes

`tools/build-opkg.sh` builds **0.6.3**, the last autotools release (0.7+
moved to CMake, which would mean carrying a cross toolchain file for one
package). It is configured with no curl, no gpgme, no openssl, no
libsolv, and linked **fully static** — one ~520KB binary, no `libopkg.so`
and no `libarchive.so` beside it.

Static is not about size. opkg is the program that overwrites files on
this system, so anything it links dynamically is something a bad package
can break, leaving no working opkg to undo it with. It also has to run on
a freshly flashed board where the dynamic loader (which ships in the
Matchbox payload) may not be present yet.

> **libarchive is unavoidable.** Every opkg release depends on it —
> `PKG_CHECK_MODULES([LIBARCHIVE], [libarchive])` is unconditional in
> `configure.ac` as far back as 0.3.0 and is still there at git master.
> The bundled busybox-derived untar people remember belonged to *ipkg*,
> opkg's predecessor. There is no version that avoids the dependency.

> **The `-static` trap.** opkg links through libtool, and libtool
> reinterprets a plain `-static` in `LDFLAGS` as "prefer the static
> archive of the libtool libraries", then drops it. The build succeeds,
> the binary looks fine, and it is still dynamic. The static link is
> therefore requested at `make` time with `-all-static`, and
> `build-opkg.sh` asserts on the result with `readelf -d` because that
> failure is otherwise completely silent.

---

## The retro-compatibility gate

Sharp-era Zaurus packages declare `Architecture: arm`. That is not merely
"old" — it means ARM **OABI** against **glibc 2.2.2**, usually installing
into `/home/QtPalmtop` for Qtopia. This system is ARM EABI with uClibc
and no Qtopia, so the binaries inside cannot be loaded at all.

Packages built for this system declare `Architecture: piko` (or `all` for
things containing no compiled code). `/etc/opkg/opkg.conf` lists exactly
those two and **deliberately omits `arm`**:

```
arch all   1
arch piko  10
```

Verified on the device:

| Package | Format | Result |
|---|---|---|
| `Architecture: piko` | `ar` (modern) | installs |
| `Architecture: arm` | `tar.gz` (pre-2005) | **refused**, exit 255 |
| no `Architecture:` field | `tar.gz` | **refused**, exit 255 |

Note that the *format* is not what decides this. opkg enables both
`archive_read_support_format_ar` and `..._tar` for the outer archive, so
it opens ancient tar.gz-style `.ipk` files perfectly happily. Only the
architecture field is consulted.

`pkgadd` turns opkg's rather terse "Not selecting X due to incompatible
architecture / Unknown package" into an explanation of what an OABI
package is and why nothing was written.

### ⚠ The gate is an *absence*, and opkg does not fail closed

This is the part worth remembering. If the `arch` lines are deleted, or
if opkg is ever rebuilt without `--sysconfdir=/etc` so that it never
finds `opkg.conf`, opkg does not complain and does not refuse anything.
It falls back to a built-in list of `{all, noarch, HOST_CPU_STR}`, and
`HOST_CPU_STR` comes from the configure `--host` triplet — so it is
literally the string `arm`.

In that state **Sharp packages install cleanly and silently.**

Verified both directions under `qemu-arm`: with the arch lines present a
Sharp package is refused; with them removed the same file reports
`Installing sharpapp (1.0)`.

`tools/test-opkg-gate.sh` locks this down, and its fourth check is the
important one — it strips the arch lines on purpose and asserts the Sharp
package *would* have installed. Without that negative control the other
checks could pass for an unrelated reason.

```
$ tools/test-opkg-gate.sh
  PASS  piko package installs
  PASS  Sharp 'arm' package refused
  PASS  package with no Architecture refused
  PASS  without arch lines, Sharp package WOULD install
```

When retro-compatibility is eventually built, do **not** simply add `arm`
back — that re-admits every genuinely incompatible package at the same
time. Give translated packages their own tag.

---

## Two destinations, and why the card one is not in the config

`opkg.conf` declares only:

```
dest root /
```

The SD card destination is added at *runtime* by `pkgadd`, with
`--add-dest card:/mnt/card/.zaurus`, and only after it has confirmed a
card is really mounted.

That is not a stylistic choice. `pkg_dest_init()` calls
`file_mkdir_hier()` on every declared destination while merely *parsing
the config* — before any command runs, whatever the command is. With a
`dest card /mnt/card/.zaurus` line and no card in the slot, `/mnt/card`
is an ordinary empty directory on the jffs2 root, so opkg silently
creates the whole tree **on the NAND**.

Verified: with the card line present and no card mounted, a bare
`opkg list-installed` — a read-only query — created
`/mnt/card/.zaurus/var/lib/opkg/info` on the root filesystem. Those
directories then shadow the real card when one is inserted, and they eat
the ~68 MiB root.

So `pkgadd ... card` with no card refuses outright:

```
pkgadd: no SD card is mounted at /mnt/card -- refusing.

Nothing was installed. Insert a card and try again, or
install to the NAND instead with:  pkgadd 1
```

Verified on the device, including that `/mnt/card` was still empty
afterwards.

Card layout matches the existing overlay in `/etc/zaurus-card.sh`:

```
/mnt/card/.zaurus/
    usr/bin/                    already on PATH
    usr/share/applications/     scanned by matchbox-desktop
    var/lib/opkg/status         this card's own package database
```

---

## How the desktop notices

`matchbox-desktop` finds out for itself. Two changes in
`matchbox-desktop-classic`:

**1. It scans the card.** `modules/dotdesktop.c` had a fixed array of
four application directories (`APP_PATHS_N 4`); the card's
`usr/share/applications` is now a fifth. A path that is not there costs
one failed `opendir()` per reload, which is the normal case.

**2. It waits on more than X.** `src/mbdesktop_watch.c` (new) waits with
`select()` on the X connection *plus*:

- an **inotify** watch on the application directories — this is what an
  `opkg install` or `remove` looks like from the desktop's point of view;
- the **mount table**, `/proc/mounts`, which is pollable: the kernel
  raises an exceptional condition on it whenever mounts change. This is
  a real event, not a timer.

> inotify **cannot** be used for the card, and this is the trap worth
> recording. A watch is attached to an *inode*, and mounting a filesystem
> over `/mnt/card` does not modify that directory — it covers it. The
> watch stays pointed at the now-hidden jffs2 directory underneath and
> reports nothing, forever. The card's own watch is therefore re-taken
> every time the mount table changes.

The desktop already handled `SIGHUP` by reloading; what it could not do
was find out on its own, or be woken while parked in `XNextEvent()`.

### ⚠ `XPending()` vs `XEventsQueued(QueuedAlready)`

Measured on the device, and the reason `mbdesktop_watch.c` says so in a
comment: the first version used `XPending()` to check for already-queued
events before blocking. `XPending()` is
`XEventsQueued(dpy, QueuedAfterFlush)` — it flushes the output buffer and
then *reads the socket*. Doing that every pass kept the connection
permanently busy:

| build | idle 30s, CPU ticks | wakeups |
|---|---|---|
| unmodified desktop | 0 | 0 |
| with `XPending()` | 23 | ~180 (6/sec) |
| with `XEventsQueued(QueuedAlready)` | **0** | **0** |

`QueuedAlready` only inspects the queue Xlib has already built and
performs no I/O, so an idle desktop stays idle — which matters on a
battery-powered 400MHz machine. Debug output (`MBDESKTOP_WATCH_DEBUG=1`)
made the cause unambiguous: `select=1 x=1 ino=0 mounts=0`, i.e. the X
descriptor, not either watch.

### Coalescing

One user action produces several events — installing a package creates a
`.desktop` file and then closes it (`IN_CREATE` then `IN_CLOSE_WRITE`),
and pulling a card changes the mount table *and* destroys the watched
directory. Each rebuild costs ~0.4s of CPU here, so after the first
change the watcher keeps absorbing for 250ms before asking for a reload.

Measured on the device, before and after:

| action | reloads before | reloads after |
|---|---|---|
| install a package | 2 | 1 |
| card removed | 2 | 1 |
| card inserted | 1 | 1 |
| idle | 0 | 0 |

`deskscan` remains for the cases watching cannot cover — a `.desktop`
file edited in place (no create or delete, so nothing fires), or a card
whose contents changed in another machine.

> `deskscan` matches on the process *name*, from `/proc/<pid>/comm`, not
> by grepping `ps` output. `ps | grep matchbox-desktop` is the obvious
> version and it is wrong: `ps` prints full command lines, so it matches
> any process whose arguments merely *mention* matchbox-desktop —
> including the shell running the script. That is not hypothetical; an
> early version killed the SSH session it was invoked from. Note also
> that `comm` is truncated to 15 characters, so the value actually stored
> is `matchbox-deskto`.

---

## Building a package

```sh
mkdir -p stage/usr/bin stage/usr/share/applications
cp myapp stage/usr/bin/
cp myapp.desktop stage/usr/share/applications/
tools/make-ipk.sh --name myapp --version 1.0 --root stage --desc "My app"
```

Produces `myapp_1.0_piko.ipk`. Copy it to the device (or onto an SD card
from a PC, which is the path that needs no laptop-side SSH) and run
`pkgadd`.

`make-ipk.sh` refuses `--arch arm` outright and rejects a payload
containing `/home/QtPalmtop`, on the grounds that both mean the package
was never going to work here.

A package with no `usr/share/applications/` gets no desktop icon. That is
correct for libraries and command-line tools, and the script says so
rather than leaving it to be discovered.

### Feeds

opkg is built `--disable-curl`, so there is no HTTP feed support. `file:`
URLs are handled by a plain copy inside `opkg_download_internal()`
*before* any download backend is consulted, so a feed on the SD card
works fully — `opkg update` and installing by name included:

```
src/gz card file:/mnt/card/.zaurus/packages
```

It is left commented out in `opkg.conf` because a `src` line pointing at
an absent card makes every `opkg update` fail noisily for nothing.

---

## Things that will bite

- **`/usr/local/bin/kill` is required.** This busybox has no `kill`,
  `killall` or `pkill` applet at all, so it is the only way to signal a
  process. `chunked-deploy.sh` already relied on it existing but nothing
  built it; `tools/build-userspace.sh` now does.
- **The X session has its own PATH.** Nothing graphical reads
  `/etc/profile` — that is a login-shell file. `/etc/init.d/xsession`
  therefore sets PATH and sources `/etc/zaurus-card.sh` itself. Without
  that, a card-installed application gets an icon and then silently does
  nothing when tapped, which is more confusing than no icon at all.
- **`/tmp` is on the jffs2 root, not a tmpfs.** Unpacking a package
  writes to flash twice. Keep packages small.
- **A package cannot be installed to both destinations at once.** opkg
  treats all destinations as one namespace for "is this already
  installed", so installing to the card something already on the NAND is
  refused as a duplicate.
- **The device clock is 1970** with no RTC or NTP, so file timestamps are
  useless for working out what changed when.
- **The SD card is FAT, and FAT has no symlinks.** opkg's `ar`/tar
  extractor drops every symlink member silently — no error, no warning,
  just a missing file. Any payload built on the usual "real versioned
  file plus a `.so` → `.so.N` → `.so.N.M.P` symlink chain" layout (see
  `userspace/stage-sdl-runtime`, e.g. `libfreetype.so.6 ->
  libfreetype.so.6.20.1`) installs to the card with the real file present
  but the SONAME the dynamic linker actually asks for missing, so the
  consuming binary fails to start with no message anywhere near the
  failure. NAND is jffs2 and tolerates the same package fine, which makes
  this easy to miss if it's only tested on NAND.

  Fix at package-build time, not by touching opkg: flatten the chain to
  **one regular file per SONAME** — i.e. the file in the payload is
  already named `libfreetype.so.6`, not `libfreetype.so.6.20.1` with a
  symlink pointing to it. Nothing on the device ever asks for the
  full versioned name, so nothing is lost by dropping it.
