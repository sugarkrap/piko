# Dead Letter — WiFi + SSH deliverable (service stack, users, wpa_supplicant)

*Written 2026-07-22. Builds on the working two-stage boot
(`../DEADLETTER-KEXEC-SYSCALL.md`, `DEADLETTER-KEXEC-ATAGS.md`,
`DEADLETTER-STAGE2-INIT.md`).*

---

## What this adds

- **Real busybox on stage 2.** The previous stage-2 busybox was nearly
  bare (inherited from the bootstrap's minimal config) — no
  `insmod`/`modprobe`, no `ifconfig`/`ip`, no `passwd`/`adduser`. Rebuilt
  with the full applet set needed for modules + networking + users.
- **`bb_syscall.c` shim** (`initramfs/busybox-1.36.1/libbb/bb_syscall.c`):
  this uClibc has no `syscall()` at all (busybox's `insmod`/`rmmod` need
  it), and separately, `__NR_*` macros here can resolve OABI-numbered —
  the same root cause as the kexec_load SIGILL
  (`../DEADLETTER-KEXEC-SYSCALL.md`). The shim provides `syscall()` and
  masks the number back to a bare EABI value (`n & 0x000fffff`) before
  `svc 0`. Verified in the disassembly.
- **Service stack: BusyBox init, not systemd.** `/init` → `exec
  /sbin/init` → `/etc/inittab` (sysinit → `/etc/init.d/rcS`; respawn getty
  on tty1) → `rcS` mounts proc/sys/devtmpfs/devpts, starts `mdev -d`
  (hotplug daemon), loads the PCMCIA platform stack, starts dropbear.
  systemd was never a real option on 64MB RAM/400MHz/uClibc.
- **Users:** `root`/`zaurus` (ash), `piko`/`piko` (zsh, `/home/piko` with
  `.zshrc`/`.zprofile`). Both `passwd`-changeable. SHA-512 crypt hashes
  generated with `openssl passwd -6`.
- **Kernel modules rebuilt fresh** from the actual stage-2 `.config` into
  `/lib/modules/7.1.4` + `depmod` (host `depmod`/kmod works cross-arch —
  it only parses ELF): PCMCIA stack (`pcmcia_core`, `pcmcia`,
  `pcmcia_rsrc`, `soc_common`, `pxa2xx_base`, `pxa2xx_sharpsl`),
  `hostap`/`hostap_cs`, `lib80211` (+ WEP/TKIP/CCMP crypto), `g_ether`
  (USB ethernet gadget, bonus). 23 modules, ~1MB total.
- **wireless-tools 29** (`iwconfig`/`iwlist`/etc.) — cross-built static,
  no surprises.
- **wpa_supplicant 2.10**, WEXT driver backend (matches the hostap driver
  — it predates cfg80211/nl80211), `CONFIG_TLS=internal` +
  `CONFIG_INTERNAL_LIBTOMMATH=y` (avoids needing external openssl/tommath
  cross-builds). WPA-PSK/TKIP + WEP only — this hardware/driver has no
  CCMP/AES association support, so WPA2-CCMP/WPA3 are out of scope by
  hardware, not by choice.
- **Auto-connect wiring:** `mdev.conf`'s `$MODALIAS` rule auto-modprobes
  `hostap_cs` (+ deps, via `modules.dep`) when a card is inserted; a
  `wlan0` rule then runs `/etc/wifi-up.sh` (`wpa_supplicant` + `udhcpc`)
  once the interface appears. Network config lives in
  `/etc/wpa_supplicant/wpa_supplicant.conf` (edit with real SSID/PSK).

## Gotchas hit building this

1. **wpa_supplicant's Makefile uses `ifdef` for feature flags, not value
   checks.** Setting `CONFIG_EAP_TLS=n` etc. does NOT disable it — `ifdef`
   is true for any *defined* variable, including the string `"n"`. Must
   *omit* the line entirely to disable a feature. Cost a full rebuild
   cycle (pulled in the entire TLSv1/X.509 stack + wanted `libtommath`
   before this was caught).
2. **`CONFIG_TLS=internal` still wants `-ltommath`** for bignum/DH-group
   support unless `CONFIG_INTERNAL_LIBTOMMATH=y` is also set (uses
   wpa_supplicant's own bundled implementation instead of linking an
   external static lib we don't have cross-built).
3. **Passing `CFLAGS=`/`LDFLAGS=` on the `make` command line clobbers
   `+=` accumulation in the Makefile** (same class of bug as the
   `bin-to-hex` cross-compile trap in `kexec-tools`, see
   `docs/DEADLETTER-*` toolchain notes) — wpa_supplicant's Makefile
   builds up `-I../src` etc. via `+=`; command-line `CFLAGS=` wiped that
   and broke every include path. Fixed by using `EXTRA_CFLAGS=` (an
   explicit hook the Makefile appends) instead, and only overriding
   `LDFLAGS=` directly after confirming nothing else populates it for a
   WEXT-only build.
4. **`pxa2xx_sharpsl.ko` doesn't list `pxa2xx_base` as a module
   dependency** in `depmod`'s output, despite needing it at runtime
   (likely resolved via platform-device registration, not a direct
   symbol reference `depmod` can see) — `rcS` loads both explicitly
   rather than trusting automatic dependency resolution alone.

## Known follow-ups

- **2026-07-26 update:** real-hardware testing found the actual gap —
  a kernel rebuild was deployed without redeploying the WiFi/PCMCIA
  modules alongside it, breaking WiFi/SSH via a module ABI mismatch
  ("section size must match"). Fixed and documented in
  `docs/HOWTO-BUILD-DEPLOY-KERNEL.md`, which is now the canonical
  kernel+module build/deploy procedure — use `flash/build-and-deploy.sh`
  for every future stage-2 kernel update, never a partial redeploy.
- **Not yet tested on real hardware** — this is the first flash of the
  full stack; module loading, PCMCIA card detection, and the WPA
  handshake are all unverified beyond "it built."
- `wpa_supplicant.conf` ships with only commented-out example networks —
  needs a real SSID/PSK edited in before it does anything.
- `wpa_passphrase` wasn't built (needs external `libtommath`, unlike
  `wpa_supplicant` itself which uses the internal one) — not required,
  since `wpa_supplicant.conf` accepts a plain `psk="passphrase"` and
  hashes it itself at connect time.
- `usb0` (`g_ether`) is built as a module now but not yet wired into
  `rcS`/`mdev` the way wlan0 is.
