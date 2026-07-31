// SPDX-License-Identifier: GPL-2.0-only
/*
 * Marvell PXA family clocks
 *
 * Copyright (C) 2014 Robert Jarzmik
 *
 * Common clock code for PXA clocks ("CKEN" type clocks + DT)
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/string.h>
#include <linux/clkdev.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/soc/pxa/smemc.h>

#include <dt-bindings/clock/pxa-clock.h>
#include "clk-pxa.h"

#define KHz 1000
#define MHz (1000 * 1000)

#define MDREFR_K0DB4	(1 << 29)	/* SDCLK0 Divide by 4 Control/Status */
#define MDREFR_K2FREE	(1 << 25)	/* SDRAM Free-Running Control */
#define MDREFR_K1FREE	(1 << 24)	/* SDRAM Free-Running Control */
#define MDREFR_K0FREE	(1 << 23)	/* SDRAM Free-Running Control */
#define MDREFR_SLFRSH	(1 << 22)	/* SDRAM Self-Refresh Control/Status */
#define MDREFR_APD	(1 << 20)	/* SDRAM/SSRAM Auto-Power-Down Enable */
#define MDREFR_K2DB2	(1 << 19)	/* SDCLK2 Divide by 2 Control/Status */
#define MDREFR_K2RUN	(1 << 18)	/* SDCLK2 Run Control/Status */
#define MDREFR_K1DB2	(1 << 17)	/* SDCLK1 Divide by 2 Control/Status */
#define MDREFR_K1RUN	(1 << 16)	/* SDCLK1 Run Control/Status */
#define MDREFR_E1PIN	(1 << 15)	/* SDCKE1 Level Control/Status */
#define MDREFR_K0DB2	(1 << 14)	/* SDCLK0 Divide by 2 Control/Status */
#define MDREFR_K0RUN	(1 << 13)	/* SDCLK0 Run Control/Status */
#define MDREFR_E0PIN	(1 << 12)	/* SDCKE0 Level Control/Status */
#define MDREFR_DB2_MASK	(MDREFR_K2DB2 | MDREFR_K1DB2)
#define MDREFR_DRI_MASK	0xFFF

static DEFINE_SPINLOCK(pxa_clk_lock);

static struct clk *pxa_clocks[CLK_MAX];
static struct clk_onecell_data onecell_data = {
	.clks = pxa_clocks,
	.clk_num = CLK_MAX,
};

struct pxa_clk {
	struct clk_hw hw;
	struct clk_fixed_factor lp;
	struct clk_fixed_factor hp;
	struct clk_gate gate;
	bool (*is_in_low_power)(void);
};

#define to_pxa_clk(_hw) container_of(_hw, struct pxa_clk, hw)

static unsigned long cken_recalc_rate(struct clk_hw *hw,
				      unsigned long parent_rate)
{
	struct pxa_clk *pclk = to_pxa_clk(hw);
	struct clk_fixed_factor *fix;

	if (!pclk->is_in_low_power || pclk->is_in_low_power())
		fix = &pclk->lp;
	else
		fix = &pclk->hp;
	__clk_hw_set_clk(&fix->hw, hw);
	return clk_fixed_factor_ops.recalc_rate(&fix->hw, parent_rate);
}

static const struct clk_ops cken_rate_ops = {
	.recalc_rate = cken_recalc_rate,
};

static u8 cken_get_parent(struct clk_hw *hw)
{
	struct pxa_clk *pclk = to_pxa_clk(hw);

	if (!pclk->is_in_low_power)
		return 0;
	return pclk->is_in_low_power() ? 0 : 1;
}

static const struct clk_ops cken_mux_ops = {
	.determine_rate = clk_hw_determine_rate_no_reparent,
	.get_parent = cken_get_parent,
	.set_parent = dummy_clk_set_parent,
};

void __init clkdev_pxa_register(int ckid, const char *con_id,
				const char *dev_id, struct clk *clk)
{
	if (!IS_ERR(clk) && (ckid != CLK_NONE))
		pxa_clocks[ckid] = clk;
	if (!IS_ERR(clk))
		clk_register_clkdev(clk, con_id, dev_id);
}

int __init clk_pxa_cken_init(const struct desc_clk_cken *clks,
			     int nb_clks, void __iomem *clk_regs)
{
	int i;
	struct pxa_clk *pxa_clk;
	struct clk *clk;

	for (i = 0; i < nb_clks; i++) {
		/*
		 * WORKAROUND (2026-07-29, narrowed 2026-07-30): skip the LCD
		 * and static-memory-controller clocks -- but only where
		 * nothing can consume them.
		 *
		 * Registering one of these two hangs this board solid inside
		 * this loop. Which of the two is at fault was never actually
		 * pinned down: they are the last two entries, share a macro
		 * and parent list, and the only thing distinguishing them at
		 * the time was an LED-blinked loop index (15 vs 16) that was
		 * read inconsistently across attempts. Skipping BOTH is what
		 * originally got this board booting at all.
		 *
		 * That over-broad skip then silently cost WiFi (found
		 * 2026-07-30). The MEMC clock's dev_id is literally
		 * "pxa2xx-pcmcia", and pxa2xx_drv_pcmcia_probe() does a
		 * devm_clk_get() and converts ANY failure into -ENODEV --
		 * which really_probe() logs at pr_debug level only. So the
		 * PCMCIA socket driver silently never bound, no socket ever
		 * appeared, hostap_cs was never auto-loaded, there was no
		 * wlan0, and dmesg said nothing at all about why.
		 *
		 * So keep skipping MEMC only in kernels built WITHOUT PCMCIA
		 * support -- i.e. the bootstrap, which only mounts JFFS2 and
		 * kexecs stage 2, has no possible consumer for this clock, is
		 * the configuration the hang was actually observed in, and is
		 * where a hang costs the most (a dead device needing an mtd1
		 * reflash). Register it wherever PCMCIA does exist (stage 2),
		 * which genuinely needs it: pxa2xx_configure_sockets() derives
		 * real memory-controller timings from clk_get_rate() on this
		 * clock, so substituting a dummy fixed-rate clock would
		 * mis-time card accesses rather than fix anything.
		 *
		 * LCD ("pxa2xx-fb") stays skipped in both: this board's display
		 * is the external W100 on nCS_2 driven by w100fb, the PXA LCD
		 * controller is unused, and no pxa2xx-fb device is ever
		 * registered -- so that clock has no consumer either way.
		 *
		 * Skipping is still safe in itself: NOT registering a clock
		 * does not disable it; each CKEN bit keeps whatever value the
		 * bootloader left. This remains a containment measure, NOT a
		 * root-cause fix -- why registration hangs is still open, and
		 * is now finally testable with a real framebuffer console on
		 * both kernels instead of counted LED blinks.
		 */
		if (clks[i].dev_id && !strcmp(clks[i].dev_id, "pxa2xx-fb"))
			continue;
		if (!IS_ENABLED(CONFIG_PCMCIA_PXA2XX) && clks[i].dev_id &&
		    !strcmp(clks[i].dev_id, "pxa2xx-pcmcia"))
			continue;

		pxa_clk = kzalloc_obj(*pxa_clk);
		if (!pxa_clk)
			return -ENOMEM;
		pxa_clk->is_in_low_power = clks[i].is_in_low_power;
		pxa_clk->lp = clks[i].lp;
		pxa_clk->hp = clks[i].hp;
		pxa_clk->gate = clks[i].gate;
		pxa_clk->gate.reg = clk_regs + clks[i].cken_reg;
		pxa_clk->gate.lock = &pxa_clk_lock;
		clk = clk_register_composite(NULL, clks[i].name,
					     clks[i].parent_names, 2,
					     &pxa_clk->hw, &cken_mux_ops,
					     &pxa_clk->hw, &cken_rate_ops,
					     &pxa_clk->gate.hw, &clk_gate_ops,
					     clks[i].flags);
		clkdev_pxa_register(clks[i].ckid, clks[i].con_id,
				    clks[i].dev_id, clk);
	}

	return 0;
}

void __init clk_pxa_dt_common_init(struct device_node *np)
{
	of_clk_add_provider(np, of_clk_src_onecell_get, &onecell_data);
}

void pxa2xx_core_turbo_switch(bool on)
{
	unsigned long flags;
	unsigned int unused, clkcfg;

	local_irq_save(flags);

	asm("mrc p14, 0, %0, c6, c0, 0" : "=r" (clkcfg));
	clkcfg &= ~CLKCFG_TURBO & ~CLKCFG_HALFTURBO;
	if (on)
		clkcfg |= CLKCFG_TURBO;
	clkcfg |= CLKCFG_FCS;

	asm volatile(
	"	b	2f\n"
	"	.align	5\n"
	"1:	mcr	p14, 0, %1, c6, c0, 0\n"
	"	b	3f\n"
	"2:	b	1b\n"
	"3:	nop\n"
		: "=&r" (unused) : "r" (clkcfg));

	local_irq_restore(flags);
}

void pxa2xx_cpll_change(struct pxa2xx_freq *freq,
			u32 (*mdrefr_dri)(unsigned int),
			void __iomem *cccr)
{
	unsigned int clkcfg = freq->clkcfg;
	unsigned int unused, preset_mdrefr, postset_mdrefr;
	unsigned long flags;
	void __iomem *mdrefr = pxa_smemc_get_mdrefr();

	local_irq_save(flags);

	/* Calculate the next MDREFR.  If we're slowing down the SDRAM clock
	 * we need to preset the smaller DRI before the change.	 If we're
	 * speeding up we need to set the larger DRI value after the change.
	 */
	preset_mdrefr = postset_mdrefr = readl(mdrefr);
	if ((preset_mdrefr & MDREFR_DRI_MASK) > mdrefr_dri(freq->membus_khz)) {
		preset_mdrefr = (preset_mdrefr & ~MDREFR_DRI_MASK);
		preset_mdrefr |= mdrefr_dri(freq->membus_khz);
	}
	postset_mdrefr =
		(postset_mdrefr & ~MDREFR_DRI_MASK) |
		mdrefr_dri(freq->membus_khz);

	/* If we're dividing the memory clock by two for the SDRAM clock, this
	 * must be set prior to the change.  Clearing the divide must be done
	 * after the change.
	 */
	if (freq->div2) {
		preset_mdrefr  |= MDREFR_DB2_MASK;
		postset_mdrefr |= MDREFR_DB2_MASK;
	} else {
		postset_mdrefr &= ~MDREFR_DB2_MASK;
	}

	/* Set new the CCCR and prepare CLKCFG */
	writel(freq->cccr, cccr);

	asm volatile(
	"	ldr	r4, [%1]\n"
	"	b	2f\n"
	"	.align	5\n"
	"1:	str	%3, [%1]		/* preset the MDREFR */\n"
	"	mcr	p14, 0, %2, c6, c0, 0	/* set CLKCFG[FCS] */\n"
	"	str	%4, [%1]		/* postset the MDREFR */\n"
	"	b	3f\n"
	"2:	b	1b\n"
	"3:	nop\n"
	     : "=&r" (unused)
	     : "r" (mdrefr), "r" (clkcfg), "r" (preset_mdrefr),
	       "r" (postset_mdrefr)
	     : "r4", "r5");

	local_irq_restore(flags);
}

int pxa2xx_determine_rate(struct clk_rate_request *req,
			  struct pxa2xx_freq *freqs, int nb_freqs)
{
	int i, closest_below = -1, closest_above = -1;
	unsigned long rate;

	for (i = 0; i < nb_freqs; i++) {
		rate = freqs[i].cpll;
		if (rate == req->rate)
			break;
		if (rate < req->min_rate)
			continue;
		if (rate > req->max_rate)
			continue;
		if (rate <= req->rate)
			closest_below = i;
		if ((rate >= req->rate) && (closest_above == -1))
			closest_above = i;
	}

	req->best_parent_hw = NULL;

	if (i < nb_freqs) {
		rate = req->rate;
	} else if (closest_below >= 0) {
		rate = freqs[closest_below].cpll;
	} else if (closest_above >= 0) {
		rate = freqs[closest_above].cpll;
	} else {
		pr_debug("%s(rate=%lu) no match\n", __func__, req->rate);
		return -EINVAL;
	}

	pr_debug("%s(rate=%lu) rate=%lu\n", __func__, req->rate, rate);
	req->rate = rate;

	return 0;
}
