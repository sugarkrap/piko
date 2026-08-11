#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/soc/pxa/cpu.h>
#include <linux/soc/pxa/smemc.h>
#include <linux/clk/pxa.h>

#include <asm/mach/map.h>
#include <asm/mach-types.h>

#include "addr-map.h"
#include "irqs.h"
#include "reset.h"
#include "smemc.h"
#include "pxa3xx-regs.h"

#include "generic.h"
#include <clocksource/pxa.h>

void clear_reset_status(unsigned int mask)
{
	if (cpu_is_pxa2xx())
		pxa2xx_clear_reset_status(mask);
	else {
		ARSR = mask;
	}
}

void __init pxa_timer_init(void)
{
	if (cpu_is_pxa25x())
		pxa25x_clocks_init(io_p2v(0x41300000));
	if (cpu_is_pxa27x())
		pxa27x_clocks_init(io_p2v(0x41300000));
	if (cpu_is_pxa3xx())
		pxa3xx_clocks_init(io_p2v(0x41340000), io_p2v(0x41350000));
	pxa_timer_nodt_init(IRQ_OST0, io_p2v(0x40a00000));
}

void pxa_smemc_set_pcmcia_timing(int sock, u32 mcmem, u32 mcatt, u32 mcio)
{
	__raw_writel(mcmem, MCMEM(sock));
	__raw_writel(mcatt, MCATT(sock));
	__raw_writel(mcio, MCIO(sock));
}
EXPORT_SYMBOL_GPL(pxa_smemc_set_pcmcia_timing);

void pxa_smemc_set_pcmcia_socket(int nr)
{
	switch (nr) {
	case 0:
		__raw_writel(0, MECR);
		break;
	case 1:
		__raw_writel(MECR_CIT, MECR);
		break;
	case 2:
		__raw_writel(MECR_CIT | MECR_NOS, MECR);
		break;
	}
}
EXPORT_SYMBOL_GPL(pxa_smemc_set_pcmcia_socket);

void __iomem *pxa_smemc_get_mdrefr(void)
{
	return MDREFR;
}

static struct map_desc common_io_desc[] __initdata = {
  	{
		.virtual	= (unsigned long)PERIPH_VIRT,
		.pfn		= __phys_to_pfn(PERIPH_PHYS),
		.length		= PERIPH_SIZE,
		.type		= MT_DEVICE
	}
};

void __init pxa_map_io(void)
{
	debug_ll_io_init();
	iotable_init(ARRAY_AND_SIZE(common_io_desc));
}
