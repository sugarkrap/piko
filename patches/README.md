# Kconfig/Makefile wiring patches

`flash/setup-kernel-src.sh` reconstructs `kernel-src/linux-7.1.4/` from a
pristine kernel.org tarball plus every hand-patched *whole file* this repo
already tracks (`corgi_patched.c`, `modules/w100/*`, `modules/nand/*`, `modules/hostap/*`).
Those are unambiguous — the upstream file was deleted, so the tracked copy
is just dropped in wholesale.

What's **not** captured anywhere in git yet is the handful of small,
incremental edits to upstream files that still exist in a pristine
7.1.4 tree:

- `arch/arm/mach-pxa/{Kconfig,Makefile}` — the `MACH_CORGI`/`SHEPHERD`/
  `HUSKY` board entries and `PXA_SHARP_C7xx` (see README.md's "What's
  still open" section: *"Kconfig/Makefile wiring ... is done and
  confirmed working"* — done on the machine that built
  `zImage-corgi-7.1.4`, just never exported as a patch file here).
- `net/wireless/{Kconfig,Makefile}` — re-adding `lib80211` as a standalone
  subsystem.
- `crypto/{Kconfig,Makefile}` — re-adding `CRYPTO_MICHAEL_MIC`.

Until the two patch files below exist, `flash/setup-kernel-src.sh` exits 2
(not 1 — this is a known, documented gap, not a bug) after applying
everything it can, with a message pointing back here.

## What to generate

Two files, both plain unified diffs, applied with `patch -p1 -d
kernel-src/linux-7.1.4`:

- `patches/mach-pxa-corgi-kconfig.patch` — covers
  `arch/arm/mach-pxa/{Kconfig,Makefile}`.
- `patches/wireless-lib80211-kconfig.patch` — covers
  `net/wireless/{Kconfig,Makefile}` and `crypto/{Kconfig,Makefile}`.

## How to generate them

On the machine with the already-working `kernel-src/linux-7.1.4/` (the one
`zImage-corgi-7.1.4` was actually built from):

```sh
# Extract a second, untouched copy of the exact same tarball to diff against.
mkdir -p /tmp/pristine && cd /tmp/pristine
curl -LO https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.1.4.tar.xz
tar xf linux-7.1.4.tar.xz

cd /path/to/zaurus-refresh   # this repo

diff -u /tmp/pristine/linux-7.1.4/arch/arm/mach-pxa/Kconfig \
        kernel-src/linux-7.1.4/arch/arm/mach-pxa/Kconfig \
  > patches/mach-pxa-corgi-kconfig.patch || true
diff -u /tmp/pristine/linux-7.1.4/arch/arm/mach-pxa/Makefile \
        kernel-src/linux-7.1.4/arch/arm/mach-pxa/Makefile \
  >> patches/mach-pxa-corgi-kconfig.patch || true

diff -u /tmp/pristine/linux-7.1.4/net/wireless/Kconfig \
        kernel-src/linux-7.1.4/net/wireless/Kconfig \
  > patches/wireless-lib80211-kconfig.patch || true
diff -u /tmp/pristine/linux-7.1.4/net/wireless/Makefile \
        kernel-src/linux-7.1.4/net/wireless/Makefile \
  >> patches/wireless-lib80211-kconfig.patch || true
diff -u /tmp/pristine/linux-7.1.4/crypto/Kconfig \
        kernel-src/linux-7.1.4/crypto/Kconfig \
  >> patches/wireless-lib80211-kconfig.patch || true
diff -u /tmp/pristine/linux-7.1.4/crypto/Makefile \
        kernel-src/linux-7.1.4/crypto/Makefile \
  >> patches/wireless-lib80211-kconfig.patch || true
```

(`diff -u` exits 1 when it finds differences — the `|| true` is just so
the shell doesn't stop there; each command still writes the diff before
exiting nonzero.)

Then sanity-check both patches apply cleanly against a *fresh* pristine
tree before committing them:

```sh
rm -rf /tmp/apply-test && cp -r /tmp/pristine/linux-7.1.4 /tmp/apply-test
patch -p1 -d /tmp/apply-test < patches/mach-pxa-corgi-kconfig.patch
patch -p1 -d /tmp/apply-test < patches/wireless-lib80211-kconfig.patch
```

Once both are committed, `flash/setup-kernel-src.sh` applies them
automatically and exits 0 with a fully buildable tree instead of exiting 2.
