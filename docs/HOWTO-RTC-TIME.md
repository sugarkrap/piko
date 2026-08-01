# The real-time clock

Until this change the board came up at **00:00 on 1 January 1970** on every
single boot. The hardware was fine and the kernel plumbing was fine; the
driver simply never ran. This is what was wrong, what changed, and how to
drive the clock from the device.

## Using it

Everything is one word, and none of it needs a `/` or a `:` typed on the
Zaurus keyboard (the same constraint that produced `bright`, `wifiup` and
`audioon` — see `AGENTS.md`):

```
settime                       show the system clock and the RTC
settime 2026 8 1 14 30        set to 2026-08-01 14:30:00
settime 2026 8 1 14 30 15     ... with seconds
settime ntp                   set from the network, then save
settime save                  system clock -> RTC   (persist)
settime load                  RTC -> system clock
```

Setting the time writes **both** the RTC and the running system clock,
because doing only one is never what anyone wants: write only the RTC and
nothing changes until the next boot; write only the system clock and it is
forgotten at the next boot.

Underneath are two binaries, usable directly if you want the util-linux
spelling:

```
hwclock                       show the RTC
hwclock -s                    RTC -> system clock      (hctosys)
hwclock -w                    system clock -> RTC      (systohc)
hwclock --set --date="2026-08-01 14:30:00"
hwclock --setfields 2026 8 1 14 30 0

ntpsync                       SNTP sync, then write the RTC
ntpsync -n                    system clock only, leave the RTC alone
ntpsync -q                    quiet (what wifi-up.sh uses)
ntpsync time.example.org      use a specific server
```

`wifi-up.sh` runs `ntpsync -q` in the background once wlan0 is addressed,
so a board that has been on WiFi has the right time without anyone asking.
Set `NTP_SYNC="no"` at the top of that file to turn it off.

## What was actually broken

Everything needed was already in the tree except one character.

`pxa25x_devices[]` in `modules/mach-pxa/pxa25x_patched.c` registers
`&sa1100_device_rtc`, and `modules/clk-pxa/clk_pxa25x_patched.c` gives it
its 32.768 kHz `DUMMY_CLK`. The platform device therefore existed, with
the MEM window at `0x40900000` and the named IRQ resources (`rtc 1Hz`,
`rtc alarm`) that `rtc-sa1100.c`'s probe looks for. The config had
`CONFIG_RTC_CLASS=y`, `CONFIG_RTC_HCTOSYS=y` and
`CONFIG_RTC_HCTOSYS_DEVICE="rtc0"`.

But the driver itself was **`CONFIG_RTC_DRV_SA1100=m`**, and nothing ever
loaded it:

- `tools/kernel-modules.sh` ships only the audio, WiFi/PCMCIA and SD/VFAT
  module sets to the device. `rtc-sa1100.ko` was in none of them, so the
  file was not even on the board.
- `/etc/init.d/rcS` had no `modprobe` for it.
- The `$MODALIAS` auto-load rule in `/etc/mdev.conf` fires on hotplug
  uevents; the RTC is a static platform device registered during early
  boot, not something that gets plugged in later.

So the platform device sat there forever with no driver bound, there was
no `/dev/rtc0`, and `rtc_hctosys()` — which in 7.1.4 is called from
`__devm_rtc_register_device()` in `drivers/rtc/class.c`, i.e. only when an
RTC actually registers — never ran. The system clock started at the epoch,
and every file written during that session got a 1970 timestamp.

The fix is `CONFIG_RTC_DRV_SA1100=y`. Built in, the driver binds during
boot, `hctosys` sets the clock before `rcS` runs, and userspace never sees
1970 at all.

`CONFIG_RTC_DRV_PXA` stays a module on purpose — that is the *extended*
PXA27x RTC, a different device that does not exist on a PXA255.

## Design decisions

**Built in, not a module.** A module would work — `hctosys` runs on
registration, not at a fixed initcall — but it would have to be added to
`tools/kernel-modules.sh` and `modprobe`d from `rcS`, which means the
clock would be wrong for the first few seconds of every boot and wrong
forever if either step were ever missed. There is no reason to accept that
for a driver measured in kilobytes.

**The RTC keeps UTC.** Nothing else on this board reads the chip — there
is no second OS to stay compatible with — and a UTC RTC makes `hwclock -w`
correct regardless of what `/etc/TZ` happens to say at the time. Both
tools take `-l` if you ever need local time in the chip instead.

**`/etc/TZ` is display-only.** It changes how a time is *printed*, never
what is stored. The uClibc binaries we build ourselves — the whole
X11/Matchbox stack, including the panel clock — read `/etc/TZ` on their
own whenever `TZ` is unset, so nothing has to export it for them. The
stage-2 busybox baked into NAND is an older build that only honours the
environment variable, so `/etc/profile` and `/etc/zshrc` export it from
that same file for shell tools like `date`.

`chunked-deploy.sh` **seeds** `/etc/TZ` rather than overwriting it: since
it cannot desynchronise anything, someone who set their zone on the device
should not have it reverted on the next deploy. (Contrast
`power-management.cfg`, which is appliance policy and *is* overwritten
every time.)

**Why our own `hwclock` and `ntpsync`.** The stage-2 rootfs busybox is the
stock stripped build from NAND, not something this repo compiles — it has
no `hwclock`, no `ntpd`, no `rdate`, and for that matter no `kill`, `tar`
or `md5sum`. Editing `modules/initramfs/busybox.config` does nothing here;
that config builds the **stage-1 initramfs** busybox, which only lives long
enough to `kexec` into stage 2. So the clock tools follow the same route as
`userspace/src/kill.c` and `md5sum.c`: small static binaries built by
`tools/build-userspace.sh` and pushed by `tools/chunked-deploy.sh`.

**`--setfields`.** The device's busybox ash has no `printf` and no working
`echo -n`, so `settime` cannot zero-pad `8` into `08` to build a
`"YYYY-MM-DD HH:MM:SS"` string. Passing six unpadded integers straight
through to the binary sidesteps shell string surgery completely — and as a
side effect means the user never types a colon.

## Verifying on the device

After deploying a kernel built with the new config:

```
dmesg | grep rtc
```

should show the driver binding and registering, something like
`sa1100-rtc sa1100-rtc: registered as rtc0`, plus a
`setting system clock to ...` line from `hctosys`. Then:

```
ls /dev/rtc0
settime
```

`settime` prints the system clock and the RTC; on a healthy board they
agree. Round-trip the persistence:

```
settime 2026 8 1 14 30
settime save
reboot
settime
```

The time after the reboot should continue from where it was, not restart
at 1970.

If `/dev/rtc0` is missing, `rcS` says so on the console at boot — that
means the running zImage predates this change, not that the hardware is
faulty.

## Known limits

- **The 32-bit second counter.** The PXA RTC counts seconds in a 32-bit
  register, so it cannot represent anything before 1970 at all; `hwclock`
  rejects such dates rather than storing something arbitrary.
- **No drift correction.** `ntpsync` *steps* the clock — no `adjtime`, no
  drift file, no gradual slew. On a board that used to boot at the epoch,
  stepping is the point. The PXA RTC's accuracy is whatever its 32.768 kHz
  crystal manages; if it matters, run `settime ntp` occasionally.
- **Power removal.** The counter is only carried across a full power-off
  by the board's backup cell. On hardware this old that cell is often
  dead, in which case the time survives reboots, `kexec` and suspend but
  not a battery pull. `ntpsync` on WiFi bring-up covers that case without
  anyone having to think about it.
