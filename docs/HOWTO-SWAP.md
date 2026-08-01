# Swap on the SD card

This machine has 52 MiB of usable RAM and a root filesystem of about
68 MiB. That leaves nowhere on board to put a swap area, and it is a
device on which "no swap" is felt: a browser tab, a large image, or
`mplayer` on a big file is the difference between slow and the OOM killer
taking the session.

So the swap lives on the SD card, at `/mnt/card/.zaurus/swap`, 64 MiB, and
it comes and goes with the card.

## What happens, and when

| Event | What runs | Result |
|---|---|---|
| Card inserted (or `mdev -s` at boot finds one) | `/etc/mdev.conf` → `/usr/sbin/sdcard` → `cardswap on &` | swapfile created if absent, then `swapon(2)` |
| Card ejected from the panel applet | `mb-applet-card` → `swapoff(2)`, then `umount(2)` | swap off, card safe to pull |
| Card pulled without asking | `/usr/sbin/sdcard` (remove) → `cardswap off &` | best effort; see the hazard below |

The panel's system-monitor applet notices on its own — it polls
`/proc/swaps` — and grows a third, blue bar next to the green CPU one and
the red memory one. No card, or a card whose swapfile did not come up, and
it is the two-bar applet it has always been.

## The tool

`/usr/sbin/cardswap` (source: `userspace/src/cardswap.c`, built by
`tools/build-userspace.sh`, shipped by `tools/chunked-deploy.sh`).

    cardswap on  [path [MiB]]   create if needed, sign, and swapon(2)
    cardswap off [path]         swapoff(2); the file is kept
    cardswap status [path]      exit 0 if that file is currently swapped on

Defaults are `/mnt/card/.zaurus/swap` and 64 MiB.

It exists as a binary because **this busybox has no `mkswap`, no `swapon`
and no `swapoff` applet** — the same hole `kill` and `pkillx` fill. There
is no shell path to a swap area on this device at all.

Creating the file writes 64 MiB to the card and takes around 50 seconds on
a typical card (measured: 52 s). Doing it again is free — an existing file
of the right size is reused, and only its signature page is rewritten, so
every insertion after the first takes about a second. That is why the mdev
hook backgrounds it: mdev handles events one at a time, and a minute of
blocking there stalls PCMCIA and wifi hotplug too.

## Swap on VFAT actually works

This surprises people, so: the kernel writes swap pages straight to the
block device, using an extent map it builds at `swapon(2)` time from the
filesystem's `bmap`. FAT implements it (`fs/fat/inode.c` sets
`.bmap = _fat_bmap`), so `mm/page_io.c`'s `generic_swapfile_activate()`
does the rest. Verified live on the board:

    Adding 65532k swap on /mnt/card/.zaurus/swap.  Priority:-1 extents:7 across:70076k

Two conditions follow from doing it that way, and `cardswap` is built
around them:

* **No holes.** Every block must really be allocated, or `swapon` fails
  with `swapon: swapfile has holes`. The file is written out in full with
  `write(2)`; `ftruncate(2)` is never used.
* **The blocks must not move** once swap is on. They will not while the
  filesystem stays mounted, which is all we need.

64 MiB gives 65532 KiB of usable swap, not 65536: the first page is the
signature page.

## The hazard

Swap on removable media means that pulling the card while pages live on it
loses those pages, and the processes that owned them. Nothing in userspace
can prevent that.

What the system does instead is make every *orderly* removal turn swap off
first, while the card is still there to page back from:

* **the panel's card applet** — its Eject entry calls `swapoff(2)` before
  unmounting. This is the path to use. It is deliberately synchronous and
  can take a while on a slow card; "safe to remove" must not appear before
  it is true.
* **the mdev remove hook** — a backgrounded `cardswap off`. Backgrounded
  because `swapoff(2)` against a card that is already gone cannot complete,
  and a synchronous call would wedge mdev itself in uninterruptible sleep
  — the same D-state failure that `umount -l` in that hook exists to avoid
  (see `rootfs/usr/sbin/sdcard`).

Shutdown deliberately does *not* swapoff: `::shutdown:` in `/etc/inittab`
lazy-unmounts the card and powers off, and reading 64 MiB back off an SD
card to then discard it would only make poweroff slow.

## Checking it

    cat /proc/swaps                       # the area, its size and how much is used
    grep -E 'Swap(Total|Free)' /proc/meminfo
    cardswap status                       # exit code, for scripts
    dmesg | grep -i 'swap on'             # extent count from the activation

If swap does not come up after inserting a card, `/tmp/cardswap.log` has
the last run's output — the hook truncates it on every insertion, so it
never grows.

The usual reasons it refuses:

* **the card is not mounted.** `cardswap` compares the target's filesystem
  against `/`'s and refuses if they match. Without that check a missing
  card would mean writing 64 MiB onto a 68 MiB root jffs2 and filling the
  ROM.
* **not enough room.** It keeps 8 MiB spare on the card beyond the
  swapfile, so enabling swap can never be what fills a user's card up.
