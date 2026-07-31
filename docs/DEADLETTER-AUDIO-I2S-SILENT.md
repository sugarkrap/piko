# Dead Letter — audio: card registers, `aplay` "plays", but no sound and DMA never runs

*Written 2026-07-30. Two independent mainline bugs stacked on top of each
other; both had to be fixed before a single sample reached the WM8731.
Both fixes are tracked under `modules/` and wired into
`tools/setup-kernel-src.sh` — see "Where the fixes live" at the bottom,
because an earlier attempt left one of them only in the gitignored
`kernel-src/` tree, where the next `--force-kernel-src` would have silently
erased it.*

---

## Symptom

Three distinct failure faces, in the order they were hit. They look like
three different problems and are really only two:

**1. PCM open fails outright**
```
aplay: main:850: audio open error: No such device or address    (-ENXIO)
```
`dmesg`:
```
pxa2xx-i2s pxa2xx-i2s: ASoC error (-6): at snd_soc_component_open() on pxa2xx-i2s
 WM8731: ASoC error (-6): at __soc_pcm_open() on WM8731
```

**2. After fixing (1): `aplay` hangs forever, or dies with `-EIO`**
```
aplay: pcm_write:2178: write error: Input/output error
```
The card is registered the whole time (`/proc/asound/cards` shows `Corgi`),
`/proc/asound/card0/pcm0p/sub0/status` reads `state: RUNNING` — and yet:
```
hw_ptr      : 0            <-- never advances
appl_ptr    : 22440        <-- userspace filled the whole buffer
```
`grep pxa-dma /proc/interrupts` stays **frozen** across the entire playback
attempt. Not slow — *zero* transfers.

**3. Even once DMA runs: silence** — see "Third thing: the mute GPIO" below.

## Cause 1 — no DMA slave-map entry for `pxa2xx-i2s` on PXA25x

`pxa2xx_soc_pcm_open()` → `pxa2xx_pcm_open()` ends with:
```c
return snd_dmaengine_pcm_open(
    substream, dma_request_slave_channel(snd_soc_rtd_to_cpu(rtd, 0)->dev,
                                         dma_params->chan_name));
```
`dma_request_slave_channel()` resolves `(dev_name, "tx"/"rx")` through the
DMA controller's **slave map**, supplied as platform data. `pxa27x.c` has:
```c
{ "pxa2xx-i2s", "rx", PDMA_FILTER_PARAM(LOWEST, 2) },
{ "pxa2xx-i2s", "tx", PDMA_FILTER_PARAM(LOWEST, 3) },
```
**`pxa25x.c` has no I2S entries at all** — only AC97, SSP, IR and MMC. So on
Corgi the lookup returns `NULL`, `snd_dmaengine_pcm_open()` fails, and ASoC
reports `-ENXIO`. The I2S DMA request-line block is unchanged between PXA25x
and PXA27x (unlike SSP, which genuinely differs and has its own PXA25x
entries), so the pxa27x values port over directly.

This is an upstream gap, not a local regression: mainline PXA25x I2S boards
appear never to have been exercised on the generic-dmaengine path.

## Cause 2 — `set_dai_fmt` clock-provider cases never match (the silent one)

This is the interesting one, and it is **invisible without reading
registers**. `include/sound/soc-dai.h` in this kernel:
```c
#define SND_SOC_DAIFMT_CBP_CFC  (3 << 12)  /* codec clk provider & frame consumer */
#define SND_SOC_DAIFMT_CBC_CFC  (4 << 12)  /* codec clk consumer & frame consumer */

#define SND_SOC_DAIFMT_BP_FP    SND_SOC_DAIFMT_CBP_CFP
#define SND_SOC_DAIFMT_BC_FP    SND_SOC_DAIFMT_CBC_CFP
#define SND_SOC_DAIFMT_BP_FC    SND_SOC_DAIFMT_CBP_CFC
#define SND_SOC_DAIFMT_BC_FC    SND_SOC_DAIFMT_CBC_CFC
```
**The `BP_FP`/`BC_FP` aliases are CODEC-centric.** `BP_FP` does *not* mean
"CPU is bitclock provider" — it is a plain alias for `CBP_CFP`, "**codec**
clk provider". The names read backwards from what they mean at a CPU-DAI
call site.

`pxa2xx_i2s_set_dai_fmt()` switched on exactly those misleading aliases:
```c
switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
case SND_SOC_DAIFMT_BP_FP:   /* == CBP_CFP (1<<12) */
    pxa_i2s.master = 1;
    break;
case SND_SOC_DAIFMT_BC_FP:   /* == CBC_CFP (2<<12) */
    pxa_i2s.master = 0;
    break;
default:
    break;                   /* <-- corgi lands HERE */
}
```
`corgi.c` asks for `SND_SOC_DAIFMT_CBC_CFC` (`4 << 12`) — codec consumes
both clocks, i.e. the PXA must drive them. That matches **neither** case, so
it falls through `default:` and `pxa_i2s.master` keeps its **static initial
value of 0**. `pxa2xx_i2s_hw_params()` then does:
```c
if (pxa_i2s.master)
    writel(readl(...SACR0) | SACR0_BCKD, ...);   /* never executed */
```
so `SACR0_BCKD` (Bit Clock Direction) is never set and the PXA stays a
bit-clock *slave*, waiting for a clock the WM8731 was configured never to
send.

The pre-rename driver mapped `CBS_CFS` (codec bit slave + frame slave =
CPU drives both) → `master = 1`. `CBS_CFS` is spelled `CBC_CFC` today, so
the correct cases are `CBC_CFC → 1` and `CBP_CFC → 0`. The mechanical
CBS→CBC rename picked the wrong aliases and inverted the meaning.

### Why this is so quiet

Nothing errors. The link *enables* (`SACR0_ENB=1`), playback *is* enabled
(`SACR1_DRPL=0`), the DMA request line *is* mapped, the DMA channel *is*
armed and running. There is simply no serial clock, so the Tx FIFO never
drains, `SASR0_TFS` (FIFO service request) never asserts, the I2S DMA
request never fires, and the armed channel waits forever. ALSA faithfully
reports `RUNNING`.

## How it was actually found

Source reading alone was not enough — both drivers *look* correct. The
decisive step was dumping the hardware registers through `/dev/mem` with a
small static ARM binary (the device has no `devmem`, no `debugfs`, no
`/sys/kernel/debug/clk`).

Broken (`master` never set):
```
SACR0  = 0x0000e101   ENB=1 BCKD=0  <-- CPU is NOT driving the bit clock
SASR0  = 0x00000005   TFS=0 BSY=1
DRCMR3 = 0x0000008d   (i2s tx) MAPVLD=1 chan=13   <-- request line IS mapped
DCSR13 = 0xa0000000   RUN=1                       <-- DMA IS armed
SASR0 sampled 5x: 0x00000005 0x00000005 0x00000005 0x00000005 0x00000005
                  ^ frozen: FIFO level never changes, nothing is moving
```

Fixed:
```
SACR0  = 0x0000e105   ENB=1 BCKD=1  <-- now driving BITCLK/LRCLK
SASR0 sampled 5x: 0x00000605 0x00000905 0x00000705 0x00000905 0x00000205
                  ^ FIFO level varying = samples actually flowing
```
and `grep pxa-dma /proc/interrupts` went from a frozen `3` to `47` after one
4-second clip, `177` after the next. `aplay` returned `EXIT:0` instead of
hanging.

**The `SASR0`-sampled-5×-and-compare trick is the fastest way to tell
"clocked and draining" from "enabled but dead".** A constant value means no
clock, no matter what every other status bit claims.

## Third thing: there is NO required mixer step (correcting an earlier claim)

An earlier revision of this document asserted that output stayed silent
until userspace ran `amixer cset numid=11 'Headphone'`, and called that step
mandatory. **That was wrong, in the most misleading possible direction, and
it is worth understanding why so the mistake is not repeated.**

The driver's power-on defaults are already the correct speaker
configuration, and are verified audible on hardware with no `amixer` call at
all:

| control | default | meaning |
|---|---|---|
| `Jack Function` | `Off` (4) | **"no jack plugged in"** — mutes the HEADPHONE outputs, leaves the speaker path alone |
| `Speaker Function` | `On` (0) | enables the `Ext Spk` widget, whose power event drives the `APM_ON` speaker-amplifier GPIO |
| `Master Playback Volume` | 121/127 (~0 dB) | plenty |

`Off` does **not** mean "muted". `corgi_ext_control()`'s `mute_l`/`mute_r`
GPIOs gate the **headphone** outputs only (and note the polarity reads
backwards: `1` = un-muted — `corgi_shutdown()`'s "Unmute HP outputs" comment
sets them to `1`). Forcing `Jack Function=Headphone` therefore routes audio
to an empty headphone jack, which is the opposite of what a speaker test
wants.

Verified by reading the SCOOP GPIOs directly during playback in the default
configuration:
```
MUTE_L bit4  dir=OUT out=0    <- headphone muted (correct for speaker)
MUTE_R bit5  dir=OUT out=0
APM_ON bit7  dir=OUT out=1    <- SPEAKER AMPLIFIER ENABLED
APM_ON sampled 10x: 1(0080) 1(0080) 1(0080) ...
```

> **SCOOP GPIO off-by-one trap.** These are SCOOP GPIOs, not PXA GPIOs, and
> `arch/arm/common/scoop.c` maps gpio offset *N* to register bit ***N+1***
> (`gpwr |= 1 << (offset + 1)`). So `MUTE_L`(3)→bit 4, `MUTE_R`(4)→bit 5,
> `APM_ON`(6)→**bit 7**. Reading the un-shifted bits shows `APM_ON = 0`
> during playback and "proves" the amplifier is dead when it is in fact on.
> This cost a full misdiagnosis before the shift was spotted.

### How the wrong conclusion got reached

The original silence was real — with no DMA at all (causes 1 and 2 above),
nothing could ever be audible. After fixing those, the remaining silence was
attributed to the mute GPIO and "fixed" by forcing `Headphone`, **without
re-testing the untouched defaults**. Because the two kernel fixes had
already made it work, the unnecessary workaround appeared to be part of the
solution and was written up as mandatory.

The lesson is the same one this repo keeps re-learning: after fixing a root
cause, **re-test the original configuration before keeping any workaround
added along the way.** A workaround that is merely harmless still becomes
false documentation.

## Where the fixes live (tracked — read this before trusting a rebuild)

`kernel-src/` is gitignored and is **reconstructed from tracked sources** by
`tools/setup-kernel-src.sh` on every `build-and-deploy.sh` run. Editing it
directly works until the next `--force-kernel-src`, then vanishes silently.
Both fixes are therefore tracked files plus `copy_in` lines:

| Fix | Tracked source | Copied to |
|---|---|---|
| I2S DMA slave map | `modules/mach-pxa/pxa25x_patched.c` | `arch/arm/mach-pxa/pxa25x.c` |
| `set_dai_fmt` clock provider | `modules/sound-pxa/pxa2xx-i2s_patched.c` | `sound/soc/pxa/pxa2xx-i2s.c` |

Verified by running `build-and-deploy.sh --force-kernel-src` (which wipes
and rebuilds the tree purely from tracked sources) and confirming
`snd-soc-pxa2xx-i2s.ko` came out with a new md5 while every other module was
byte-identical.

Note the split: `pxa25x.c` is **built-in** (lands in `zImage-full`), while
`pxa2xx-i2s.c` builds as **`snd-soc-pxa2xx-i2s.ko`**. A module-only change
still needs the modules reloaded — `softreboot` (self-kexec) is the cheap
way, no NAND flash and no power cycle.

## Also worth knowing

- `pxa2xx_i2s_hw_params()` contains a real mainline bug at the
  "is port used by another stream" check:
  ```c
  if (!(SACR0 & SACR0_ENB)) {
  ```
  `SACR0` there is the **offset constant `0x0000`**, not a register read
  (everywhere else the file uses `readl(i2s_reg_base + SACR0)`). So the
  expression is `0 & 1 == 0` and the branch is always taken. Benign in
  practice — the block re-initialises SACR0 from scratch, which is what a
  fresh stream wants — but it is not doing what it says, and it would
  misbehave if playback and capture were ever opened concurrently. Left
  alone deliberately: fixing it changes concurrent-stream behaviour we
  cannot currently test.
- The stuck `aplay` from a failed attempt holds the PCM open and the next
  run fails with `Device or resource busy`. This busybox has **no `kill`
  applet**, so `softreboot` is the practical way to clear it.
