// SPDX-License-Identifier: GPL-2.0-only
/*
 * Support for Sharp SL-C7xx PDAs
 * Models: SL-C700 (Corgi), SL-C750 (Shepherd), SL-C760 (Husky)
 *
 * Copyright (c) 2004-2005 Richard Purdie
 *
 * Based on Sharp's 2.4 kernel patches/lubbock.c
 */

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>	/* symbol_get ; symbol_put */
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/leds.h>
#include <linux/mmc/host.h>
#include <linux/mtd/physmap.h>
#include <linux/pm.h>
#include <linux/gpio.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/property.h>
#include <linux/property.h>
#include <linux/backlight.h>
#include <linux/i2c.h>
#include <linux/platform_data/i2c-pxa.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/regulator/machine.h>
#include <linux/spi/spi.h>
#include <linux/spi/ads7846.h>
#include <linux/spi/corgi_lcd.h>
#include <linux/mtd/sharpsl.h>
#include <linux/pxa2xx_ssp.h>

#include "irqs.h"
#include <linux/input-event-codes.h>
#include <linux/input/matrix_keypad.h>
#include <linux/gpio_keys.h>
#include <linux/memblock.h>
#include <video/w100fb.h>

#include <asm/setup.h>
#include <asm/memory.h>
#include <asm/mach-types.h>
#include <asm/irq.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>

#include "pxa25x.h"
#include <linux/platform_data/mmc-pxamci.h>
#include "corgi.h"
#include "sharpsl_pm.h"

#include <asm/mach/sharpsl_param.h>
#include <asm/hardware/scoop.h>

#include "generic.h"
#include "devices.h"

static unsigned long corgi_pin_config[] __initdata = {
	/* Static Memory I/O */
	GPIO78_nCS_2,	/* w100fb */
	GPIO80_nCS_4,	/* scoop */

	/* SSP1 */
	GPIO23_SSP1_SCLK,
	GPIO25_SSP1_TXD,
	GPIO26_SSP1_RXD,
	GPIO24_GPIO,	/* CORGI_GPIO_ADS7846_CS - SFRM as chip select */

	/* I2S */
	GPIO28_I2S_BITCLK_OUT,
	GPIO29_I2S_SDATA_IN,
	GPIO30_I2S_SDATA_OUT,
	GPIO31_I2S_SYNC,
	GPIO32_I2S_SYSCLK,

	/* Infra-Red */
	GPIO47_FICP_TXD,
	GPIO46_FICP_RXD,

	/* FFUART */
	GPIO40_FFUART_DTR,
	GPIO41_FFUART_RTS,
	GPIO39_FFUART_TXD,
	GPIO37_FFUART_DSR,
	GPIO34_FFUART_RXD,
	GPIO35_FFUART_CTS,

	/* PC Card */
	GPIO48_nPOE,
	GPIO49_nPWE,
	GPIO50_nPIOR,
	GPIO51_nPIOW,
	GPIO52_nPCE_1,
	GPIO53_nPCE_2,
	GPIO54_nPSKTSEL,
	GPIO55_nPREG,
	GPIO56_nPWAIT,
	GPIO57_nIOIS16,

	/* MMC */
	GPIO6_MMC_CLK,
	GPIO8_MMC_CS0,

	/* GPIO Matrix Keypad */
	GPIO66_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 0 */
	GPIO67_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 1 */
	GPIO68_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 2 */
	GPIO69_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 3 */
	GPIO70_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 4 */
	GPIO71_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 5 */
	GPIO72_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 6 */
	GPIO73_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 7 */
	GPIO74_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 8 */
	GPIO75_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 9 */
	GPIO76_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 10 */
	GPIO77_GPIO | MFP_LPM_DRIVE_HIGH,	/* column 11 */
	GPIO58_GPIO,	/* row 0 */
	GPIO59_GPIO,	/* row 1 */
	GPIO60_GPIO,	/* row 2 */
	GPIO61_GPIO,	/* row 3 */
	GPIO62_GPIO,	/* row 4 */
	GPIO63_GPIO,	/* row 5 */
	GPIO64_GPIO,	/* row 6 */
	GPIO65_GPIO,	/* row 7 */

	/* GPIO */
	GPIO9_GPIO,				/* CORGI_GPIO_nSD_DETECT */
	GPIO7_GPIO,				/* CORGI_GPIO_nSD_WP */
	GPIO11_GPIO | WAKEUP_ON_EDGE_BOTH,	/* CORGI_GPIO_MAIN_BAT_{LOW,COVER} */
	GPIO13_GPIO | MFP_LPM_KEEP_OUTPUT,	/* CORGI_GPIO_LED_ORANGE */
	GPIO21_GPIO,				/* CORGI_GPIO_ADC_TEMP */
	GPIO22_GPIO,				/* CORGI_GPIO_IR_ON */
	GPIO33_GPIO,				/* CORGI_GPIO_SD_PWR */
	GPIO38_GPIO | MFP_LPM_KEEP_OUTPUT,	/* CORGI_GPIO_CHRG_ON */
	GPIO43_GPIO | MFP_LPM_KEEP_OUTPUT,	/* CORGI_GPIO_CHRG_UKN */
	GPIO44_GPIO,				/* CORGI_GPIO_HSYNC */

	GPIO0_GPIO | WAKEUP_ON_EDGE_BOTH,	/* CORGI_GPIO_KEY_INT */
	GPIO1_GPIO | WAKEUP_ON_EDGE_RISE,	/* CORGI_GPIO_AC_IN */
	GPIO3_GPIO | WAKEUP_ON_EDGE_BOTH,	/* CORGI_GPIO_WAKEUP */
};

/*
 * Corgi SCOOP Device
 */
static struct resource corgi_scoop_resources[] = {
	[0] = {
		.start		= 0x10800000,
		.end		= 0x10800fff,
		.flags		= IORESOURCE_MEM,
	},
};

static struct scoop_config corgi_scoop_setup = {
	.io_dir 	= CORGI_SCOOP_IO_DIR,
	.io_out		= CORGI_SCOOP_IO_OUT,
	.gpio_base	= CORGI_SCOOP_GPIO_BASE,
};

struct platform_device corgiscoop_device = {
	.name		= "sharp-scoop",
	.id		= -1,
	.dev		= {
 		.platform_data	= &corgi_scoop_setup,
	},
	.num_resources	= ARRAY_SIZE(corgi_scoop_resources),
	.resource	= corgi_scoop_resources,
};

static struct scoop_pcmcia_dev corgi_pcmcia_scoop[] = {
{
	.dev        = &corgiscoop_device.dev,
	.irq        = CORGI_IRQ_GPIO_CF_IRQ,
	.cd_irq     = CORGI_IRQ_GPIO_CF_CD,
	.cd_irq_str = "PCMCIA0 CD",
},
};

static struct scoop_pcmcia_config corgi_pcmcia_config = {
	.devs         = &corgi_pcmcia_scoop[0],
	.num_devs     = 1,
};

static struct w100_mem_info corgi_fb_mem = {
	.ext_cntl          = 0x00040003,
	.sdram_mode_reg    = 0x00650021,
	.ext_timing_cntl   = 0x10002a4a,
	.io_cntl           = 0x7ff87012,
	.size              = 0x1fffff,
};

static struct w100_gen_regs corgi_fb_regs = {
	.lcd_format    = 0x00000003,
	.lcdd_cntl1    = 0x01CC0000,
	.lcdd_cntl2    = 0x0003FFFF,
	.genlcd_cntl1  = 0x00FFFF0D,
	.genlcd_cntl2  = 0x003F3003,
	.genlcd_cntl3  = 0x000102aa,
};

static struct w100_gpio_regs corgi_fb_gpio = {
	.init_data1   = 0x000000bf,
	.init_data2   = 0x00000000,
	.gpio_dir1    = 0x00000000,
	.gpio_oe1     = 0x03c0feff,
	.gpio_dir2    = 0x00000000,
	.gpio_oe2     = 0x00000000,
};

static struct w100_mode corgi_fb_modes[] = {
{
	.xres            = 480,
	.yres            = 640,
	.left_margin     = 0x56,
	.right_margin    = 0x55,
	.upper_margin    = 0x03,
	.lower_margin    = 0x00,
	.crtc_ss         = 0x82360056,
	.crtc_ls         = 0xA0280000,
	.crtc_gs         = 0x80280028,
	.crtc_vpos_gs    = 0x02830002,
	.crtc_rev        = 0x00400008,
	.crtc_dclk       = 0xA0000000,
	.crtc_gclk       = 0x8015010F,
	.crtc_goe        = 0x80100110,
	.crtc_ps1_active = 0x41060010,
	.pll_freq        = 75,
	.fast_pll_freq   = 100,
	.sysclk_src      = CLK_SRC_PLL,
	.sysclk_divider  = 0,
	.pixclk_src      = CLK_SRC_PLL,
	.pixclk_divider  = 2,
	.pixclk_divider_rotated = 6,
},{
	.xres            = 240,
	.yres            = 320,
	.left_margin     = 0x27,
	.right_margin    = 0x2e,
	.upper_margin    = 0x01,
	.lower_margin    = 0x00,
	.crtc_ss         = 0x81170027,
	.crtc_ls         = 0xA0140000,
	.crtc_gs         = 0xC0140014,
	.crtc_vpos_gs    = 0x00010141,
	.crtc_rev        = 0x00400008,
	.crtc_dclk       = 0xA0000000,
	.crtc_gclk       = 0x8015010F,
	.crtc_goe        = 0x80100110,
	.crtc_ps1_active = 0x41060010,
	.pll_freq        = 0,
	.fast_pll_freq   = 0,
	.sysclk_src      = CLK_SRC_XTAL,
	.sysclk_divider  = 0,
	.pixclk_src      = CLK_SRC_XTAL,
	.pixclk_divider  = 1,
	.pixclk_divider_rotated = 1,
},

};

static struct w100fb_mach_info corgi_fb_info = {
	.init_mode  = INIT_MODE_ROTATED,
	.mem        = &corgi_fb_mem,
	.regs       = &corgi_fb_regs,
	.modelist   = &corgi_fb_modes[0],
	.num_modes  = 2,
	.gpio       = &corgi_fb_gpio,
	.xtal_freq  = 12500000,
	.xtal_dbl   = 0,
};

static struct resource corgi_fb_resources[] = {
	[0] = {
		.start   = 0x08000000,
		.end     = 0x08ffffff,
		.flags   = IORESOURCE_MEM,
	},
};

static struct platform_device corgifb_device = {
	.name           = "w100fb",
	.id             = -1,
	.num_resources	= ARRAY_SIZE(corgi_fb_resources),
	.resource	= corgi_fb_resources,
	.dev            = {
		.platform_data = &corgi_fb_info,
	},

};

/*
 * Corgi Keyboard Device
 */
#define CORGI_KEY_CALENDER	KEY_F1
#define CORGI_KEY_ADDRESS	KEY_F2
#define CORGI_KEY_FN		KEY_F3
#define CORGI_KEY_CANCEL	KEY_F4
#define CORGI_KEY_OFF		KEY_SUSPEND
#define CORGI_KEY_EXOK		KEY_F5
#define CORGI_KEY_EXCANCEL	KEY_F6
#define CORGI_KEY_EXJOGDOWN	KEY_F7
#define CORGI_KEY_EXJOGUP	KEY_F8
#define CORGI_KEY_JAP1		KEY_LEFTCTRL
#define CORGI_KEY_JAP2		KEY_LEFTALT
#define CORGI_KEY_MAIL		KEY_F10
#define CORGI_KEY_OK		KEY_F11
#define CORGI_KEY_MENU		KEY_F12

static const uint32_t corgikbd_keymap[] = {
	KEY(0, 1, KEY_1),
	KEY(0, 2, KEY_3),
	KEY(0, 3, KEY_5),
	KEY(0, 4, KEY_6),
	KEY(0, 5, KEY_7),
	KEY(0, 6, KEY_9),
	KEY(0, 7, KEY_0),
	KEY(0, 8, KEY_BACKSPACE),
	KEY(1, 1, KEY_2),
	KEY(1, 2, KEY_4),
	KEY(1, 3, KEY_R),
	KEY(1, 4, KEY_Y),
	KEY(1, 5, KEY_8),
	KEY(1, 6, KEY_I),
	KEY(1, 7, KEY_O),
	KEY(1, 8, KEY_P),
	KEY(2, 0, KEY_TAB),
	KEY(2, 1, KEY_Q),
	KEY(2, 2, KEY_E),
	KEY(2, 3, KEY_T),
	KEY(2, 4, KEY_G),
	KEY(2, 5, KEY_U),
	KEY(2, 6, KEY_J),
	KEY(2, 7, KEY_K),
	KEY(3, 0, CORGI_KEY_CALENDER),
	KEY(3, 1, KEY_W),
	KEY(3, 2, KEY_S),
	KEY(3, 3, KEY_F),
	KEY(3, 4, KEY_V),
	KEY(3, 5, KEY_H),
	KEY(3, 6, KEY_M),
	KEY(3, 7, KEY_L),
	KEY(3, 9, KEY_RIGHTSHIFT),
	KEY(4, 0, CORGI_KEY_ADDRESS),
	KEY(4, 1, KEY_A),
	KEY(4, 2, KEY_D),
	KEY(4, 3, KEY_C),
	KEY(4, 4, KEY_B),
	KEY(4, 5, KEY_N),
	KEY(4, 6, KEY_DOT),
	KEY(4, 8, KEY_ENTER),
	KEY(4, 10, KEY_LEFTSHIFT),
	KEY(5, 0, CORGI_KEY_MAIL),
	KEY(5, 1, KEY_Z),
	KEY(5, 2, KEY_X),
	KEY(5, 3, KEY_MINUS),
	KEY(5, 4, KEY_SPACE),
	KEY(5, 5, KEY_COMMA),
	KEY(5, 7, KEY_UP),
	KEY(5, 11, CORGI_KEY_FN),
	KEY(6, 0, KEY_SYSRQ),
	KEY(6, 1, CORGI_KEY_JAP1),
	KEY(6, 2, CORGI_KEY_JAP2),
	KEY(6, 3, CORGI_KEY_CANCEL),
	KEY(6, 4, CORGI_KEY_OK),
	KEY(6, 5, CORGI_KEY_MENU),
	KEY(6, 6, KEY_LEFT),
	KEY(6, 7, KEY_DOWN),
	KEY(6, 8, KEY_RIGHT),
	KEY(7, 0, CORGI_KEY_OFF),
	KEY(7, 1, CORGI_KEY_EXOK),
	KEY(7, 2, CORGI_KEY_EXCANCEL),
	KEY(7, 3, CORGI_KEY_EXJOGDOWN),
	KEY(7, 4, CORGI_KEY_EXJOGUP),
};

/*
 * matrix_keypad_platform_data was removed from current mainline in favour
 * of software_node/property_entry instantiation, matching spitz.c's
 * spitz_mkp_init(). Row/col GPIO numbers are unchanged from the old
 * corgikbd_row_gpios[]/corgikbd_col_gpios[] arrays.
 */
static const struct software_node_ref_args corgikbd_row_gpios[] = {
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 58, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 59, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 60, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 61, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 62, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 63, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 64, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 65, GPIO_ACTIVE_HIGH),
};

static const struct software_node_ref_args corgikbd_col_gpios[] = {
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 66, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 67, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 68, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 69, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 70, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 71, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 72, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 73, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 74, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 75, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 76, GPIO_ACTIVE_HIGH),
	SOFTWARE_NODE_REFERENCE(&pxa2xx_gpiochip_node, 77, GPIO_ACTIVE_HIGH),
};

static const struct property_entry corgikbd_properties[] = {
	PROPERTY_ENTRY_U32_ARRAY("linux,keymap", corgikbd_keymap),
	PROPERTY_ENTRY_REF_ARRAY("row-gpios", corgikbd_row_gpios),
	PROPERTY_ENTRY_REF_ARRAY("col-gpios", corgikbd_col_gpios),
	PROPERTY_ENTRY_U32("col-scan-delay-us", 10),
	PROPERTY_ENTRY_U32("debounce-delay-ms", 10),
	PROPERTY_ENTRY_BOOL("wakeup-source"),
	{ }
};

static const struct platform_device_info corgikbd_info __initconst = {
	.name		= "matrix-keypad",
	.id		= PLATFORM_DEVID_NONE,
	.properties	= corgikbd_properties,
};

static void __init corgi_keypad_init(void)
{
	struct platform_device *pd;
	int err;

	pd = platform_device_register_full(&corgikbd_info);
	err = PTR_ERR_OR_ZERO(pd);
	if (err)
		pr_err("failed to create keypad device: %d\n", err);
}

static struct gpio_keys_button corgi_gpio_keys[] = {
	{
		.type	= EV_SW,
		.code	= SW_LID,
		.gpio	= CORGI_GPIO_SWA,
		.desc	= "Lid close switch",
		.debounce_interval = 500,
	},
	{
		.type	= EV_SW,
		.code	= SW_TABLET_MODE,
		.gpio	= CORGI_GPIO_SWB,
		.desc	= "Tablet mode switch",
		.debounce_interval = 500,
	},
	{
		.type	= EV_SW,
		.code	= SW_HEADPHONE_INSERT,
		.gpio	= CORGI_GPIO_AK_INT,
		.desc	= "HeadPhone insert",
		.debounce_interval = 500,
	},
};

static struct gpio_keys_platform_data corgi_gpio_keys_platform_data = {
	.buttons	= corgi_gpio_keys,
	.nbuttons	= ARRAY_SIZE(corgi_gpio_keys),
	.poll_interval	= 250,
};

static struct platform_device corgi_gpio_keys_device = {
	.name	= "gpio-keys-polled",
	.id	= -1,
	.dev	= {
		.platform_data	= &corgi_gpio_keys_platform_data,
	},
};

/*
 * Corgi LEDs
 *
 * Amber/charge LED (GPIO13, direct PXA GPIO): the board -- not the kernel --
 * owns this LED. GPIO13 is an *enable*, not a plain output: with it high the
 * charger circuit lights and extinguishes the LED itself according to real
 * adapter presence, and with it low the LED stays dark no matter what.
 * Sharp's own kernels exploited exactly that and simply drove it high once
 * at boot, so that is what we do -- DEFSTATE_ON with both retain_state flags,
 * no trigger, nobody touching it again for the life of the boot. The old
 * "sharpsl-charge" trigger only re-derived in software (via ACIN polling in
 * sharpsl_pm.c) a decision the hardware had already made, and could only ever
 * disagree with the panel. The second half of the enable, CORGI_GPIO_CHRG_ON,
 * is likewise held open permanently -- see corgi_charger_init() in corgi_pm.c.
 *
 * Green LED (SCOOP PA11, via CORGI_GPIO_LED_GREEN): originally a stock
 * "mail" notification LED, repurposed here as a drive/flash-access
 * indicator using the in-kernel "nand-disk" trigger (drivers/leds/trigger/
 * ledtrig-mtd.c, fired from mtdcore.c on every MTD read/write/erase) --
 * requires CONFIG_LEDS_TRIGGER_MTD=y.
 */
static struct gpio_led corgi_gpio_leds[] = {
	{
		.name			= "corgi:amber:charge",
		.gpio			= CORGI_GPIO_LED_ORANGE,
		.default_state		= LEDS_GPIO_DEFSTATE_ON,
		.retain_state_suspended	= 1,
		.retain_state_shutdown	= 1,
	},
	{
		.name			= "corgi:green:drive",
		.default_trigger	= "nand-disk",
		.gpio			= CORGI_GPIO_LED_GREEN,
	},
};

static struct gpio_led_platform_data corgi_gpio_leds_info = {
	.leds		= corgi_gpio_leds,
	.num_leds	= ARRAY_SIZE(corgi_gpio_leds),
};

static struct platform_device corgiled_device = {
	.name		= "leds-gpio",
	.id		= -1,
	.dev		= {
		.platform_data = &corgi_gpio_leds_info,
	},
};

static struct gpiod_lookup_table corgi_audio_gpio_table = {
	.dev_id = "corgi-audio",
	.table = {
		GPIO_LOOKUP("sharp-scoop",
			    CORGI_GPIO_MUTE_L - CORGI_SCOOP_GPIO_BASE,
			    "mute-l", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("sharp-scoop",
			    CORGI_GPIO_MUTE_R - CORGI_SCOOP_GPIO_BASE,
			    "mute-r", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("sharp-scoop",
			    CORGI_GPIO_APM_ON - CORGI_SCOOP_GPIO_BASE,
			    "apm-on", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("sharp-scoop",
			    CORGI_GPIO_MIC_BIAS - CORGI_SCOOP_GPIO_BASE,
			    "mic-bias", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/*
 * Corgi Audio
 */
static u64 corgi_audio_dma_mask = DMA_BIT_MASK(32);

/*
 * pxa2xx_soc_pcm_new() (sound/arm/pxa2xx-pcm-lib.c) calls
 * dma_coerce_mask_and_coherent() on rtd->card->snd_card->dev, which is
 * *this* device (the "corgi-audio" card device registered by
 * snd_soc_register_card(), not the "pxa-pcm-audio" platform device) --
 * found 2026-07-26 after fixing pxa_device_asoc_platform's dma_mask in
 * devices.c did NOT resolve the same -EINVAL(-22) "can't create pcm
 * WM8731" error. Without a valid dma_mask here, dma_coerce_mask_and_coherent()
 * fails and snd_soc_pcm_component_new() propagates -EINVAL up through
 * snd_soc_register_card() / corgi_probe().
 */
static struct platform_device corgi_audio_device = {
	.name	= "corgi-audio",
	.id	= -1,
	.dev	= {
		.dma_mask = &corgi_audio_dma_mask,
		.coherent_dma_mask = DMA_BIT_MASK(32),
	},
};

/*
 * MMC/SD Device
 *
 * The card detect interrupt isn't debounced so we delay it by 250ms
 * to give the card a chance to fully insert/eject.
 */
static struct pxamci_platform_data corgi_mci_platform_data = {
	.detect_delay_ms	= 250,
	.ocr_mask		= MMC_VDD_32_33|MMC_VDD_33_34,
};

static struct gpiod_lookup_table corgi_mci_gpio_table = {
	.dev_id = "pxa2xx-mci.0",
	.table = {
		/* Card detect on GPIO 9 */
		GPIO_LOOKUP("gpio-pxa", CORGI_GPIO_nSD_DETECT,
			    "cd", GPIO_ACTIVE_LOW),
		/* Write protect on GPIO 7 */
		GPIO_LOOKUP("gpio-pxa", CORGI_GPIO_nSD_WP,
			    "wp", GPIO_ACTIVE_LOW),
		/* Power on GPIO 33 */
		GPIO_LOOKUP("gpio-pxa", CORGI_GPIO_SD_PWR,
			    "power", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/*
 * Husky boards can report inverted SD detect/write-protect levels compared
 * to early Corgi wiring. Keep legacy polarity on Corgi/Shepherd and use a
 * Husky-specific table so mmc core sees card-present correctly.
 */
static struct gpiod_lookup_table husky_mci_gpio_table = {
	.dev_id = "pxa2xx-mci.0",
	.table = {
		/*
		 * Husky CD wiring is unreliable with current mainline descriptor
		 * handling. Intentionally omit "cd" so mmc core does not gate probe
		 * on a potentially wrong card-detect level.
		 */
		/* Write protect on GPIO 7 (Husky quirk: active high) */
		GPIO_LOOKUP("gpio-pxa", CORGI_GPIO_nSD_WP,
			    "wp", GPIO_ACTIVE_HIGH),
		/* Power on GPIO 33 */
		GPIO_LOOKUP("gpio-pxa", CORGI_GPIO_SD_PWR,
			    "power", GPIO_ACTIVE_HIGH),
		{ },
	},
};

/*
 * USB Device Controller
 *
 * pxa25x_udc.c dropped platform_data support (Apr 2026) in favour of a
 * "pullup" GPIO descriptor looked up against dev_name() == "pxa25x-udc".
 * No connect GPIO; corgi can't tell connection status.
 */
static struct gpiod_lookup_table corgi_udc_gpio_table = {
	.dev_id = "pxa25x-udc",
	.table = {
		GPIO_LOOKUP_IDX("gpio-pxa", CORGI_GPIO_USB_PULLUP, "pullup", 0, GPIO_ACTIVE_HIGH),
		{ },
	},
};

#if IS_ENABLED(CONFIG_SPI_PXA2XX)
/*
 * pxa2xx-spi shares the SSP port that the pxa25x-ssp core driver already
 * enumerated (matched via pxa_ssp_request() on port id = pdev->id + 1 --
 * drivers/soc/pxa/ssp.c does that +1 translation itself, with the comment
 * "PXA2xx/3xx SSP ports starts from 1 and the internal pdev->id starts
 * from 0"). pxa25x_device_ssp (arch/arm/mach-pxa/devices.c) is .id = 0,
 * so it lands on port_id 1 -- hence .id = 1 here.
 * We must still tell the driver which SSP variant it is and how many chip
 * selects exist, or pxa2xx_spi_init_pdata() bails with "missing platform
 * data" / creates only one chip select.
 *
 * NB: getting this right is necessary but NOT sufficient -- stock
 * spi-pxa2xx-platform.c requests the SSP twice and the second request
 * always fails, which is what actually kept this bus down. See
 * modules/spi/spi_pxa2xx_platform_patched.c.
 */
static const struct property_entry corgi_spi_properties[] = {
	PROPERTY_ENTRY_U32("intel,spi-pxa2xx-type", PXA25x_SSP),
	PROPERTY_ENTRY_U32("num-cs", 3),
	{ }
};

static const struct platform_device_info corgi_spi_device_info = {
	.name = "pxa2xx-spi",
	.id = 1,	/* must match corgi_spi_devices[].bus_num below */
	.properties = corgi_spi_properties,
};

static struct gpiod_lookup_table corgi_spi_gpio_table = {
	.dev_id = "spi1",
	.table = {
		GPIO_LOOKUP_IDX("gpio-pxa", CORGI_GPIO_ADS7846_CS, "cs", 0, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX("gpio-pxa", CORGI_GPIO_LCDCON_CS, "cs", 1, GPIO_ACTIVE_LOW),
		GPIO_LOOKUP_IDX("gpio-pxa", CORGI_GPIO_MAX1111_CS, "cs", 2, GPIO_ACTIVE_LOW),
		{ },
	},
};

/*
 * ads7846_platform_data was removed from current mainline; the driver now
 * takes these as device properties (including the hsync GPIO itself,
 * replacing the old wait_for_sync() board callback), matching
 * spitz_ads7846_props in spitz.c.
 */
static const struct property_entry corgi_ads7846_props[] = {
	PROPERTY_ENTRY_STRING("compatible", "ti,ads7846"),
	PROPERTY_ENTRY_U32("touchscreen-max-pressure", 1024),
	PROPERTY_ENTRY_U16("ti,x-plate-ohms", 419),
	PROPERTY_ENTRY_U16("ti,y-plate-ohms", 486),
	PROPERTY_ENTRY_U16("ti,vref-delay-usecs", 100),
	PROPERTY_ENTRY_GPIO("pendown-gpios", &pxa2xx_gpiochip_node,
			    CORGI_GPIO_TP_INT, GPIO_ACTIVE_LOW),
	PROPERTY_ENTRY_GPIO("ti,hsync-gpios", &pxa2xx_gpiochip_node,
			    CORGI_GPIO_HSYNC, GPIO_ACTIVE_LOW),
	{ }
};

static const struct software_node corgi_ads7846_swnode = {
	.name = "ads7846",
	.properties = corgi_ads7846_props,
};

static void corgi_bl_kick_battery(void)
{
	void (*kick_batt)(void);

	kick_batt = symbol_get(sharpsl_battery_kick);
	if (kick_batt) {
		kick_batt();
		symbol_put(sharpsl_battery_kick);
	}
}

static struct gpiod_lookup_table corgi_lcdcon_gpio_table = {
	.dev_id = "spi1.1",
	.table = {
		/*
		 * BL_CONT is a SCOOP gpio (CORGI_GPIO_BACKLIGHT_CONT =
		 * CORGI_SCOOP_GPIO_BASE + 7), not a PXA gpio. The old entry
		 * named chip "gpio-pxa" with the scoop-absolute number
		 * (PXA_NR_BUILTIN_GPIO + 7 = 199), so corgi-lcd's probe failed
		 * with "requested GPIO 199 out of range [0..84] for gpio-pxa"
		 * (-EINVAL) and there was no backlight/lcd device at all.
		 * Look it up on the scoop chip by its relative offset, exactly
		 * like corgi_audio_gpio_table does for the other scoop lines.
		 */
		GPIO_LOOKUP("sharp-scoop",
			    CORGI_GPIO_BACKLIGHT_CONT - CORGI_SCOOP_GPIO_BASE,
			    "BL_CONT", GPIO_ACTIVE_HIGH),
		{ },
	},
};

static struct corgi_lcd_platform_data corgi_lcdcon_info = {
	.init_mode		= CORGI_LCD_MODE_VGA,
	.max_intensity		= 0x2f,
	.default_intensity	= 0x1f,
	.limit_mask		= 0x0b,
	.kick_battery		= corgi_bl_kick_battery,
};

static struct spi_board_info corgi_spi_devices[] = {
	{
		.modalias	= "ads7846",
		.max_speed_hz	= 1200000,
		.bus_num	= 1,
		.chip_select	= 0,
		.swnode		= &corgi_ads7846_swnode,
		.irq		= PXA_GPIO_TO_IRQ(CORGI_GPIO_TP_INT),
	}, {
		.modalias	= "corgi-lcd",
		.max_speed_hz	= 50000,
		.bus_num	= 1,
		.chip_select	= 1,
		.platform_data	= &corgi_lcdcon_info,
	}, {
		.modalias	= "max1111",
		.max_speed_hz	= 450000,
		.bus_num	= 1,
		.chip_select	= 2,
	},
};

static void __init corgi_init_spi(void)
{
	struct platform_device *pd;
	int err;

	gpiod_add_lookup_table(&corgi_spi_gpio_table);
	/* pxa2xx_set_spi_info()/pxa2xx_spi_controller were removed from current
	 * mainline; pxa2xx-spi is now instantiated as a plain platform device,
	 * same as spitz.c does. */
	pd = platform_device_register_full(&corgi_spi_device_info);
	err = PTR_ERR_OR_ZERO(pd);
	if (err)
		pr_err("pxa2xx-spi: failed to instantiate SPI controller: %d\n", err);
	gpiod_add_lookup_table(&corgi_lcdcon_gpio_table);
	spi_register_board_info(ARRAY_AND_SIZE(corgi_spi_devices));
}
#else
static inline void corgi_init_spi(void) {}
#endif

static uint8_t scan_ff_pattern[] = { 0xff, 0xff };

static struct nand_bbt_descr sharpsl_bbt = {
	.options = 0,
	.offs = 4,
	.len = 2,
	.pattern = scan_ff_pattern
};

static const char * const probes[] = {
	"cmdlinepart",
	"ofpart",
	"sharpslpart",
	NULL,
};

/*
 * FALLBACK (2026-07-29, piko project): static partition table for when
 * sharpslpart's on-flash directory read fails.
 *
 * On real hardware, sharpslpart's own FTL scan succeeds ("Sharp SL FTL:
 * 448 blocks used (424 logical, 24 reserved)" -- matches this exact
 * board), but reading BOTH copies of the on-flash partition-info record
 * (logical 0x60000 and 0x64000, inside that same FTL area) fails. Root
 * cause not yet established -- could be genuine flash wear from the many
 * kernel flashes this project has done, or something else.
 *
 * These three offsets/sizes are not a guess: they are independently
 * confirmed twice, once from this device's own Cacko /proc/mtd
 * (docs/DEADLETTER-MACHINE-ID-196.md) and once from sharpslpart.c's own
 * source comment, which documents a reference dump taken from an actual
 * SL-C860 -- this exact model:
 *
 *   mtd1: 00700000 00004000 "smf"
 *   mtd2: 03500000 00004000 "root"
 *   mtd3: 04400000 00004000 "home"
 *
 * mtd_device_parse_register() only falls back to this table if every
 * parser in `probes` above fails first, so a working on-flash table
 * still takes priority -- this is a safety net, not a replacement.
 */
static struct mtd_partition sharpsl_nand_fallback_parts[] = {
	{ .name = "smf",  .offset = 0x00000000, .size = 0x00700000 },
	{ .name = "root", .offset = 0x00700000, .size = 0x03500000 },
	{ .name = "home", .offset = 0x03c00000, .size = 0x04400000 },
};

static struct sharpsl_nand_platform_data sharpsl_nand_platform_data = {
	.badblock_pattern	= &sharpsl_bbt,
	.part_parsers		= probes,
	.partitions		= sharpsl_nand_fallback_parts,
	.nr_partitions		= ARRAY_SIZE(sharpsl_nand_fallback_parts),
};

static struct resource sharpsl_nand_resources[] = {
	{
		.start	= 0x0C000000,
		.end	= 0x0C000FFF,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device sharpsl_nand_device = {
	.name		= "sharpsl-nand",
	.id		= -1,
	.resource	= sharpsl_nand_resources,
	.num_resources	= ARRAY_SIZE(sharpsl_nand_resources),
	.dev.platform_data	= &sharpsl_nand_platform_data,
};

static struct mtd_partition sharpsl_rom_parts[] = {
	{
		.name	="Boot PROM Filesystem",
		.offset	= 0x00120000,
		.size	= MTDPART_SIZ_FULL,
	},
};

static struct physmap_flash_data sharpsl_rom_data = {
	.width		= 2,
	.nr_parts	= ARRAY_SIZE(sharpsl_rom_parts),
	.parts		= sharpsl_rom_parts,
};

static struct resource sharpsl_rom_resources[] = {
	{
		.start	= 0x00000000,
		.end	= 0x007fffff,
		.flags	= IORESOURCE_MEM,
	},
};

static struct platform_device sharpsl_rom_device = {
	.name	= "physmap-flash",
	.id	= -1,
	.resource = sharpsl_rom_resources,
	.num_resources = ARRAY_SIZE(sharpsl_rom_resources),
	.dev.platform_data = &sharpsl_rom_data,
};

static struct platform_device *devices[] __initdata = {
	&corgiscoop_device,
	&corgifb_device,
	&corgi_gpio_keys_device,
	&corgiled_device,
	&corgi_audio_device,
	&sharpsl_nand_device,
};

static struct i2c_board_info __initdata corgi_i2c_devices[] = {
	{ I2C_BOARD_INFO("wm8731", 0x1b) },
};

static void corgi_poweroff(void)
{
	if (!machine_is_corgi())
		/* Green LED off tells the bootloader to halt */
		gpio_set_value(CORGI_GPIO_LED_GREEN, 0);

	pxa_restart(REBOOT_HARD, NULL);
}

static void corgi_restart(enum reboot_mode mode, const char *cmd)
{
	if (!machine_is_corgi())
		/* Green LED on tells the bootloader to reboot */
		gpio_set_value(CORGI_GPIO_LED_GREEN, 1);

	pxa_restart(REBOOT_HARD, cmd);
}

static void __init corgi_init(void)
{
	pm_power_off = corgi_poweroff;

	/* Stop 3.6MHz and drive HIGH to PCMCIA and CS */
	PCFR |= PCFR_OPDE;

	pxa2xx_mfp_config(ARRAY_AND_SIZE(corgi_pin_config));

	/*
	 * COLD-BOOT W100 FRAMEBUFFER FIX (crystal warm-up).
	 *
	 * The W100 (ATI Imageon) framebuffer sits on nCS2 at 0x08000000. Its
	 * static-memory timing (MSC1.CS2) is already VLIO from the bootloader,
	 * so the bus is fine -- but on a genuine cold power-on the W100's
	 * crystal oscillator starts from a dead stop and its register interface
	 * returns garbage until the oscillator stabilises (a few ms to tens of
	 * ms). w100fb_probe() reads mmCHIP_ID immediately and, on that garbage,
	 * prints "Unknown imageon chip ID" and bails -> no fb -> dummy console
	 * -> BLACK SCREEN. Warm boots never hit this because the crystal is
	 * already running (which is why it "worked" before).
	 *
	 * Poll mmCHIP_ID here, early in machine init -- before any driver
	 * (corgi_lcd, w100fb) touches the chip -- giving the oscillator time to
	 * spin up. Once it reads back a valid Imageon id the chip stays clocked,
	 * so w100fb's own read later succeeds and it binds. These reads are safe
	 * at this early point (they return garbage, not a bus stall); the loop
	 * bails after ~1s if the chip never answers.
	 */
	{
		void __iomem *w100 = ioremap(0x08000000 + 0x10000, 0x1000);
		if (w100) {
			u32 id = 0;
			int i;
			for (i = 0; i < 200; i++) {
				id = readl(w100 + 0x0000);	/* mmCHIP_ID */
				if (id == 0x57411002 ||		/* W100  */
				    id == 0x56441002)		/* W3200 */
					break;
				mdelay(5);
			}
			pr_info("w100-warmup: chip-id=0x%08x after %d ms%s\n",
				id, i * 5,
				(id == 0x57411002 || id == 0x56441002) ?
					" (ready)" : " (TIMEOUT -- still no W100)");
			iounmap(w100);
		}
	}

	/* allow wakeup from various GPIOs */
	gpio_set_wake(CORGI_GPIO_KEY_INT, 1);
	gpio_set_wake(CORGI_GPIO_WAKEUP, 1);
	gpio_set_wake(CORGI_GPIO_AC_IN, 1);
	gpio_set_wake(CORGI_GPIO_CHRG_FULL, 1);

	if (!machine_is_corgi())
		gpio_set_wake(CORGI_GPIO_MAIN_BAT_LOW, 1);

	pxa_set_ffuart_info(NULL);
	pxa_set_btuart_info(NULL);
	pxa_set_stuart_info(NULL);

	corgi_init_spi();

 	gpiod_add_lookup_table(&corgi_udc_gpio_table);
	if (machine_is_husky())
		gpiod_add_lookup_table(&husky_mci_gpio_table);
	else
		gpiod_add_lookup_table(&corgi_mci_gpio_table);
	gpiod_add_lookup_table(&corgi_audio_gpio_table);
	pxa_set_mci_info(&corgi_mci_platform_data, NULL);
	pxa_set_i2c_info(NULL);
	i2c_register_board_info(0, ARRAY_AND_SIZE(corgi_i2c_devices));

	platform_scoop_config = &corgi_pcmcia_config;

	platform_add_devices(devices, ARRAY_SIZE(devices));
	corgi_keypad_init();

	regulator_has_full_constraints();
}

static void __init fixup_corgi(struct tag *tags, char **cmdline)
{
	sharpsl_save_param();
	if (machine_is_corgi())
		memblock_add(0xa0000000, SZ_32M);
	else
		memblock_add(0xa0000000, SZ_64M);
}

#ifdef CONFIG_MACH_CORGI
MACHINE_START(CORGI, "SHARP Corgi")
	.fixup		= fixup_corgi,
	.map_io		= pxa25x_map_io,
	.nr_irqs	= PXA_NR_IRQS,
	.init_irq	= pxa25x_init_irq,
	.init_machine	= corgi_init,
	.init_time	= pxa_timer_init,
	.restart	= corgi_restart,
MACHINE_END
#endif

#ifdef CONFIG_MACH_SHEPHERD
MACHINE_START(SHEPHERD, "SHARP Shepherd")
	.fixup		= fixup_corgi,
	.map_io		= pxa25x_map_io,
	.nr_irqs	= PXA_NR_IRQS,
	.init_irq	= pxa25x_init_irq,
	.init_machine	= corgi_init,
	.init_time	= pxa_timer_init,
	.restart	= corgi_restart,
MACHINE_END
#endif

#ifdef CONFIG_MACH_HUSKY
MACHINE_START(HUSKY, "SHARP Husky")
	.fixup		= fixup_corgi,
	.map_io		= pxa25x_map_io,
	.nr_irqs	= PXA_NR_IRQS,
	.init_irq	= pxa25x_init_irq,
	.init_machine	= corgi_init,
	.init_time	= pxa_timer_init,
	.restart	= corgi_restart,
MACHINE_END
#endif

/*
 * Sharp's legacy machine number (196) -- the one this hardware actually
 * boots with.
 *
 * Found 2026-07-29 by decompressing the Cacko/Sharp kernel straight out of
 * the board's own NAND (SYSTC760.DBK):
 *
 *   Linux version 2.4.18-rmk7-pxa3-embedix-021129 (zaurus@sharplinux)
 *
 * That kernel contains exactly one Sharp machine descriptor, "SHARP
 * Shepherd", with nr = 196, phys_ram = 0xa0000000, phys_io = 0x40000000.
 * The bootloader consequently passes 196 in r1 -- and 196 was NEVER
 * registered in mainline's arch/arm/tools/mach-types, which only knows
 * corgi 423, poodle 424, tosa 520, husky 543, shepherd 545.
 *
 * So no mainline kernel could ever match this board: setup_machine_tags()
 * found nothing, and dump_machine_table() ended in its bare
 * `while (true);` -- a silent, console-less hang that looks exactly like a
 * failed flash. That cost an entire debugging session; see
 * docs/DEADLETTER-MACHINE-ID-196.md.
 *
 * Hardware-wise this is the same board as Corgi/Shepherd/Husky (same
 * .fixup/.map_io/.init_irq/.init_machine), and machine_is_corgi() is
 * false for 196, so it takes the identical non-Corgi path.
 */
#define MACH_TYPE_SHARP_LEGACY	196

/*
 * ...and the number the bootloader ACTUALLY passes in r1: 19.
 *
 * Read directly off the LEDs on real hardware (2026-07-29), stable across
 * power cycles, unambiguous once the decimal readout gained a rapid
 * "attention burst" prefix -- a 0 digit is encoded as one LONG flash, so
 * two flash-groups means exactly two digits: 1 and 9.
 *
 * 19 is MACH_TYPE_L7200 upstream (a LinkUp L7200 SDP -- utterly unrelated
 * hardware), so this is almost certainly NOT a deliberate machine ID at
 * all: Sharp's own kernel ships exactly one machine_desc, so it has no
 * reason to validate r1, and the bootloader evidently never sets it
 * meaningfully. We match it anyway because it is stable, and because the
 * alternative -- no match -- means dump_machine_table()'s silent
 * `while (true);`, which is indistinguishable from a failed flash and
 * cost an entire session to diagnose.
 *
 * Deliberately NOT spelled MACH_TYPE_L7200: that name would imply this is
 * an L7200, which it emphatically is not.
 */
#define MACH_TYPE_SHARP_BOOTLOADER	19

MACHINE_START(SHARP_BOOTLOADER, "SHARP Zaurus (bootloader nr 19)")
	.fixup		= fixup_corgi,
	.map_io		= pxa25x_map_io,
	.nr_irqs	= PXA_NR_IRQS,
	.init_irq	= pxa25x_init_irq,
	.init_machine	= corgi_init,
	.init_time	= pxa_timer_init,
	.restart	= corgi_restart,
MACHINE_END

MACHINE_START(SHARP_LEGACY, "SHARP Shepherd (Sharp legacy nr 196)")
	.fixup		= fixup_corgi,
	.map_io		= pxa25x_map_io,
	.nr_irqs	= PXA_NR_IRQS,
	.init_irq	= pxa25x_init_irq,
	.init_machine	= corgi_init,
	.init_time	= pxa_timer_init,
	.restart	= corgi_restart,
MACHINE_END

