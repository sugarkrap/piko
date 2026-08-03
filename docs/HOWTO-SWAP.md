# Swap: zram in RAM, and the SD card behind it

This machine has 52 MiB of usable RAM and a root filesystem of about
68 MiB. That leaves nowhere on board to put a swap *file*, and it is a
device on which "no swap" is felt: a browser tab, a large image, or
`mplayer` on a big file is the difference between slow and the OOM killer
taking the session.

There are two swap areas, stacked:

1. **`/dev/zram0`, up to 32 MiB (compressed, lives in RAM itself).** Always
   on — brought up by `rcS` at every boot, card or no card. A page that
   compresses even 2:1 is cheaper to keep here than to write anywhere.
2. **`/mnt/card/.zaurus/swap`, 256 MiB, on the SD card.** Comes and goes
   with the card, and is what everything falls back to once zram's fixed
   capacity fills up, or when there is no card-independent memory left to
   compress into.

zram is swapped on at an explicit high priority
(`SWAP_FLAG_PREFER`), so pages always go there first regardless of which
of the two came up first at boot — see `userspace/src/zramswap.c`'s header
for why that is stated outright rather than left to swapon(2)'s default
"earlier call wins" ordering.

## What happens, and when

| Event | What runs | Result |
|---|---|---|
| Every boot, right after `/sys` and `/dev` are mounted | `rcS` → `zramswap on &` | `/dev/zram0` sized, signed, `swapon(2)`'d at high priority — no card needed |
| Card inserted (or `mdev -s` at boot finds one) | `/etc/mdev.conf` → `/usr/sbin/sdcard` → `cardswap on &` | swapfile created if absent, then `swapon(2)` at default (lower) priority |
| Card ejected from the panel applet | `mb-applet-card` → `swapoff(2)`, then `umount(2)` | card's swap off, card safe to pull — zram is untouched |
| Card pulled without asking | `/usr/sbin/sdcard` (remove) → `cardswap off &` | best effort; see the hazard below — zram is untouched |

The panel's system-monitor applet notices on its own — it polls
`/proc/swaps` and `/proc/meminfo`'s `SwapTotal`/`SwapFree`, which the
kernel already sums across every active swap area — and grows a third,
blue bar next to the green CPU one and the red memory one, sized off the
combined total. With just zram it is a smaller blue bar than with a card
in too; with neither it is the two-bar applet it has always been.

## The tools

Two small static binaries, same shape, same reason to exist: **this
busybox has no `mkswap`, no `swapon` and no `swapoff` applet** — the same
hole `kill` and `pkillx` fill. There is no shell path to a swap area on
this device at all, RAM-backed or card-backed.

### `zramswap` (RAM)

`/usr/sbin/zramswap` (source: `userspace/src/zramswap.c`, built by
`tools/build-userspace.sh`, shipped by `tools/chunked-deploy.sh`).

    zramswap on  [MiB]   create/resize /dev/zram0, sign, and swapon(2)
    zramswap off          swapoff(2), then reset the device (frees its RAM)
    zramswap status        exit 0 if currently swapped on

Default is 32 MiB of *uncompressed* capacity — a ceiling, not a
reservation. Real RAM used is whatever the stored pages actually compress
to (zsmalloc allocates as data lands, not up front), which for typical
memory contents is well under that, but incompressible pages (already-
compressed images, audio) can cost close to 1:1, so the default is
deliberately conservative on a ~52 MiB machine rather than tuned to an
optimistic ratio. There is no card-style "reuse the existing one" fast
path to worry about here — resizing zram is a `reset` + a `disksize`
write, both effectively instant, since there is no data to write out.

Needs `CONFIG_ZRAM=y` in the running kernel (`kernel.config-corgi-7.1.4` —
built in, not a module, so `/dev/zram0` exists before `rcS` even runs, no
`modprobe` needed). If it's missing, `zramswap on` says so plainly and the
machine simply runs with card-only swap.

### `cardswap` (SD card)

`/usr/sbin/cardswap` (source: `userspace/src/cardswap.c`, built by
`tools/build-userspace.sh`, shipped by `tools/chunked-deploy.sh`).

    cardswap on  [path [MiB]]   create if needed, sign, and swapon(2)
    cardswap off [path]         swapoff(2); the file is kept
    cardswap status [path]      exit 0 if that file is currently swapped on

Defaults are `/mnt/card/.zaurus/swap` and 256 MiB — sized for a ~512 MiB
card; a smaller card needs the default overridden on the command line (or
`cardswap` will simply refuse for want of room, see below).

Creating the file writes 256 MiB to the card. The 64 MiB predecessor of
this file measured at around 50 seconds (52 s) to write on a typical card;
expect the 256 MiB version to take a few minutes the first time a given
card is used. Doing it again is free either way — an existing file of the
right size is reused, and only its signature page is rewritten, so every
insertion after the first takes about a second. That is why the mdev hook
backgrounds it: mdev handles events one at a time, and minutes of blocking
there stalls PCMCIA and wifi hotplug too.

## Swap on VFAT actually works

This surprises people, so: the kernel writes swap pages straight to the
block device, using an extent map it builds at `swapon(2)` time from the
filesystem's `bmap`. FAT implements it (`fs/fat/inode.c` sets
`.bmap = _fat_bmap`), so `mm/page_io.c`'s `generic_swapfile_activate()`
does the rest. Verified live on the board at the previous 64 MiB size:

    Adding 65532k swap on /mnt/card/.zaurus/swap.  Priority:-1 extents:7 across:70076k

Two conditions follow from doing it that way, and `cardswap` is built
around them:

* **No holes.** Every block must really be allocated, or `swapon` fails
  with `swapon: swapfile has holes`. The file is written out in full with
  `write(2)`; `ftruncate(2)` is never used.
* **The blocks must not move** once swap is on. They will not while the
  filesystem stays mounted, which is all we need.

64 MiB gives 65532 KiB of usable swap, not 65536: the first page is the
signature page. The same holds at 256 MiB, scaled up (262144 KiB total,
minus one page).

zram needs neither condition: `/dev/zram0` is a block device that already
exists in full the moment its `disksize` is set — there is nothing to
write out and nothing that can develop a hole, which is also why
`zramswap` has no `fill_zeroes()` step at all (see its header comment for
the full list of things it does *not* need to do that `cardswap` does).

## The hazard (card only)

Swap on removable media means that pulling the card while pages live on it
loses those pages, and the processes that owned them. Nothing in userspace
can prevent that. zram has no equivalent hazard — it disappears with the
RAM it lives in, i.e. only on a reboot or crash, exactly like every other
page of memory.

What the system does instead is make every *orderly* card removal turn its
swap off first, while the card is still there to page back from:

* **the panel's card applet** — its Eject entry calls `swapoff(2)` before
  unmounting. This is the path to use. It is deliberately synchronous and
  can take a while on a slow card; "safe to remove" must not appear before
  it is true.
* **the mdev remove hook** — a backgrounded `cardswap off`. Backgrounded
  because `swapoff(2)` against a card that is already gone cannot complete,
  and a synchronous call would wedge mdev itself in uninterruptible sleep
  — the same D-state failure that `umount -l` in that hook exists to avoid
  (see `rootfs/usr/sbin/sdcard`).

Neither path touches zram — there is nothing on it that removing the card
puts at risk.

Shutdown deliberately does *not* swapoff the card: `::shutdown:` in
`/etc/inittab` lazy-unmounts the card and powers off, and reading 256 MiB
back off an SD card to then discard it would only make poweroff slow.

## Checking it

    cat /proc/swaps                       # both areas, their size and how much is used
    grep -E 'Swap(Total|Free)' /proc/meminfo
    zramswap status                       # exit code, for scripts
    cardswap status                       # exit code, for scripts
    dmesg | grep -i 'swap on'             # extent count from the card's activation

If card swap does not come up after inserting a card, `/tmp/cardswap.log`
has the last run's output — the hook truncates it on every insertion, so
it never grows. If zram did not come up at boot, `/tmp/zramswap.log` has
that run's output.

The usual reasons `cardswap` refuses:

* **the card is not mounted.** `cardswap` compares the target's filesystem
  against `/`'s and refuses if they match. Without that check a missing
  card would mean writing 256 MiB onto a 68 MiB root jffs2 and filling the
  ROM many times over.
* **not enough room.** It keeps 32 MiB spare on the card beyond the
  swapfile, so enabling swap can never be what fills a user's card up.

The usual reason `zramswap` refuses: **`CONFIG_ZRAM` is not in the running
kernel**, so `/sys/block/zram0/disksize` does not exist. `zramswap on`
says so plainly rather than failing silently.
