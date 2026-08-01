// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell PXA25x family clocks
 *
 * Copyright (C) 2014 Robert Jarzmik
 *
 * Heavily inspired from former arch/arm/mach-pxa/pxa25x.c.
 *
 * For non-devicetree platforms. Once pxa is fully converted to devicetree, this
 * should go away.
 */
#include <linux/clk-provider.h>
#include <linux/clk.h>
#include <linux/clk/pxa.h>
#include <linux/clkdev.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/soc/pxa/smemc.h>
#include <linux/soc/pxa/cpu.h>

#include <dt-bindings/clock/pxa-clock.h>
#include "clk-pxa.h"
#include "clk-pxa2xx.h"

#define KHz 1000
#define MHz (1000 * 1000)

enum {
	PXA_CORE_RUN = 0,
	PXA_CORE_TURBO,
};

#define PXA25x_CLKCFG(T)			\
	(CLKCFG_FCS |				\
	 ((T) ? CLKCFG_TURBO : 0))
#define PXA25x_CCCR(N2, M, L) (N2 << 7 | M << 5 | L)

/* Define the refresh period in mSec for the SDRAM and the number of rows */
#define SDRAM_TREF	64	/* standard 64ms SDRAM */

/*
 * Various clock factors driven by the CCCR register.
 */
static void __iomem *clk_regs;

/* Crystal Frequency to Memory Frequency Multiplier (L) */
static unsigned char L_clk_mult[32] = { 0, 27, 32, 36, 40, 45, 0, };

/* Memory Frequency to Run Mode Frequency Multiplier (M) */
static unsigned char M_clk_mult[4] = { 0, 1, 2, 4 };

/* Run Mode Frequency to Turbo Mode Frequency Multiplier (N) */
/* Note: we store the value N * 2 here. */
static unsigned char N2_clk_mult[8] = { 0, 0, 2, 3, 4, 0, 6, 0 };

static const char * const get_freq_khz[] = {
	"core", "run", "cpll", "memory"
};

static u32 mdrefr_dri(unsigned int freq_khz)
{
	u32 interval = freq_khz * SDRAM_TREF / pxa2xx_smemc_get_sdram_rows();

	return interval / 32;
}

/*
 * Get the clock frequency as reflected by CCCR and the turbo flag.
 * We assume these values have been applied via a fcs.
 * If info is not 0 we also display the current settings.
 */
unsigned int pxa25x_get_clk_frequency_khz(int info)
{
	struct clk *clk;
	unsigned long clks[5];
	int i;

	for (i = 0; i < ARRAY_SIZE(get_freq_khz); i++) {
		clk = clk_get(NULL, get_freq_khz[i]);
		if (IS_ERR(clk)) {
			clks[i] = 0;
		} else {
			clks[i] = clk_get_rate(clk);
			clk_put(clk);
		}
	}

	if (info) {
		pr_info("Run Mode clock: %ld.%02ldMHz\n",
			clks[1] / 1000000, (clks[1] % 1000000) / 10000);
		pr_info("Turbo Mode clock: %ld.%02ldMHz\n",
			clks[2] / 1000000, (clks[2] % 1000000) / 10000);
		pr_info("Memory clock: %ld.%02ldMHz\n",
			clks[3] / 1000000, (clks[3] % 1000000) / 10000);
	}

	return (unsigned int)clks[0] / KHz;
}

static unsigned long clk_pxa25x_memory_get_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	unsigned long cccr = readl(clk_regs + CCCR);
	unsigned int m = M_clk_mult[(cccr >> 5) & 0x03];

	return parent_rate / m;
}
PARENTS(clk_pxa25x_memory) = { "run" };
RATE_RO_OPS(clk_pxa25x_memory, "memory");

PARENTS(pxa25x_pbus95) = { "ppll_95_85mhz", "ppll_95_85mhz" };
PARENTS(pxa25x_pbus147) = { "ppll_147_46mhz", "ppll_147_46mhz" };
PARENTS(pxa25x_osc3) = { "osc_3_6864mhz", "osc_3_6864mhz" };

#define PXA25X_CKEN(dev_id, con_id, parents, mult, div,			\
		    bit, is_lp, flags)					\
	PXA_CKEN(dev_id, con_id, bit, parents, mult, div, mult, div,	\
		 is_lp,  CKEN, CKEN_ ## bit, flags)
#define PXA25X_PBUS95_CKEN(dev_id, con_id, bit, mult_hp, div_hp, delay)	\
	PXA25X_CKEN(dev_id, con_id, pxa25x_pbus95_parents, mult_hp,	\
		    div_hp, bit, NULL, 0)
#define PXA25X_PBUS147_CKEN(dev_id, con_id, bit, mult_hp, div_hp, delay)\
	PXA25X_CKEN(dev_id, con_id, pxa25x_pbus147_parents, mult_hp,	\
		    div_hp, bit, NULL, 0)
#define PXA25X_OSC3_CKEN(dev_id, con_id, bit, mult_hp, div_hp, delay)	\
	PXA25X_CKEN(dev_id, con_id, pxa25x_osc3_parents, mult_hp,	\
		    div_hp, bit, NULL, 0)

#define PXA25X_CKEN_1RATE(dev_id, con_id, bit, parents, delay)		\
	PXA_CKEN_1RATE(dev_id, con_id, bit, parents,			\
		       CKEN, CKEN_ ## bit, 0)
#define PXA25X_CKEN_1RATE_AO(dev_id, con_id, bit, parents, delay)	\
	PXA_CKEN_1RATE(dev_id, con_id, bit, parents,			\
		       CKEN, CKEN_ ## bit, CLK_IGNORE_UNUSED)

static struct desc_clk_cken pxa25x_clocks[] __initdata = {
	PXA25X_PBUS95_CKEN("pxa2xx-mci.0", NULL, MMC, 1, 5, 0),
	PXA25X_PBUS95_CKEN("pxa2xx-i2c.0", NULL, I2C, 1, 3, 0),
	PXA25X_PBUS95_CKEN("pxa2xx-ir", "FICPCLK", FICP, 1, 2, 0),
	PXA25X_PBUS95_CKEN("pxa25x-udc", NULL, USB, 1, 2, 5),
	PXA25X_PBUS147_CKEN("pxa2xx-uart.0", NULL, FFUART, 1, 10, 1),
	PXA25X_PBUS147_CKEN("pxa2xx-uart.1", NULL, BTUART, 1, 10, 1),
	PXA25X_PBUS147_CKEN("pxa2xx-uart.2", NULL, STUART, 1, 10, 1),
	PXA25X_PBUS147_CKEN("pxa2xx-uart.3", NULL, HWUART, 1, 10, 1),
	PXA25X_PBUS147_CKEN("pxa2xx-i2s", NULL, I2S, 1, 10, 0),
	PXA25X_PBUS147_CKEN(NULL, "AC97CLK", AC97, 1, 12, 0),
	PXA25X_OSC3_CKEN("pxa25x-ssp.0", NULL, SSP, 1, 1, 0),
	PXA25X_OSC3_CKEN("pxa25x-nssp.1", NULL, NSSP, 1, 1, 0),
	PXA25X_OSC3_CKEN("pxa25x-nssp.2", NULL, ASSP, 1, 1, 0),
	PXA25X_OSC3_CKEN("pxa25x-pwm.0", NULL, PWM0, 1, 1, 0),
	PXA25X_OSC3_CKEN("pxa25x-pwm.1", NULL, PWM1, 1, 1, 0),

	PXA25X_CKEN_1RATE("pxa2xx-fb", NULL, LCD, clk_pxa25x_memory_parents, 0),
	PXA25X_CKEN_1RATE_AO("pxa2xx-pcmcia", NULL, MEMC,
			     clk_pxa25x_memory_parents, 0),
};

/*
 * In this table, PXA25x_CCCR(N2, M, L) has the following meaning, where :
 *   - freq_cpll = n * m * L * 3.6864 MHz
 *   - n = N2 / 2
 *   - m = 2^(M - 1), where 1 <= M <= 3
 *   - l = L_clk_mult[L], ie. { 0, 27, 32, 36, 40, 45, 0, }[L]
 */
static struct pxa2xx_freq pxa25x_freqs[] = {
	/* CPU  MEMBUS  CCCR                  DIV2 CCLKCFG      */
	{ 99532800, 99500, PXA25x_CCCR(2,  1, 1),  1, PXA25x_CLKCFG(1)},
	{199065600, 99500, PXA25x_CCCR(4,  1, 1),  0, PXA25x_CLKCFG(1)},
	{298598400, 99500, PXA25x_CCCR(3,  2, 1),  0, PXA25x_CLKCFG(1)},
	{398131200, 99500, PXA25x_CCCR(4,  2, 1),  0, PXA25x_CLKCFG(1)},

	/*
	 * OVERCLOCK STEPS -- above the PXA255's rated 400 MHz. Present in this
	 * table but NOT reachable by default: nothing selects a rate that is
	 * not in the cpufreq driver's frequency table, and that driver builds
	 * its pxa25x table only up to its `maxfreq` module parameter, which
	 * defaults to the stock 398 MHz. See docs/HOWTO-OVERCLOCK.md.
	 *
	 * There is no N (turbo ratio) step between 2 and 3 on this part, so
	 * every intermediate step above 398 MHz has to come from raising L,
	 * the crystal-to-memory multiplier -- which means the memory clock
	 * rises in lockstep with the core:
	 *
	 *   L idx  l   core (n=2, m=2)  membus = l * 3.6864
	 *     2    32     471.86 MHz        117.96 MHz
	 *     3    36     530.84 MHz        132.71 MHz
	 *     4    40     589.82 MHz        147.46 MHz
	 *     5    45     663.55 MHz        165.89 MHz
	 *
	 * DIV2 stays 0, so SDCLK tracks the memory clock and the SDRAM is
	 * overclocked by the same ratio as the core -- exactly what the
	 * period Zaurus/PXA255 overclocking tools did. 471 MHz (SDRAM at
	 * 118 MHz) is the step those tools shipped as their first and safest
	 * notch; the three above it run SDRAM 33-67% over its 100 MHz rating
	 * and are not expected to be stable on most boards.
	 *
	 * Two consumers of the memory clock are worth naming, because they
	 * behave differently:
	 *
	 *  - PCMCIA (and so WiFi -- the only remote access this board has)
	 *    re-derives its socket timings on every frequency change, via
	 *    pxa2xx_pcmcia_frequency_change() in drivers/pcmcia/pxa2xx_base.c.
	 *    That path is compiled in here (CONFIG_CPU_FREQ) and needs nothing
	 *    from us.
	 *  - The W100 framebuffer on nCS_2 does NOT. Its static-memory timing
	 *    (MSC1.CS2) is left at the VLIO value the bootloader programmed
	 *    (see corgi_init() in modules/mach-pxa/corgi_patched.c), and is
	 *    expressed in bus cycles, so raising the memory clock shortens
	 *    every W100 access in real time. The display is therefore the
	 *    most likely thing to misbehave first when stepping up.
	 */
	{471859200, 117964, PXA25x_CCCR(4,  2, 2),  0, PXA25x_CLKCFG(1)},
	{530841600, 132710, PXA25x_CCCR(4,  2, 3),  0, PXA25x_CLKCFG(1)},
	{589824000, 147456, PXA25x_CCCR(4,  2, 4),  0, PXA25x_CLKCFG(1)},
	{663552000, 165888, PXA25x_CCCR(4,  2, 5),  0, PXA25x_CLKCFG(1)},
};

static u8 clk_pxa25x_core_get_parent(struct clk_hw *hw)
{
	unsigned long clkcfg;
	unsigned int t;

	asm("mrc\tp14, 0, %0, c6, c0, 0" : "=r" (clkcfg));
	t  = clkcfg & (1 << 0);
	if (t)
		return PXA_CORE_TURBO;
	return PXA_CORE_RUN;
}

static int clk_pxa25x_core_set_parent(struct clk_hw *hw, u8 index)
{
	if (index > PXA_CORE_TURBO)
		return -EINVAL;

	pxa2xx_core_turbo_switch(index == PXA_CORE_TURBO);

	return 0;
}

static int clk_pxa25x_core_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	return __clk_mux_determine_rate(hw, req);
}

PARENTS(clk_pxa25x_core) = { "run", "cpll" };
MUX_OPS(clk_pxa25x_core, "core", CLK_SET_RATE_PARENT);

static unsigned long clk_pxa25x_run_get_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	unsigned long cccr = readl(clk_regs + CCCR);
	unsigned int n2 = N2_clk_mult[(cccr >> 7) & 0x07];

	return (parent_rate / n2) * 2;
}
PARENTS(clk_pxa25x_run) = { "cpll" };
RATE_RO_OPS(clk_pxa25x_run, "run");

static unsigned long clk_pxa25x_cpll_get_rate(struct clk_hw *hw,
	unsigned long parent_rate)
{
	unsigned long clkcfg, cccr = readl(clk_regs + CCCR);
	unsigned int l, m, n2, t;

	asm("mrc\tp14, 0, %0, c6, c0, 0" : "=r" (clkcfg));
	t = clkcfg & (1 << 0);
	l  =  L_clk_mult[(cccr >> 0) & 0x1f];
	m = M_clk_mult[(cccr >> 5) & 0x03];
	n2 = N2_clk_mult[(cccr >> 7) & 0x07];

	return m * l * n2 * parent_rate / 2;
}

static int clk_pxa25x_cpll_determine_rate(struct clk_hw *hw,
					  struct clk_rate_request *req)
{
	return pxa2xx_determine_rate(req, pxa25x_freqs,
				     ARRAY_SIZE(pxa25x_freqs));
}

static int clk_pxa25x_cpll_set_rate(struct clk_hw *hw, unsigned long rate,
				    unsigned long parent_rate)
{
	int i;

	pr_debug("%s(rate=%lu parent_rate=%lu)\n", __func__, rate, parent_rate);
	for (i = 0; i < ARRAY_SIZE(pxa25x_freqs); i++)
		if (pxa25x_freqs[i].cpll == rate)
			break;

	if (i >= ARRAY_SIZE(pxa25x_freqs))
		return -EINVAL;

	pxa2xx_cpll_change(&pxa25x_freqs[i], mdrefr_dri, clk_regs + CCCR);

	return 0;
}
PARENTS(clk_pxa25x_cpll) = { "osc_3_6864mhz" };
RATE_OPS(clk_pxa25x_cpll, "cpll");

static void __init pxa25x_register_core(void)
{
	clkdev_pxa_register(CLK_NONE, "cpll", NULL,
			    clk_register_clk_pxa25x_cpll());
	clkdev_pxa_register(CLK_NONE, "run", NULL,
			    clk_register_clk_pxa25x_run());
	clkdev_pxa_register(CLK_CORE, "core", NULL,
			    clk_register_clk_pxa25x_core());
}

static void __init pxa25x_register_plls(void)
{
	clk_register_fixed_rate(NULL, "osc_3_6864mhz", NULL,
				CLK_GET_RATE_NOCACHE, 3686400);
	clkdev_pxa_register(CLK_OSC32k768, "osc_32_768khz", NULL,
			    clk_register_fixed_rate(NULL, "osc_32_768khz", NULL,
						    CLK_GET_RATE_NOCACHE,
						    32768));
	clk_register_fixed_rate(NULL, "clk_dummy", NULL, 0, 0);
	clk_register_fixed_factor(NULL, "ppll_95_85mhz", "osc_3_6864mhz",
				  0, 26, 1);
	clk_register_fixed_factor(NULL, "ppll_147_46mhz", "osc_3_6864mhz",
				  0, 40, 1);
}

static void __init pxa25x_base_clocks_init(void)
{
	pxa25x_register_plls();
	pxa25x_register_core();
	clkdev_pxa_register(CLK_NONE, "system_bus", NULL,
			    clk_register_clk_pxa25x_memory());
}

#define DUMMY_CLK(_con_id, _dev_id, _parent) \
	{ .con_id = _con_id, .dev_id = _dev_id, .parent = _parent }
struct dummy_clk {
	const char *con_id;
	const char *dev_id;
	const char *parent;
};
static struct dummy_clk dummy_clks[] __initdata = {
	DUMMY_CLK(NULL, "pxa25x-gpio", "osc_32_768khz"),
	DUMMY_CLK(NULL, "pxa26x-gpio", "osc_32_768khz"),
	DUMMY_CLK("GPIO11_CLK", NULL, "osc_3_6864mhz"),
	DUMMY_CLK("GPIO12_CLK", NULL, "osc_32_768khz"),
	DUMMY_CLK(NULL, "sa1100-rtc", "osc_32_768khz"),
	DUMMY_CLK("OSTIMER0", NULL, "osc_3_6864mhz"),
	DUMMY_CLK("UARTCLK", "pxa2xx-ir", "STUART"),
};

static void __init pxa25x_dummy_clocks_init(void)
{
	struct clk *clk;
	struct dummy_clk *d;
	const char *name;
	int i;

	/*
	 * All pinctrl logic has been wiped out of the clock driver, especially
	 * for gpio11 and gpio12 outputs. Machine code should ensure proper pin
	 * control (ie. pxa2xx_mfp_config() invocation).
	 */
	for (i = 0; i < ARRAY_SIZE(dummy_clks); i++) {
		d = &dummy_clks[i];
		name = d->dev_id ? d->dev_id : d->con_id;
		clk = clk_register_fixed_factor(NULL, name, d->parent, 0, 1, 1);
		clk_register_clkdev(clk, d->con_id, d->dev_id);
	}
}

int __init pxa25x_clocks_init(void __iomem *regs)
{
	int ret;

	clk_regs = regs;

	pxa25x_base_clocks_init();

	pxa25x_dummy_clocks_init();

	ret = clk_pxa_cken_init(pxa25x_clocks, ARRAY_SIZE(pxa25x_clocks), clk_regs);

	return ret;
}

static void __init pxa25x_dt_clocks_init(struct device_node *np)
{
	pxa25x_clocks_init(ioremap(0x41300000ul, 0x10));
	clk_pxa_dt_common_init(np);
}
CLK_OF_DECLARE(pxa25x_clks, "marvell,pxa250-core-clocks",
	       pxa25x_dt_clocks_init);
