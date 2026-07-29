# Handoff: Xfbdev/X11 bring-up (2026-07-28)

## Scope
This handoff covers the current state of the legacy X stack work for Zaurus:
- `xserver` (kdrive + `Xfbdev` path)
- `libX11`
- `libXfont`
- `xtrans`
- `matchbox-window-manager` standalone mode

It also captures recent kernel build evidence from `/tmp/kbuild-20260728-210455.log`.

## Current Workspace State
From `git status --short`:
- `M  .gitmodules`
- ` M tools/build-and-deploy.sh`
- ` M tools/chunked-deploy.sh`
- `AM userspace/src/libX11`
- `AM userspace/src/libXfont`
- ` m userspace/src/matchbox-window-manager`
- `A  userspace/src/xorg-macros`
- `AM userspace/src/xserver`
- `AM userspace/src/xtrans`
- `?? userspace/stage-host/`

## Submodule SHAs (important)
From `git submodule status --recursive`:
- `userspace/src/libX11` -> `fc74dc12b1ff3c43e240e1a713316ce1bf525d61` (`libX11-1.4.4`)
- `userspace/src/libXfont` -> `7d246751628bb877e04da762ec1a2e41ffa62154` (`libXfont-1.5.4`)
- `userspace/src/matchbox-window-manager` -> `f77676335d563abbea7146b2d73381523325e666`
- `userspace/src/xorg-macros` -> `a9d71e3fd8e6758b70be31c586921bbbcd2a8449` (`util-macros-1.20.2`)
- `userspace/src/xserver` -> `5db8aa3f8495223080e06b420eb02628c9b7959d` (`xorg-server-1.10.6`)
- `userspace/src/xtrans` -> `cf05ba4a10c90da2c63805a5375e983b174e28b0` (`xtrans-1.6.0`)

## Verified So Far
1. Kernel build succeeded through `zImage` generation in `/tmp/kbuild-20260728-210455.log`.
2. `xserver` configure succeeded for minimal Xfbdev-oriented mode when disabling nonessential server variants/features.
3. `matchbox-window-manager` standalone build succeeded after compatibility fixes.
4. `Xfbdev` binary link succeeded when invoked in `hw/kdrive/fbdev` with staged include/lib search paths.

## Local Compatibility Edits Already Present
### xserver
- `userspace/src/xserver/m4/fontutil-compat.m4` adds local macro definitions:
	- `XORG_FONT_MACROS_VERSION`
	- `XORG_FONTROOTDIR`
	- `XORG_FONTSUBDIR`

### libX11
- `userspace/src/libX11/nls/Makefile.am` adjusted test rule to avoid modern automake `$(srcdir)`-in-`TESTS` issue:
	- `TESTS = compose-check.pl`
	- explicit copy rule from `$(srcdir)/compose-check.pl`

### matchbox-window-manager
- `userspace/src/matchbox-window-manager/configure.ac` includes a no-op fallback for missing GConf macro and condition:
	- `AM_GCONF_SOURCE_2`
	- `GCONF_SCHEMAS_INSTALL` false conditional
- `userspace/src/matchbox-window-manager/src/misc.c` includes `<sys/wait.h>`
- `userspace/src/matchbox-window-manager/src/keys.c` includes `<ctype.h>`

## Staging Layout (host-side)
Local host staging tree used to satisfy legacy include/lib expectations:
- `userspace/stage-host/usr/local/include/X11/Xtrans/...`
- `userspace/stage-host/usr/local/lib/libXfont.*`
- pkg-config files under:
	- `userspace/stage-host/usr/local/share/pkgconfig` (xtrans)
	- `userspace/stage-host/usr/local/lib/pkgconfig` (xfont)

## Critical Build Notes
1. `xtrans-1.2.7` caused compile-time type mismatch in `xstrans` on modern GCC.
2. Upgrading to `xtrans-1.6.0` resolved that class of failure.
3. Full top-level `xserver` build still requires explicit include/lib path propagation because legacy build system assumes system-installed deps.

## Repro Commands (host validation)
Run from repo root unless noted.

### 1) Build+stage xtrans
```sh
cd userspace/src/xtrans
ACLOCAL_PATH=$PWD/../xorg-macros NOCONFIGURE=1 ./autogen.sh
make -j1
make -j1 install DESTDIR=/home/vodkannelle/Code/piko/userspace/stage-host
```

### 2) Build+stage libXfont
```sh
cd userspace/src/libXfont
PKG_CONFIG_PATH=/home/vodkannelle/Code/piko/userspace/stage-host/usr/local/share/pkgconfig \
CPPFLAGS='-I/home/vodkannelle/Code/piko/userspace/stage-host/usr/local/include' \
make -j1

PKG_CONFIG_PATH=/home/vodkannelle/Code/piko/userspace/stage-host/usr/local/share/pkgconfig \
CPPFLAGS='-I/home/vodkannelle/Code/piko/userspace/stage-host/usr/local/include' \
make -j1 install DESTDIR=/home/vodkannelle/Code/piko/userspace/stage-host
```

### 3) xserver full build attempt (minimal warnings policy)
```sh
cd userspace/src/xserver
PKG_CONFIG_PATH=/home/vodkannelle/Code/piko/userspace/stage-host/usr/local/lib/pkgconfig:/home/vodkannelle/Code/piko/userspace/stage-host/usr/local/share/pkgconfig \
CPPFLAGS='-I/home/vodkannelle/Code/piko/userspace/stage-host/usr/local/include' \
LDFLAGS='-L/home/vodkannelle/Code/piko/userspace/stage-host/usr/local/lib' \
make -j1 CWARNFLAGS='-Wall -Wno-error'
```

### 4) Direct Xfbdev link that succeeded
```sh
cd userspace/src/xserver/hw/kdrive/fbdev
LIBRARY_PATH=/home/vodkannelle/Code/piko/userspace/stage-host/usr/local/lib make -j1 Xfbdev
```

Produced binary:
- `userspace/src/xserver/hw/kdrive/fbdev/Xfbdev`

## Known Open Items For Next Agent
1. Convert host-side validation into ARM cross-compile flow for:
	 - `xtrans`, `libXfont`, `libX11`, `xserver` (`Xfbdev`)
2. Stage/install target-side runtime tree in rootfs packaging format.
3. Integrate launch path (`Xfbdev` + `matchbox-window-manager`) into init/login flow on device.
4. Investigate why `tools/build-and-deploy.sh` currently exits `1` after successful kernel compile (likely post-build deploy step/network/auth path).

## Suggested Immediate Next Command
Start by capturing the failing deploy tail for exact cause:
```sh
cd /home/vodkannelle/Code/piko/tools
TOOLCHAIN_BIN_DIR=/home/vodkannelle/Code/piko/toolchain/arm-buildroot-linux-uclibcgnueabi/bin \
CROSS_COMPILE=arm-buildroot-linux-uclibcgnueabi- \
./build-and-deploy.sh --adapter wlan0 root@10.208.47.72 --force-kernel-src \
2>&1 | tail -n 200
```

