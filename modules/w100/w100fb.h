/* SPDX-License-Identifier: GPL-2.0-only */
/*
 *  Support for the w100 frame buffer.
 *
 *  Copyright (c) 2004-2005 Richard Purdie
 *  Copyright (c) 2005 Ian Molton
 */

#define W100_GPIO_PORT_A	0
#define W100_GPIO_PORT_B	1

#define CLK_SRC_XTAL  0
#define CLK_SRC_PLL   1

struct w100fb_par;

unsigned long w100fb_gpio_read(int port);
void w100fb_gpio_write(int port, unsigned long value);
unsigned long w100fb_get_hsynclen(struct device *dev);

/* LCD Specific Routines and Config */
struct w100_tg_info {
	void (*change)(struct w100fb_par*);
	void (*suspend)(struct w100fb_par*);
	void (*resume)(struct w100fb_par*);
};

/* General Platform Specific w100 Register Values */
struct w100_gen_regs {
	unsigned long lcd_format;
	unsigned long lcdd_cntl1;
	unsigned long lcdd_cntl2;
	unsigned long genlcd_cntl1;
	unsigned long genlcd_cntl2;
	unsigned long genlcd_cntl3;
};

struct w100_gpio_regs {
	unsigned long init_data1;
	unsigned long init_data2;
	unsigned long gpio_dir1;
	unsigned long gpio_oe1;
	unsigned long gpio_dir2;
	unsigned long gpio_oe2;
};

/* Optional External Memory Configuration */
struct w100_mem_info {
	unsigned long ext_cntl;
	unsigned long sdram_mode_reg;
	unsigned long ext_timing_cntl;
	unsigned long io_cntl;
	unsigned int size;
};

struct w100_bm_mem_info {
	unsigned long ext_mem_bw;
	unsigned long offset;
	unsigned long ext_timing_ctl;
	unsigned long ext_cntl;
	unsigned long mode_reg;
	unsigned long io_cntl;
	unsigned long config;
};

/* LCD Mode definition */
struct w100_mode {
	unsigned int xres;
	unsigned int yres;
	unsigned short left_margin;
	unsigned short right_margin;
	unsigned short upper_margin;
	unsigned short lower_margin;
	unsigned long crtc_ss;
	unsigned long crtc_ls;
	unsigned long crtc_gs;
	unsigned long crtc_vpos_gs;
	unsigned long crtc_rev;
	unsigned long crtc_dclk;
	unsigned long crtc_gclk;
	unsigned long crtc_goe;
	unsigned long crtc_ps1_active;
	char pll_freq;
	char fast_pll_freq;
	int sysclk_src;
	int sysclk_divider;
	int pixclk_src;
	int pixclk_divider;
	int pixclk_divider_rotated;
};

struct w100_pll_info {
	uint16_t freq;  /* desired Fout for PLL (Mhz) */
	uint8_t M;      /* input divider */
	uint8_t N_int;  /* VCO multiplier */
	uint8_t N_fac;  /* VCO multiplier fractional part */
	uint8_t tfgoal;
	uint8_t lock_time;
};

/* Initial Video mode orientation flags */
#define INIT_MODE_ROTATED  0x1
#define INIT_MODE_FLIPPED  0x2

/*
 * This structure describes the machine which we are running on.
 * It is set by machine specific code and used in the probe routine
 * of drivers/video/w100fb.c
 */
struct w100fb_mach_info {
	/* General Platform Specific Registers */
	struct w100_gen_regs *regs;
	/* Table of modes the LCD is capable of */
	struct w100_mode *modelist;
	unsigned int num_modes;
	/* Hooks for any platform specific tg/lcd code (optional) */
	struct w100_tg_info *tg;
	/* External memory definition (if present) */
	struct w100_mem_info *mem;
	/* Additional External memory definition (if present) */
	struct w100_bm_mem_info *bm_mem;
	/* GPIO definitions (optional) */
	struct w100_gpio_regs *gpio;
	/* Initial Mode flags */
	unsigned int init_mode;
	/* Xtal Frequency */
	unsigned int xtal_freq;
	/* Enable Xtal input doubler (1 == enable) */
	unsigned int xtal_dbl;
};

/* General frame buffer data structure */
struct w100fb_par {
	unsigned int chip_id;
	unsigned int xres;
	unsigned int yres;
	unsigned int extmem_active;
	unsigned int flip;
	unsigned int blanked;
	unsigned int fastpll_mode;
	/* Last frequency w100_set_pll_freq() actually locked, in Hz. 0 if the
	 * PLL has never been locked (e.g. before the first mode set). Purely
	 * instrumentation -- see the "clocks" sysfs attribute -- and does not
	 * feed back into any register programming. */
	unsigned int pll_freq_hz;
	/* Last PLL frequency (MHz) that actually locked -- w100_set_pll_freq()
	 * falls back to this on a failed w100_pll_set_clk() rather than
	 * leaving the chip on whatever half-applied state calibration
	 * failure left behind (w100_pll_set_clk() parks SCLK on XTAL and
	 * writes the new divider before it knows calibration will succeed).
	 * 0 until the first successful lock. */
	unsigned int pll_freq_last_good;
	/* Runtime PLL override (MHz), set via the "sysclk" sysfs attribute.
	 * When nonzero, w100_init_clocks() uses this instead of the current
	 * mode's own pll_freq/fast_pll_freq, so a mode change (e.g. a
	 * rotation) does not silently revert an operator-requested clock. */
	unsigned int pll_override;
	/* Runtime pixel-clock override, in Hz, set via the "pixclk" sysfs
	 * attribute. 0 = no override (use the current mode's own
	 * pixclk_divider/pixclk_divider_rotated). Stored as a target
	 * frequency, not a raw divider, because rotated and non-rotated
	 * orientations want different dividers off the same source -- see
	 * w100_current_pixclk_divider(), which re-solves this against
	 * whichever orientation is live every time w100_set_dispregs() runs.
	 * Always clamped to W100_PCLK_MAX_HZ in the kernel regardless of what
	 * was written here -- this is the one value in the whole clock-domain
	 * bring-up plan that can run the panel out of spec. */
	unsigned int pixclk_override_hz;
	/* Runtime override for w100_mem_info.sdram_mode_reg, set via the
	 * "sdram_mode_reg" sysfs attribute (0 = unset, use the compiled-in
	 * mach->mem->sdram_mode_reg). CAS-latency bisection tool -- see
	 * w100_setup_memory()'s comment on why this can ONLY safely apply on
	 * a genuine external-memory off->on transition, never as a live
	 * register poke: the write sequence it feeds is the SDRAM's
	 * precharge/MRS init, which does not preserve existing content and
	 * assumes nothing is actively scanning that memory out yet. Setting
	 * this does NOT itself reprogram anything -- it only changes what the
	 * NEXT off->on transition (e.g. bouncing through QVGA and back) will
	 * write. */
	unsigned int sdram_mode_reg_override;
	unsigned long hsync_len;
	struct w100_mode *mode;
	struct w100_pll_info *pll_table;
	struct w100fb_mach_info *mach;
	uint32_t *saved_intmem;
	uint32_t *saved_extmem;
};
