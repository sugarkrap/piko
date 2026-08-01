# Software Center (`pikostore`), the ROM manifest, and update history

*Written 2026-08-01, when the ROM got a GUI for updating itself. Companion
to `docs/HOWTO-OFFLINE-UPDATE.md`, which covers `piko-update` — the thing
this drives — and `docs/HOWTO-FLTK.md`, which covers the toolkit it is
built on.*

The short version:

```sh
tools/build-fltk.sh                    # toolkit (once)
tools/build-pikostore.sh               # the app
tools/build-matchbox-payload.sh --deploy --adapter wlan0 root@<ip>
# then on the device: the "Software Center" icon under System Tools
```

---

## What it is

`pikostore` is an FLTK app living in its own repo
([sugarkrap/pikostore](https://github.com/sugarkrap/pikostore)), tracked
here as a submodule at `userspace/src/pikostore`. It is the first GUI in
this ROM that does real work rather than proving a library loads.

Two tabs:

- **Packages** — a stub. This becomes an ipkg front end later.
- **System Update** — the working half, and the last tab.

System Update shows the running kernel, this ROM's version and changelog,
a table of every update ever installed with the tool, and an **Update…**
button that installs a package picked off the SD card.

If `/etc/zaurus/manifest` is missing or unreadable, the changelog area
says *"oups, no changelog found, sowy"*. **That typo is deliberate.** It
is in `pikostore.cxx` with a comment saying so; please leave it alone.

---

## The ROM manifest

Every package built by `flash/build-update-package.sh` now carries
`etc/zaurus/manifest`, generated at build time by
`tools/gen-rom-manifest.sh`:

```
PIKO-ROM-MANIFEST 1
version: r148
built: 2026-08-01T09:12:00Z
commit: 224c038

Software Center arrives, with a system update tab that can install
piko.tar packages straight from the SD card.
```

An email-style header block, **one blank line**, then the changelog as
free prose. That blank-line split is the entire parser contract, and it is
what lets a changelog paragraph contain colons and punctuation without any
escaping or flattening onto one line.

It cannot live in `rootfs/` like every other config file — a tracked copy
would be stale the moment it was committed.

### Versioning

The version is the **git commit count**, rendered `rN`:

| tree state | version | meaning |
|---|---|---|
| clean | `r148` | exactly commit 148 |
| dirty | `r148+` | uncommitted work — **does not** match that commit |
| no git | `r0` | tarball export, commit shows `unknown` |

This auto-increments on every commit with no tracked counter to bump, no
build-time file mutation, and no way for a CI build and a local build of
the same tree to disagree — all of which a checked-in `VERSION` file gets
wrong. It also maps straight back to a commit, which a date-based scheme
does not.

The `+` matters. A ROM built from uncommitted work is not the `rN` its
commit count claims, and silently shipping it as if it were is how you end
up unable to reproduce a device's exact state later.

### The changelog

Write it in `CHANGELOG` at the repo root. The generator takes the **first
paragraph** — everything from the first non-blank line to the first blank
line. Older entries live below, separated by blank lines; a `--` line ends
the live section.

Keep it to one short paragraph. It is read on a 640×480 screen, in a
scrolling box, by someone deciding whether to install.

---

## Update history

`piko-update` appends one line to `/etc/zaurus/update-history` after every
successful install:

```
r148|2026-08-01T09:20:11Z|update.tar
```

Pipe-delimited, because a version, a timestamp and a filename can all
plausibly contain spaces but not pipes, and this gets parsed by a C++ app
with no CSV library on a device with no python to repair things.

The table shows it newest-first. The **Revert** button in each row is
always disabled: reverting needs the previous package kept somewhere, and
nothing does that yet. It is drawn rather than being a real widget, so
making it live later means replacing one `draw_cell` branch.

A malformed line is skipped rather than being fatal — this file
accumulates across every update the device ever takes, and one bad line
must not hide the rest of the history.

---

## How the app talks to piko-update

Two pipes, not one:

| stream | fd | carries |
|---|---|---|
| stdout + stderr | 1, 2 | human text, shown verbatim in the console box |
| progress | 3 | `TOTAL` / `PROGRESS` / `STATUS` / `DONE` records |

```
TOTAL <n>                    file count, once, before any PROGRESS
PROGRESS <phase> <done> <n>  phase is verify | install | smf
STATUS <free text...>        human-readable one-liner, may repeat
DONE <exit-code>             always last, even on failure
```

Enabled with `piko-update <pkg> --progress-fd 3`. You can watch it from a
shell:

```sh
piko-update /mnt/card/update.tar --dry-run --progress-fd 3 3>&1 >/dev/null
```

**Why separate.** The console box shows stdout verbatim — no filter,
nothing hidden. Mixing progress markers into it would mean either showing
them to the user or maintaining a stripper here that has to stay in step
with every message `piko-update` ever prints.

**Unknown records are ignored**, deliberately, so a newer `piko-update` can
add records without breaking an older `pikostore`.

**`DONE` is emitted on every exit path**, including `die()`. A GUI holding
a progress bar has no other way to tell "failed" from "still working".

### Why `--no-reboot`

`piko-update` reboots the instant it succeeds. That is right from a shell
and wrong from a GUI: it tears down the window showing the progress before
anyone can read it, and makes success indistinguishable from a crash. The
app always passes `--no-reboot` and offers a **Reboot now** button.

---

## Building

`tools/build-pikostore.sh` needs `tools/build-fltk.sh` to have run (it
links against the staged `libfltk`) and fails loudly rather than skipping
if it hasn't. It also runs the host-side tests on every build.

```sh
tools/build-pikostore.sh
PIKOSTORE_SKIP_TESTS=1 tools/build-pikostore.sh    # iterating on the GUI
```

**In a worktree**, the toolchain and the X11 stage live in the main
checkout (both are gitignored build output), so point at them:

```sh
STAGE=/path/to/piko/userspace/stage-target \
TOOLCHAIN_BIN_DIR=/path/to/piko/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/bin \
tools/build-pikostore.sh
```

### Tests

`userspace/src/pikostore/tests/romstate-test.cxx` covers the manifest and
history parsing and the progress protocol. It compiles with a plain host
`g++` — no FLTK, no X, no `/etc/zaurus` — which is the whole point: it is
the only part of this app that can be exercised before it reaches the one
spare board. CI runs it on every push.

```sh
g++ -O2 -Wall -Wextra -o /tmp/rt userspace/src/pikostore/tests/romstate-test.cxx && /tmp/rt
```

---

## Notes

- Runs as **root**: the X session starts from `inittab` and inherits its
  uid, which is what `piko-update` needs to write `/etc` and `/boot`.
- The **Update…** file chooser defaults to `/mnt/card`. Typing a path is
  close to impossible on this keyboard — it cannot produce `/` (AGENTS.md).
- An update that stages an `smf`/bootstrap write does **not** flash it. The
  progress bar's `smf` phase is the compare-and-stage step only; the actual
  NAND write is still the separate, manual `smfcommit` after a proven
  reboot. See `docs/HOWTO-OFFLINE-UPDATE.md`.
