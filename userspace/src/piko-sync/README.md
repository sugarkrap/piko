# piko-sync — resilient file transfer, with a build/deploy front end

A small custom client/server pair for getting files onto a Sharp Zaurus
SL-C760/C860 running the [piko](https://github.com/sugarkrap/piko) ROM,
built because SFTP (dropbear's `sftp-server`) has no hook for a GUI to
show live progress, and no resume when the device's flaky WiFi drops
mid-transfer. FLTK 1.3, C++98.

Unlike `pikostore`, this is **not** a git submodule — it lives directly
in the `piko` tree at `userspace/src/piko-sync`, built and shipped by
`tools/userspace/build-piko-sync.sh` there.

## The two apps

**`piko-sync-server`** runs *on the Zaurus*. It shows the device's own
WiFi address and listens for connections; incoming files land in
`/mnt/card/Transfers`. Cross-compiled against the staged FLTK/X11 tree,
same as `pikostore`.

**`piko-sync-client`** runs on the host. It queues files to send (a
FileZilla-style list: queued / sending / reconnecting / done / error,
each with its own progress bar, plus one aggregate bar for the whole
queue) and, on a second tab, is a GUI front end for the existing
`tools/build-and-deploy.sh` — a button, some of that script's own flags
as checkboxes, a live log, and a step progress bar. It shells out to the
script unmodified; nothing about the build/deploy logic is reimplemented
here. Built against the **host's own** FLTK (`fltk-config`), not the
cross-compiled one.

## The wire protocol

See `protocol.h` for the actual message definitions. The two ideas worth
knowing before reading the code:

- **Byte count is the source of truth for resume, not a session id.**
  The server always knows exactly how many bytes of a file it has
  durably written; a client that reconnects just re-offers the same
  file and is told where to seek to. No cookie, no client-side resume
  state needed — see `chunked-deploy.sh` in the piko repo for the same
  philosophy applied to one-shot `scp` transfers.
- **A whole-file CRC32 at the end, not per-chunk hashing.** Every chunk
  already crosses the wire inside TCP, which checksums it in flight; the
  CRC32 only has to catch what TCP can't (corruption after a byte was
  already durable). A mismatch means "resend the whole file," not
  "resend one bad chunk" — again, `chunked-deploy.sh`'s tradeoff.

Resume is scoped to **within one running server process**: reconnecting
after a dropped WiFi link resumes correctly; a server *restart* does not
remember partial transfers (matching the wire's own byte-count-on-disk
model — see `transfer_state.h`'s header comment for why that is a
deliberate limit, not an oversight).

"Share parts" (client-side) means queuing several whole files at once,
each over its own connection with independent resume — **not** splitting
one large file across parallel connections. That would be a reasonable
follow-up (protocol.h's frame format doesn't preclude it) but is out of
scope here.

## Building

Normally:

```sh
tools/userspace/build-piko-sync.sh                    # both apps
tools/userspace/build-piko-sync.sh --server-only
tools/userspace/build-piko-sync.sh --client-only
tools/userspace/build-piko-sync.sh --deploy user@host # also installs the .ipk onto the SD card
```

Directly:

```sh
make server STAGE=/path/to/userspace/stage-target \
            CXX=arm-unknown-linux-uclibcgnueabi-g++
make client HOST_FLTK_CXXFLAGS="$(fltk-config --cxxflags)" \
            HOST_FLTK_LDFLAGS="$(fltk-config --ldflags)"
make test   # protocol/transfer_state/transfer_queue/settings, no FLTK, no device
```

Run `piko-sync-client` from the piko repo root (or set
`PIKO_SYNC_REPO_ROOT`, or point Settings... → Repo at it) so its Build &
Deploy tab can find `tools/build-and-deploy.sh`.

## Client settings

`piko-sync-client` remembers everything you can set in it —  the Zaurus
address and the last directory you added files from, plus the Build &
Deploy tab's adapter, target, staging destination, every flag checkbox,
and the repo/toolchain/jobs paths from Settings... — in:

```
$HOME/.config/piko-sync/settings.cfg      # or $XDG_CONFIG_HOME/piko-sync/
```

Plain `key = value` lines, safe to hand-edit while the app is not
running; see `settings.h` for the format's (deliberate) limits. Keys the
running build doesn't recognize are written back untouched rather than
dropped, so an older client won't quietly eat a newer one's settings.

Three things are worth knowing:

- **It is written on exit, on OK in the Settings dialog, and when a
  build is launched** — not on every keystroke. A `kill -9` (or a
  machine that goes down mid-build) loses whatever changed since the
  last of those.
- **`PIKO_SYNC_REPO_ROOT` still wins over the saved Repo**, since it is
  a deliberate per-launch override. It is also never written back into
  the file, so exporting it for one run doesn't silently become your
  permanent setting.
- **Adapter and Staging are stored by name, not by menu position** ("`wlan0`",
  "`sd`"), and a saved name that no longer exists just leaves the
  default selected — a USB tether that isn't plugged in this time
  can't shift the selection onto some unrelated interface.

The transfer queue itself is not saved: a partial file's resume offset
lives in the *server* process (see `transfer_state.h`), so a queue
restored across launches would show rows that could not actually
resume.

## Notes

- The server runs as root, same reason `pikostore` does: the X session
  starts from `inittab` and inherits its uid.
- `/mnt/card/Transfers` is created on first run if missing. If the SD
  card isn't mounted/writable, the window says so instead of silently
  failing to receive anything.
- A name collision on `/mnt/card/Transfers` (a fresh offer that doesn't
  match an in-progress or already-finished transfer) gets `" (1)"`,
  `" (2)"`, ... appended before the extension — the same convention
  browsers use for downloads.
