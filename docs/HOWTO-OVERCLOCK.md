# HOWTO — CPU speed and overclocking (`mhz`)

A remake of the speed-step tool the Cacko ROM shipped, built on mainline
cpufreq rather than a Sharp-era kernel patch.

> Overclocking runs this board's SDRAM and its static memory bus outside
> their ratings. It cannot brick the NAND on its own — but a lockup during
> a JFFS2 write can corrupt the `home` partition, and this is the last
> spare board. Read the ladder below before picking a step.

## The tool

`mhz`, deployed to `/usr/sbin/mhz`. Single word, no `/` or `:` — it is
meant to be typed on the device keyboard (see AGENTS.md).

```
mhz              show the current speed, the governor and the steps
mhz auto         scale on demand over the permitted range
mhz 398          pin the core to a fixed step
mhz 471          first overclock notch
mhz 530 force    steps above 471 need the extra word
mhz stock        back to the rated 398 MHz ceiling, on demand
```

**Nothing persists.** The ceiling lives in a module parameter, so a reboot
always comes back at the rated 398 MHz. That is deliberate: the way out of
an overclock that will not boot is a power cycle, not a reflash.

## Why these steps and no others

The PXA255 core clock is

```
core = n * m * l * 3.6864 MHz
```

with `n` (turbo ratio) ∈ {1, 1.5, 2, 3}, `m` (memory→run) ∈ {1, 2, 4} and
`l` (crystal→memory) ∈ {27, 32, 36, 40, 45}. The memory clock is `l *
3.6864 MHz` regardless of `n` and `m`. Stock is `n=2, m=2, l=27` = 398.13
MHz with memory at 99.5 MHz.

`n` has **no step between 2 and 3**, so there is no way to reach 450 or 500
MHz by pushing the turbo ratio alone — the next notch on that axis is 597
MHz in one jump. Every usable intermediate step therefore comes from
raising `l`, which drags the memory clock up with the core in lockstep:

| Step | `l` | Core | Memory / SDRAM | Status |
|---|---|---|---|---|
| | 27 | 99.53 MHz | 99.5 MHz | rated |
| | 27 | 199.07 MHz | 99.5 MHz | rated |
| | 27 | 298.60 MHz | 99.5 MHz | rated |
| **398** | 27 | 398.13 MHz | 99.5 MHz | **rated ceiling, default** |
| **471** | 32 | 471.86 MHz | 118.0 MHz | overclock — the usual notch |
| **530** | 36 | 530.84 MHz | 132.7 MHz | needs `force` |
| **589** | 40 | 589.82 MHz | 147.5 MHz | needs `force` |
| **663** | 45 | 663.55 MHz | 165.9 MHz | needs `force` |

This is the same ladder the period PXA255 overclocking tools exposed, and
for the same reason — it is the only one the part offers. 471 MHz is the
step those tools shipped as their first notch and the one most boards
survived; 530 and above run the SDRAM 33–67% over its 100 MHz rating and
are a lottery per board.

`mhz` refuses anything above 471 without the extra word `force`.

## What else moves when the memory clock does

Two consumers matter, and they behave differently:

- **PCMCIA — and therefore WiFi, the only remote access this board has —
  re-times itself.** `pxa2xx_pcmcia_frequency_change()` in
  `drivers/pcmcia/pxa2xx_base.c` recomputes the socket's memory-controller
  timings on every frequency transition, pre-change when speeding up and
  post-change when slowing down. That path is compiled in here
  (`CONFIG_CPU_FREQ`) and needs nothing from us.
- **The W100 framebuffer does not.** Its static-memory timing (`MSC1.CS2`)
  is left at the VLIO value the bootloader programmed — see `corgi_init()`
  in `modules/mach-pxa/corgi_patched.c` — and is expressed in *bus cycles*,
  so raising the memory clock shortens every W100 access in real time. **The
  display is the most likely thing to misbehave first when stepping up.**

SDRAM refresh is handled: `mdrefr_dri()` in
`modules/clk-pxa/clk_pxa25x_patched.c` derives the MDREFR refresh interval
from the new memory clock, and `pxa2xx_cpll_change()` presets it before a
speed-up and postsets it after a slow-down.

## Where the change lives

| File | What it does |
|---|---|
| `modules/clk-pxa/clk_pxa25x_patched.c` | Four overclock entries in `pxa25x_freqs[]` — the actual CCCR combinations. |
| `modules/cpufreq/pxa2xx_cpufreq_patched.c` | The `pxa255_maxfreq` module parameter, and an honest frequency table (see below). |
| `userspace/src/mhz.c` | The tool. |

Overclock steps are **not** in the policy's frequency table unless
`pxa255_maxfreq` (MHz) is raised past 398. This matters more than it looks:
this kernel's default governor is `ondemand`, which would otherwise ramp
straight to the top of the table on the first busy tick. `mhz` reloads the
module at the ceiling you ask for, which is why changing steps briefly
unloads `pxa2xx-cpufreq`.

## Two stock bugs fixed on the way

Both were found while making the step list honest, and both are worth
knowing about independently of overclocking.

### The stock 398 MHz step actually ran at 298 MHz

`pxa2xx_determine_rate()` prefers the closest table entry **at or below**
the requested rate, and only takes the one above when there is nothing
below. Stock asked for `398100` kHz against a real cpll of `398131.2` kHz —
just *under* it, with `298598.4` kHz sitting underneath — so selecting the
maximum stock frequency programmed 298 MHz while `/sys` reported 398.

Every entry is now `ceil(cpll / 1000)`, i.e. a hair *above* the real rate,
so each step lands on itself. **The rounding direction is load-bearing:**
any entry added here later must be rounded up the same way.

A side effect worth expecting: `scaling_cur_freq` reads back `398131`
(what `clk_get_rate()` reports) against a table entry of `398132`. The
1 kHz difference is cosmetic, and `mhz` matches on whole MHz because of it.

### Three listed steps never existed

The stock `pxa255_run_freqs[]` table listed 132.7, 265.4 and 331.8 MHz.
None of those were ever reachable on a kernel using the pxa25x common-clk
driver: `clk-pxa25x.c` only programs CCCR combinations in its own
`pxa25x_freqs[]` table (99.53 / 199.07 / 298.60 / 398.13 MHz), and
`pxa2xx_determine_rate()` silently rounds anything else to the nearest
entry. Asking for 265 MHz quietly gave you 199 or 298 while `/sys` reported
265.

Listing frequencies the hardware is never actually set to makes any speed
UI a liar, so the table now lists only what the clock driver can really
program — the four real steps, plus the overclocks.

## Checking it worked

```
mhz                          # core / governor / ceiling / steps
cat /proc/cpuinfo            # BogoMIPS moves with the core clock
dmesg | grep -i pxa255       # the driver logs its table and any overclock
```

`mhz` reports the frequency it reads back from `scaling_cur_freq` after
setting, not the one it asked for — so a step that silently did not take
shows up as a different number rather than a success.
