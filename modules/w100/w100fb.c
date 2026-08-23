
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/preempt.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <asm/io.h>
#include <linux/uaccess.h>
#include <video/w100fb.h>
#include <video/w100fb_accel.h>

#include "w100fb.h"

static void w100_suspend(u32 mode);
static int w100_vsync(bool tight);
static void w100_vsync_pause(bool tight);
static void w100_hw_init(struct w100fb_par*);

static bool assume_initialised;
module_param(assume_initialised, bool, 0444);
MODULE_PARM_DESC(assume_initialised,
	"controller is already running from an earlier kernel: adopt its state instead of resetting and re-initialising it");
static void w100_soft_reset(void);
static void w100_pwm_setup(struct w100fb_par*);
static void w100_init_clocks(struct w100fb_par*);
static void w100_setup_memory(struct w100fb_par*);
static void w100_init_lcd(struct w100fb_par*);
static void w100_set_dispregs(struct w100fb_par*);
static void w100_update_enable(void);
static void w100_update_disable(void);
static void calc_hsync(struct w100fb_par *par);
static void w100_init_graphic_engine(struct w100fb_par *par);
struct w100_pll_info *w100_get_xtal_table(unsigned int freq);
static unsigned int w100_get_testcount(unsigned int testclk_sel);
static int w100_set_pll_freq(struct w100fb_par *par, unsigned int freq);

static int w100_vsync_mode;
module_param_named(vsync_mode, w100_vsync_mode, int, 0644);
MODULE_PARM_DESC(vsync_mode,
	"w100 vsync wait mode (0=vline irq status, default; 1=CRTC frame counter, non-functional on Corgi)");

static bool w100_vsync_debug;
module_param_named(vsync_debug, w100_vsync_debug, bool, 0644);
MODULE_PARM_DESC(vsync_debug, "enable extra vsync timeout diagnostics");

static bool w100_pan_hw_latch = true;
module_param_named(pan_hw_latch, w100_pan_hw_latch, bool, 0644);
MODULE_PARM_DESC(pan_hw_latch,
	"flip via the CRTC vblank latch (default 1); 0 = software vsync wait then direct write");

static bool w100_pan_verify;
module_param_named(pan_verify, w100_pan_verify, bool, 0644);
MODULE_PARM_DESC(pan_verify,
	"bring-up diagnostic: confirm the vblank latch actually completes (costs one frame per pan)");

static bool w100_force_fullrate;
MODULE_PARM_DESC(force_fullrate, "force full-rate panel fetch (may increase power)");

static struct w100fb_par *w100_primary_par;

static int w100_set_force_fullrate(const char *val, const struct kernel_param *kp)
{
	bool old = w100_force_fullrate;
	int ret;

	ret = param_set_bool(val, kp);
	if (ret)
		return ret;

	if (old != w100_force_fullrate && w100_primary_par)
		w100_set_dispregs(w100_primary_par);

	return 0;
}

static const struct kernel_param_ops w100_force_fullrate_ops = {
	.set = w100_set_force_fullrate,
	.get = param_get_bool,
};

module_param_cb(force_fullrate, &w100_force_fullrate_ops,
		&w100_force_fullrate, 0644);

#define W100_VSYNC_TIMEOUT_MS	100

#define W100_VBLANK_US		182

#define W100_PLL_MAX_MHZ	125
#define W100_PLL_MIN_MHZ	50

#define W100_PCLK_MAX_HZ	25000000

#define MAX_PALETTES      16

#define W100_SUSPEND_EXTMEM 0
#define W100_SUSPEND_ALL    1

#define BITS_PER_PIXEL    16

static void __iomem *remapped_base;
static void __iomem *remapped_regs;
static void __iomem *remapped_fbuf;

#define REMAPPED_FB_LEN   0x15ffff

#define W100_FB_BASE MEM_EXT_BASE_VALUE

static ssize_t flip_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par=info->par;

	return sprintf(buf, "%d\n",par->flip);
}

static ssize_t flip_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned int flip;
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par=info->par;

	flip = simple_strtoul(buf, NULL, 10);

	if (flip > 0)
		par->flip = 1;
	else
		par->flip = 0;

	w100_update_disable();
	w100_set_dispregs(par);
	w100_update_enable();

	calc_hsync(par);

	return count;
}

static DEVICE_ATTR_RW(flip);

static ssize_t w100fb_reg_read(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long regs, param;
	regs = simple_strtoul(buf, NULL, 16);
	param = readl(remapped_regs + regs);
	printk("Read Register 0x%08lX: 0x%08lX\n", regs, param);
	return count;
}

static DEVICE_ATTR(reg_read, 0200, NULL, w100fb_reg_read);

static ssize_t w100fb_reg_write(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long regs, param;
	sscanf(buf, "%lx %lx", &regs, &param);

	if (regs <= 0x2000) {
		printk("Write Register 0x%08lX: 0x%08lX\n", regs, param);
		writel(param, remapped_regs + regs);
	}

	return count;
}

static DEVICE_ATTR(reg_write, 0200, NULL, w100fb_reg_write);

static ssize_t fastpllclk_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par=info->par;

	return sprintf(buf, "%d\n",par->fastpll_mode);
}

static ssize_t fastpllclk_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par=info->par;

	if (simple_strtoul(buf, NULL, 10) > 0) {
		par->fastpll_mode=1;
		printk("w100fb: Using fast system clock (if possible)\n");
	} else {
		par->fastpll_mode=0;
		printk("w100fb: Using normal system clock\n");
	}

	w100_init_clocks(par);
	calc_hsync(par);

	return count;
}

static DEVICE_ATTR_RW(fastpllclk);

static unsigned int w100_pclk_divider_for(unsigned int src_hz, unsigned int target_hz)
{
	unsigned int ratio, div;

	if (!src_hz || !target_hz)
		return 15;

	ratio = (src_hz + target_hz - 1) / target_hz;
	if (ratio < 1)
		ratio = 1;
	div = ratio - 1;
	if (div > 15)
		div = 15;
	return div;
}

static unsigned int w100_current_pixclk_divider(struct w100fb_par *par,
						 struct w100_mode *mode, bool rotated)
{
	unsigned int table_div = rotated ? mode->pixclk_divider_rotated : mode->pixclk_divider;
	unsigned int src_hz, target_hz;

	if (!par->pixclk_override_hz)
		return table_div;

	src_hz = (mode->pixclk_src == CLK_SRC_PLL) ? par->pll_freq_hz : par->mach->xtal_freq;
	target_hz = par->pixclk_override_hz;
	if (target_hz > W100_PCLK_MAX_HZ)
		target_hz = W100_PCLK_MAX_HZ;

	return w100_pclk_divider_for(src_hz, target_hz);
}

static ssize_t clocks_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par = info->par;
	struct w100_mode *mode = par->mode;
	bool rotated;
	unsigned int divider, pclk_src_hz, sclk_src_hz;
	unsigned int pll_hz, sclk_hz, pclk_hz;

	if (!mode)
		return scnprintf(buf, PAGE_SIZE, "no mode set yet\n");

	rotated = (par->xres != mode->xres);
	divider = w100_current_pixclk_divider(par, mode, rotated);

	pll_hz = par->pll_freq_hz;

	sclk_src_hz = (mode->sysclk_src == CLK_SRC_PLL) ? pll_hz : par->mach->xtal_freq;
	sclk_hz = sclk_src_hz / (mode->sysclk_divider + 1);

	pclk_src_hz = (mode->pixclk_src == CLK_SRC_PLL) ? pll_hz : par->mach->xtal_freq;
	pclk_hz = pclk_src_hz / (divider + 1);

	return scnprintf(buf, PAGE_SIZE,
		"xtal  %u\n"
		"pll   %u\n"
		"sclk  %u\n"
		"pclk  %u\n"
		"mode  %ux%u rotated=%d div=%u\n"
		"pixclk_override_hz %u (0 = none; clamped to %u)\n"
		"testcount_raw xtal=%u pll=%u sclk=%u pclk=%u (uncalibrated CLK_TEST_CNTL counts, not Hz)\n",
		par->mach->xtal_freq, pll_hz, sclk_hz, pclk_hz,
		par->xres, par->yres, rotated, divider,
		par->pixclk_override_hz, W100_PCLK_MAX_HZ,
		w100_get_testcount(TESTCLK_SRC_XTAL),
		w100_get_testcount(TESTCLK_SRC_PLL),
		w100_get_testcount(TESTCLK_SRC_SCLK),
		w100_get_testcount(TESTCLK_SRC_PCLK));
}

static DEVICE_ATTR_RO(clocks);

static ssize_t sysclk_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par = info->par;

	return scnprintf(buf, PAGE_SIZE, "%u\n", par->pll_freq_hz / 1000000);
}

static ssize_t sysclk_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par = info->par;
	unsigned long mhz;

	mhz = simple_strtoul(buf, NULL, 10);
	if (mhz < W100_PLL_MIN_MHZ || mhz > W100_PLL_MAX_MHZ)
		return -EINVAL;

	if (!w100_set_pll_freq(par, mhz))
		return -EIO;

	par->pll_override = mhz;
	calc_hsync(par);

	return count;
}

static DEVICE_ATTR_RW(sysclk);

static ssize_t pixclk_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par = info->par;

	return scnprintf(buf, PAGE_SIZE, "%u\n", par->pixclk_override_hz);
}

static ssize_t pixclk_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par = info->par;
	unsigned long hz;

	hz = simple_strtoul(buf, NULL, 10);
	if (hz > W100_PCLK_MAX_HZ)
		return -EINVAL;

	par->pixclk_override_hz = hz;

	w100_update_disable();
	w100_set_dispregs(par);
	w100_update_enable();

	calc_hsync(par);

	return count;
}

static DEVICE_ATTR_RW(pixclk);

static ssize_t sdram_mode_reg_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par = info->par;

	return scnprintf(buf, PAGE_SIZE, "%#x (compiled-in default %#lx)\n",
			  par->sdram_mode_reg_override,
			  par->mach->mem ? par->mach->mem->sdram_mode_reg : 0);
}

static ssize_t sdram_mode_reg_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par = info->par;

	par->sdram_mode_reg_override = simple_strtoul(buf, NULL, 16);

	return count;
}

static DEVICE_ATTR_RW(sdram_mode_reg);

static struct attribute *w100fb_attrs[] = {
	&dev_attr_fastpllclk.attr,
	&dev_attr_clocks.attr,
	&dev_attr_sysclk.attr,
	&dev_attr_pixclk.attr,
	&dev_attr_sdram_mode_reg.attr,
	&dev_attr_reg_read.attr,
	&dev_attr_reg_write.attr,
	&dev_attr_flip.attr,
	NULL,
};
ATTRIBUTE_GROUPS(w100fb);

unsigned long w100fb_get_hsynclen(struct device *dev)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct w100fb_par *par=info->par;

	if (par->blanked)
		return 0;
	else
		return par->hsync_len;
}
EXPORT_SYMBOL(w100fb_get_hsynclen);

static void w100fb_clear_screen(struct w100fb_par *par)
{
	memset_io(remapped_fbuf + (W100_FB_BASE-MEM_WINDOW_BASE), 0, (par->xres * par->yres * BITS_PER_PIXEL/8));
}

static int w100fb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			     u_int trans, struct fb_info *info)
{
	unsigned int val;
	int ret = 1;

	if (info->var.grayscale)
		red = green = blue = (19595 * red + 38470 * green + 7471 * blue) >> 16;

	if (regno < MAX_PALETTES) {
		u32 *pal = info->pseudo_palette;

		val = (red & 0xf800) | ((green & 0xfc00) >> 5) | ((blue & 0xf800) >> 11);
		pal[regno] = val;
		ret = 0;
	}
	return ret;
}

static int w100fb_blank(int blank_mode, struct fb_info *info)
{
	struct w100fb_par *par = info->par;

	switch(blank_mode) {

 	case FB_BLANK_NORMAL:
	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
 	case FB_BLANK_POWERDOWN:
  		if (par->blanked == 0) {
			par->blanked = 1;
  		}
  		break;

 	case FB_BLANK_UNBLANK:
  		if (par->blanked != 0) {
			par->blanked = 0;
  		}
  		break;
 	}
	return 0;
}

static void w100_fifo_wait(int entries)
{
	union rbbm_status_u status;
	int i;

	for (i = 0; i < 2000000; i++) {
		status.val = readl(remapped_regs + mmRBBM_STATUS);
		if (status.f.cmdfifo_avail >= entries)
			return;
		udelay(1);
	}
	printk(KERN_ERR "w100fb: FIFO Timeout!\n");
}

static int w100fb_sync(struct fb_info *info)
{
	union rbbm_status_u status;
	int i;

	for (i = 0; i < 2000000; i++) {
		status.val = readl(remapped_regs + mmRBBM_STATUS);
		if (!status.f.gui_active)
			return 0;
		udelay(1);
	}
	printk(KERN_ERR "w100fb: Graphic engine timeout!\n");
	return -EBUSY;
}

static int w100fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	return vm_iomap_memory(vma, info->fix.smem_start, info->fix.smem_len);
}

static void w100_init_graphic_engine(struct w100fb_par *par)
{
	union dp_gui_master_cntl_u gmc;
	union dp_mix_u dp_mix;
	union dp_datatype_u dp_datatype;
	union dp_cntl_u dp_cntl;

	w100_fifo_wait(4);
	writel(W100_FB_BASE, remapped_regs + mmDST_OFFSET);
	writel(par->xres, remapped_regs + mmDST_PITCH);
	writel(W100_FB_BASE, remapped_regs + mmSRC_OFFSET);
	writel(par->xres, remapped_regs + mmSRC_PITCH);

	w100_fifo_wait(3);
	writel(0, remapped_regs + mmSC_TOP_LEFT);
	writel((par->yres << 16) | par->xres, remapped_regs + mmSC_BOTTOM_RIGHT);
	writel(0x1fff1fff, remapped_regs + mmSRC_SC_BOTTOM_RIGHT);

	w100_fifo_wait(4);
	dp_cntl.val = 0;
	dp_cntl.f.dst_x_dir = 1;
	dp_cntl.f.dst_y_dir = 1;
	dp_cntl.f.src_x_dir = 1;
	dp_cntl.f.src_y_dir = 1;
	dp_cntl.f.dst_major_x = 1;
	dp_cntl.f.src_major_x = 1;
	writel(dp_cntl.val, remapped_regs + mmDP_CNTL);

	gmc.val = 0;
	gmc.f.gmc_src_pitch_offset_cntl = 1;
	gmc.f.gmc_dst_pitch_offset_cntl = 1;
	gmc.f.gmc_src_clipping = 1;
	gmc.f.gmc_dst_clipping = 1;
	gmc.f.gmc_brush_datatype = GMC_BRUSH_NONE;
	gmc.f.gmc_dst_datatype = 3;
	gmc.f.gmc_src_datatype = SRC_DATATYPE_EQU_DST;
	gmc.f.gmc_byte_pix_order = 1;
	gmc.f.gmc_default_sel = 0;
	gmc.f.gmc_rop3 = ROP3_SRCCOPY;
	gmc.f.gmc_dp_src_source = DP_SRC_MEM_RECTANGULAR;
	gmc.f.gmc_clr_cmp_fcn_dis = 1;
	gmc.f.gmc_wr_msk_dis = 1;
	gmc.f.gmc_dp_op = DP_OP_ROP;
	writel(gmc.val, remapped_regs + mmDP_GUI_MASTER_CNTL);

	dp_datatype.val = dp_mix.val = 0;
	dp_datatype.f.dp_dst_datatype = gmc.f.gmc_dst_datatype;
	dp_datatype.f.dp_brush_datatype = gmc.f.gmc_brush_datatype;
	dp_datatype.f.dp_src2_type = 0;
	dp_datatype.f.dp_src2_datatype = gmc.f.gmc_src_datatype;
	dp_datatype.f.dp_src_datatype = gmc.f.gmc_src_datatype;
	dp_datatype.f.dp_byte_pix_order = gmc.f.gmc_byte_pix_order;
	writel(dp_datatype.val, remapped_regs + mmDP_DATATYPE);

	dp_mix.f.dp_src_source = gmc.f.gmc_dp_src_source;
	dp_mix.f.dp_src2_source = 1;
	dp_mix.f.dp_rop3 = gmc.f.gmc_rop3;
	dp_mix.f.dp_op = gmc.f.gmc_dp_op;
	writel(dp_mix.val, remapped_regs + mmDP_MIX);
}

static void w100_accel_fillrect(unsigned long dst_offset, u32 dst_pitch_px,
				 u32 dx, u32 dy, u32 width, u32 height,
				 u32 color)
{
	union dp_gui_master_cntl_u gmc;

	w100_fifo_wait(2);
	writel(dst_offset, remapped_regs + mmDST_OFFSET);
	writel(dst_pitch_px, remapped_regs + mmDST_PITCH);

	gmc.val = readl(remapped_regs + mmDP_GUI_MASTER_CNTL);
	gmc.f.gmc_rop3 = ROP3_PATCOPY;
	gmc.f.gmc_brush_datatype = GMC_BRUSH_SOLID_COLOR;
	w100_fifo_wait(2);
	writel(gmc.val, remapped_regs + mmDP_GUI_MASTER_CNTL);
	writel(color, remapped_regs + mmDP_BRUSH_FRGD_CLR);

	w100_fifo_wait(2);
	writel((dy << 16) | (dx & 0xffff), remapped_regs + mmDST_Y_X);
	writel((width << 16) | (height & 0xffff), remapped_regs + mmDST_WIDTH_HEIGHT);
}

static void w100_accel_copyarea(unsigned long src_offset, u32 src_pitch_px,
				 unsigned long dst_offset, u32 dst_pitch_px,
				 u32 sx, u32 sy, u32 dx, u32 dy,
				 u32 width, u32 height)
{
	union dp_gui_master_cntl_u gmc;

	w100_fifo_wait(4);
	writel(src_offset, remapped_regs + mmSRC_OFFSET);
	writel(src_pitch_px, remapped_regs + mmSRC_PITCH);
	writel(dst_offset, remapped_regs + mmDST_OFFSET);
	writel(dst_pitch_px, remapped_regs + mmDST_PITCH);

	gmc.val = readl(remapped_regs + mmDP_GUI_MASTER_CNTL);
	gmc.f.gmc_rop3 = ROP3_SRCCOPY;
	gmc.f.gmc_brush_datatype = GMC_BRUSH_NONE;
	w100_fifo_wait(1);
	writel(gmc.val, remapped_regs + mmDP_GUI_MASTER_CNTL);

	w100_fifo_wait(3);
	writel((sy << 16) | (sx & 0xffff), remapped_regs + mmSRC_Y_X);
	writel((dy << 16) | (dx & 0xffff), remapped_regs + mmDST_Y_X);
	writel((width << 16) | (height & 0xffff), remapped_regs + mmDST_WIDTH_HEIGHT);
}

static void w100fb_fillrect(struct fb_info *info,
                            const struct fb_fillrect *rect)
{
	struct w100fb_par *par = info->par;

	if (info->state != FBINFO_STATE_RUNNING)
		return;
	if (info->flags & FBINFO_HWACCEL_DISABLED) {
		cfb_fillrect(info, rect);
		return;
	}

	w100_accel_fillrect(W100_FB_BASE, par->xres,
			     rect->dx, rect->dy, rect->width, rect->height,
			     rect->color);
}

static void w100fb_copyarea(struct fb_info *info,
                            const struct fb_copyarea *area)
{
	struct w100fb_par *par = info->par;

	if (info->state != FBINFO_STATE_RUNNING)
		return;
	if (info->flags & FBINFO_HWACCEL_DISABLED) {
		cfb_copyarea(info, area);
		return;
	}

	w100_accel_copyarea(W100_FB_BASE, par->xres,
			     W100_FB_BASE, par->xres,
			     area->sx, area->sy, area->dx, area->dy,
			     area->width, area->height);
}

static void w100fb_activate_var(struct w100fb_par *par)
{
	struct w100_tg_info *tg = par->mach->tg;

	if (par->adopted) {
		union graphic_h_disp_u live_h;
		union graphic_v_disp_u live_v;
		unsigned int live_xres, live_yres;

		par->adopted = 0;

		live_h.val = readl(remapped_regs + mmGRAPHIC_H_DISP);
		live_v.val = readl(remapped_regs + mmGRAPHIC_V_DISP);
		live_xres = live_h.f.graphic_h_end - live_h.f.graphic_h_start;
		live_yres = live_v.f.graphic_v_end - live_v.f.graphic_v_start;

		if (live_xres == par->mode->xres && live_yres == par->mode->yres)
			return;

		printk(KERN_INFO "w100fb: adopted controller is %ux%u but %ux%u was requested, programming it\n",
		       live_xres, live_yres, par->mode->xres, par->mode->yres);
	}

	w100_pwm_setup(par);
	w100_setup_memory(par);
	w100_init_clocks(par);
	w100fb_clear_screen(par);
	w100_vsync(false);

	w100_update_disable();
	w100_init_lcd(par);
	w100_set_dispregs(par);
	w100_update_enable();
	w100_init_graphic_engine(par);

	calc_hsync(par);

	if (!par->blanked && tg && tg->change)
		tg->change(par);
}

static struct w100_mode *w100fb_get_mode(struct w100fb_par *par, unsigned int *x, unsigned int *y, int saveval)
{
	struct w100_mode *mode = NULL;
	struct w100_mode *modelist = par->mach->modelist;
	unsigned int best_x = 0xffffffff, best_y = 0xffffffff;
	unsigned int i;

	for (i = 0 ; i < par->mach->num_modes ; i++) {
		if (modelist[i].xres >= *x && modelist[i].yres >= *y &&
				modelist[i].xres < best_x && modelist[i].yres < best_y) {
			best_x = modelist[i].xres;
			best_y = modelist[i].yres;
			mode = &modelist[i];
		} else if(modelist[i].xres >= *y && modelist[i].yres >= *x &&
		        modelist[i].xres < best_y && modelist[i].yres < best_x) {
			best_x = modelist[i].yres;
			best_y = modelist[i].xres;
			mode = &modelist[i];
		}
	}

	if (mode && saveval) {
		*x = best_x;
		*y = best_y;
	}

	return mode;
}

static int w100fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct w100fb_par *par=info->par;
	unsigned long available;
	unsigned long virtual_size;

	if(!w100fb_get_mode(par, &var->xres, &var->yres, 1))
		return -EINVAL;

	if (par->mach->mem && ((var->xres*var->yres*BITS_PER_PIXEL/8) > (par->mach->mem->size+1)))
		return -EINVAL;

	if (!par->mach->mem && ((var->xres*var->yres*BITS_PER_PIXEL/8) > (MEM_INT_SIZE+1)))
		return -EINVAL;

	var->xres_virtual = max(var->xres_virtual, var->xres);
	var->yres_virtual = max(var->yres_virtual, var->yres);
	if (var->xres_virtual != var->xres)
		return -EINVAL;

	available = par->mach->mem ? par->mach->mem->size + 1 : MEM_INT_SIZE + 1;
	available = min_t(unsigned long, available, REMAPPED_FB_LEN + 1);
	virtual_size = (unsigned long)var->xres_virtual * var->yres_virtual *
		BITS_PER_PIXEL / 8;
	if (virtual_size > available || var->xoffset != 0 ||
	    var->yoffset + var->yres > var->yres_virtual)
		return -EINVAL;

	if (var->bits_per_pixel > BITS_PER_PIXEL)
		return -EINVAL;
	else
		var->bits_per_pixel = BITS_PER_PIXEL;

	var->red.offset = 11;
	var->red.length = 5;
	var->green.offset = 5;
	var->green.length = 6;
	var->blue.offset = 0;
	var->blue.length = 5;
	var->transp.offset = var->transp.length = 0;

	var->nonstd = 0;
	var->height = -1;
	var->width = -1;
	var->vmode = FB_VMODE_NONINTERLACED;
	var->sync = 0;
	var->pixclock = 0x04;

	return 0;
}

static int w100fb_set_par(struct fb_info *info)
{
	struct w100fb_par *par=info->par;
	unsigned long needed;
	int want_extmem;

	needed = max_t(unsigned long,
		       (unsigned long)info->var.xres * info->var.yres,
		       (unsigned long)info->var.xres_virtual * info->var.yres_virtual)
		 * BITS_PER_PIXEL / 8;
	want_extmem = par->mach->mem && needed > MEM_INT_SIZE + 1;

	if (par->xres != info->var.xres || par->yres != info->var.yres ||
	    want_extmem != par->extmem_active) {
		par->xres = info->var.xres;
		par->yres = info->var.yres;
		par->mode = w100fb_get_mode(par, &par->xres, &par->yres, 0);

		info->fix.visual = FB_VISUAL_TRUECOLOR;
		info->fix.ypanstep = 1;
		info->fix.ywrapstep = 0;
		info->fix.line_length = par->xres * BITS_PER_PIXEL / 8;

		mutex_lock(&info->mm_lock);
		if (want_extmem) {
			par->extmem_active = 1;
			info->fix.smem_len = par->mach->mem->size+1;
		} else {
			par->extmem_active = 0;
			info->fix.smem_len = MEM_INT_SIZE+1;
		}
		mutex_unlock(&info->mm_lock);

		w100fb_activate_var(par);
	}
	return 0;
}

static unsigned long w100fb_scanout_offset(struct w100fb_par *par,
					   unsigned int yoffset)
{
	unsigned long offset;

	if (par->xres == par->mode->xres)
		offset = par->flip ? (par->xres * par->yres) - 1 : 0;
	else
		offset = par->flip ? par->xres - 1 :
			 par->xres * (par->yres - 1);

	return offset + (unsigned long)yoffset * par->xres;
}

static void w100_pan_flip(unsigned long addr)
{
	union disp_db_buf_cntl_rd_u rd;
	ktime_t deadline;

	if (w100_pan_hw_latch) {
		rd.val = readl(remapped_regs + mmDISP_DB_BUF_CNTL);
		if (!rd.f.en_db_buf) {
			pr_warn_ratelimited("w100fb: pan: en_db_buf is clear, falling back to the timed write\n");
			goto timed_write;
		}

		writel(addr, remapped_regs + mmGRAPHIC_OFFSET);
		w100_update_enable();

		if (!w100_pan_verify)
			return;

		deadline = ktime_add_ms(ktime_get(), W100_VSYNC_TIMEOUT_MS);
		while (ktime_before(ktime_get(), deadline)) {
			rd.val = readl(remapped_regs + mmDISP_DB_BUF_CNTL);
			if (rd.f.update_db_buf_done) {
				pr_info_ratelimited("w100fb: pan: vblank latch completed -- pan_hw_latch works on this board\n");
				return;
			}
			w100_vsync_pause(false);
		}

		pr_warn_ratelimited("w100fb: pan: vblank latch never completed (db_buf_cntl=%#x) -- mmGRAPHIC_OFFSET may not be double-buffered here; try pan_hw_latch=0\n",
				    readl(remapped_regs + mmDISP_DB_BUF_CNTL));
		return;
	}

timed_write:
	w100_vsync(true);
	writel(addr, remapped_regs + mmGRAPHIC_OFFSET);
}

static int w100fb_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info)
{
	struct w100fb_par *par = info->par;
	unsigned long offset;

	if (var->xoffset != 0 || var->yoffset + info->var.yres >
				 info->var.yres_virtual)
		return -EINVAL;

	offset = w100fb_scanout_offset(par, var->yoffset);
	w100_pan_flip(W100_FB_BASE + ((offset * BITS_PER_PIXEL / 8) & ~0x03UL));

	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;

	return 0;
}

static bool w100fb_coord_ok(u32 x, u32 y, u32 width, u32 height)
{
	return x < 65536 && y < 65536 && width < 65536 && height < 65536;
}

static bool w100fb_rect_fits(struct w100fb_par *par, struct fb_info *info,
			      u32 offset, u32 pitch, u32 x, u32 y,
			      u32 width, u32 height)
{
	u64 last_byte;

	if (!pitch || !width || !height)
		return false;
	if (x + width > par->xres || y + height > par->yres)
		return false;
	if (pitch % (BITS_PER_PIXEL / 8))
		return false;

	last_byte = (u64)offset + (u64)(y + height - 1) * pitch +
		    (u64)(x + width) * (BITS_PER_PIXEL / 8);
	return last_byte <= info->fix.smem_len;
}

static int w100fb_ioctl(struct fb_info *info, unsigned int cmd,
			unsigned long arg)
{
	struct w100fb_par *par = info->par;
	void __user *argp = (void __user *)arg;

	if (cmd == FBIO_WAITFORVSYNC) {
		return w100_vsync(false);
	}

	if (cmd == W100FB_IOC_SYNC)
		return w100fb_sync(info);

	if (cmd == W100FB_IOC_FILL) {
		struct w100fb_fill_args a;

		if (info->state != FBINFO_STATE_RUNNING)
			return -EBUSY;
		if (info->flags & FBINFO_HWACCEL_DISABLED)
			return -ENOTTY;
		if (copy_from_user(&a, argp, sizeof(a)))
			return -EFAULT;
		if (!w100fb_coord_ok(a.x, a.y, a.width, a.height))
			return -EINVAL;
		if (!w100fb_rect_fits(par, info, a.dst_offset, a.dst_pitch,
				      a.x, a.y, a.width, a.height))
			return -EINVAL;

		w100_accel_fillrect(W100_FB_BASE + a.dst_offset,
				     a.dst_pitch / (BITS_PER_PIXEL / 8),
				     a.x, a.y, a.width, a.height, a.color);
		return 0;
	}

	if (cmd == W100FB_IOC_BLIT) {
		struct w100fb_blit_args a;

		if (info->state != FBINFO_STATE_RUNNING)
			return -EBUSY;
		if (info->flags & FBINFO_HWACCEL_DISABLED)
			return -ENOTTY;
		if (copy_from_user(&a, argp, sizeof(a)))
			return -EFAULT;
		if (!w100fb_coord_ok(a.sx, a.sy, a.width, a.height) ||
		    !w100fb_coord_ok(a.dx, a.dy, a.width, a.height))
			return -EINVAL;
		if (!w100fb_rect_fits(par, info, a.src_offset, a.src_pitch,
				      a.sx, a.sy, a.width, a.height) ||
		    !w100fb_rect_fits(par, info, a.dst_offset, a.dst_pitch,
				      a.dx, a.dy, a.width, a.height))
			return -EINVAL;

		w100_accel_copyarea(W100_FB_BASE + a.src_offset,
				     a.src_pitch / (BITS_PER_PIXEL / 8),
				     W100_FB_BASE + a.dst_offset,
				     a.dst_pitch / (BITS_PER_PIXEL / 8),
				     a.sx, a.sy, a.dx, a.dy, a.width, a.height);
		return 0;
	}

	return -EINVAL;
}

static const struct fb_ops w100fb_ops = {
	.owner        = THIS_MODULE,
	.fb_check_var = w100fb_check_var,
	.fb_set_par   = w100fb_set_par,
	.fb_setcolreg = w100fb_setcolreg,
	.fb_blank     = w100fb_blank,
	.fb_pan_display = w100fb_pan_display,
	.fb_fillrect  = w100fb_fillrect,
	.fb_copyarea  = w100fb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_ioctl     = w100fb_ioctl,
	.fb_sync      = w100fb_sync,
	.fb_mmap      = w100fb_mmap,
};

#ifdef CONFIG_PM
static void w100fb_save_vidmem(struct w100fb_par *par)
{
	int memsize;

	if (par->extmem_active) {
		memsize=par->mach->mem->size;
		par->saved_extmem = vmalloc(memsize);
		if (par->saved_extmem)
			memcpy_fromio(par->saved_extmem, remapped_fbuf + (W100_FB_BASE-MEM_WINDOW_BASE), memsize);
	}
	memsize=MEM_INT_SIZE;
	par->saved_intmem = vmalloc(memsize);
	if (par->saved_intmem && par->extmem_active)
		memcpy_fromio(par->saved_intmem, remapped_fbuf + (W100_FB_BASE-MEM_INT_BASE_VALUE), memsize);
	else if (par->saved_intmem)
		memcpy_fromio(par->saved_intmem, remapped_fbuf + (W100_FB_BASE-MEM_WINDOW_BASE), memsize);
}

static void w100fb_restore_vidmem(struct w100fb_par *par)
{
	int memsize;

	if (par->extmem_active && par->saved_extmem) {
		memsize=par->mach->mem->size;
		memcpy_toio(remapped_fbuf + (W100_FB_BASE-MEM_WINDOW_BASE), par->saved_extmem, memsize);
		vfree(par->saved_extmem);
		par->saved_extmem = NULL;
	}
	if (par->saved_intmem) {
		memsize=MEM_INT_SIZE;
		if (par->extmem_active)
			memcpy_toio(remapped_fbuf + (W100_FB_BASE-MEM_INT_BASE_VALUE), par->saved_intmem, memsize);
		else
			memcpy_toio(remapped_fbuf + (W100_FB_BASE-MEM_WINDOW_BASE), par->saved_intmem, memsize);
		vfree(par->saved_intmem);
		par->saved_intmem = NULL;
	}
}

static int w100fb_suspend(struct platform_device *dev, pm_message_t state)
{
	struct fb_info *info = platform_get_drvdata(dev);
	struct w100fb_par *par=info->par;
	struct w100_tg_info *tg = par->mach->tg;

	w100fb_save_vidmem(par);
	if(tg && tg->suspend)
		tg->suspend(par);
	w100_suspend(W100_SUSPEND_ALL);
	par->blanked = 1;

	return 0;
}

static int w100fb_resume(struct platform_device *dev)
{
	struct fb_info *info = platform_get_drvdata(dev);
	struct w100fb_par *par=info->par;
	struct w100_tg_info *tg = par->mach->tg;

	w100_hw_init(par);
	w100fb_activate_var(par);
	w100fb_restore_vidmem(par);
	if(tg && tg->resume)
		tg->resume(par);
	par->blanked = 0;

	return 0;
}
#else
#define w100fb_suspend  NULL
#define w100fb_resume   NULL
#endif

static int w100fb_probe(struct platform_device *pdev)
{
	int err = -EIO;
	struct w100fb_mach_info *inf;
	struct fb_info *info = NULL;
	struct w100fb_par *par;
	struct resource *mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	unsigned int chip_id;
	bool adopt;

	if (!mem)
		return -EINVAL;

	remapped_base = ioremap(mem->start+W100_CFG_BASE, W100_CFG_LEN);
	if (remapped_base == NULL)
		goto out;

	remapped_regs = ioremap(mem->start+W100_REG_BASE, W100_REG_LEN);
	if (remapped_regs == NULL)
		goto out;

	adopt = assume_initialised;
	if (adopt) {
		chip_id = readl(remapped_regs + mmCHIP_ID);
		if (chip_id != CHIP_ID_W100 && chip_id != CHIP_ID_W3200 &&
		    chip_id != CHIP_ID_W3220) {
			printk(KERN_INFO "w100fb: chip id 0x%08x without a reset, initialising it after all\n",
			       chip_id);
			adopt = false;
		} else {
			printk(KERN_INFO "w100fb: chip id 0x%08x already running, adopting it\n",
			       chip_id);
		}
	}

	if (!adopt) {
		int i;
		for (i = 0; i < 10; i++) {
			w100_soft_reset();
			chip_id = readl(remapped_regs + mmCHIP_ID);
			if (chip_id == CHIP_ID_W100 ||
			    chip_id == CHIP_ID_W3200 ||
			    chip_id == CHIP_ID_W3220)
				break;
			mdelay(10);
		}
		printk(KERN_INFO "w100fb: chip id 0x%08x after %d soft-reset(s)\n",
		       chip_id, i + 1);
	}

	printk("Found ");
	switch(chip_id) {
		case CHIP_ID_W100:  printk("w100");  break;
		case CHIP_ID_W3200: printk("w3200"); break;
		case CHIP_ID_W3220: printk("w3220"); break;
		default:
			printk("Unknown imageon chip ID 0x%08x\n", chip_id);
			err = -ENODEV;
			goto out;
	}
	printk(" at 0x%08lx.\n", (unsigned long) mem->start+W100_CFG_BASE);

	remapped_fbuf = ioremap_wc(mem->start+MEM_WINDOW_BASE, MEM_WINDOW_SIZE);
	if (remapped_fbuf == NULL)
		goto out;

	info=framebuffer_alloc(sizeof(struct w100fb_par), &pdev->dev);
	if (!info) {
		err = -ENOMEM;
		goto out;
	}

	par = info->par;
	platform_set_drvdata(pdev, info);
	w100_primary_par = par;

	inf = dev_get_platdata(&pdev->dev);
	par->chip_id = chip_id;
	par->mach = inf;
	par->fastpll_mode = 0;
	par->blanked = 0;

	par->pll_table=w100_get_xtal_table(inf->xtal_freq);
	if (!par->pll_table) {
		printk(KERN_ERR "No matching Xtal definition found\n");
		err = -EINVAL;
		goto out;
	}

	info->pseudo_palette = kmalloc_array(MAX_PALETTES, sizeof(u32),
					     GFP_KERNEL);
	if (!info->pseudo_palette) {
		err = -ENOMEM;
		goto out;
	}

	info->fbops = &w100fb_ops;
	info->flags = FBINFO_HWACCEL_COPYAREA | FBINFO_HWACCEL_FILLRECT;
	info->node = -1;
	info->screen_base = remapped_fbuf + (W100_FB_BASE-MEM_WINDOW_BASE);
	info->screen_size = REMAPPED_FB_LEN;

	strcpy(info->fix.id, "w100fb");
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.type_aux = 0;
	info->fix.ypanstep = 1;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.smem_start = mem->start+W100_FB_BASE;
	info->fix.mmio_start = mem->start+W100_REG_BASE;
	info->fix.mmio_len = W100_REG_LEN;

	if (fb_alloc_cmap(&info->cmap, 256, 0) < 0) {
		err = -ENOMEM;
		goto out;
	}

	par->mode = &inf->modelist[0];
	if(inf->init_mode & INIT_MODE_ROTATED) {
		info->var.xres = par->mode->yres;
		info->var.yres = par->mode->xres;
	}
	else {
		info->var.xres = par->mode->xres;
		info->var.yres = par->mode->yres;
	}

	if(inf->init_mode &= INIT_MODE_FLIPPED)
		par->flip = 1;
	else
		par->flip = 0;

	info->var.xres_virtual = info->var.xres;
	info->var.yres_virtual = info->var.yres;
	info->var.pixclock = 0x04;
	info->var.sync = 0;
	info->var.grayscale = 0;
	info->var.xoffset = info->var.yoffset = 0;
	info->var.accel_flags = 0;
	info->var.activate = FB_ACTIVATE_NOW;

	par->adopted = adopt ? 1 : 0;
	if (!adopt)
		w100_hw_init(par);

	if (w100fb_check_var(&info->var, info) < 0) {
		err = -EINVAL;
		goto out;
	}

	if (register_framebuffer(info) < 0) {
		err = -EINVAL;
		goto out;
	}

	fb_info(info, "%s frame buffer device\n", info->fix.id);
	return 0;
out:
	if (info) {
		fb_dealloc_cmap(&info->cmap);
		kfree(info->pseudo_palette);
	}
	if (remapped_fbuf != NULL) {
		iounmap(remapped_fbuf);
		remapped_fbuf = NULL;
	}
	if (remapped_regs != NULL) {
		iounmap(remapped_regs);
		remapped_regs = NULL;
	}
	if (remapped_base != NULL) {
		iounmap(remapped_base);
		remapped_base = NULL;
	}
	if (info)
		framebuffer_release(info);
	return err;
}

static void w100fb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);
	struct w100fb_par *par=info->par;

	if (w100_primary_par == par)
		w100_primary_par = NULL;

	unregister_framebuffer(info);

	vfree(par->saved_intmem);
	vfree(par->saved_extmem);
	kfree(info->pseudo_palette);
	fb_dealloc_cmap(&info->cmap);

	iounmap(remapped_base);
	remapped_base = NULL;
	iounmap(remapped_regs);
	remapped_regs = NULL;
	iounmap(remapped_fbuf);
	remapped_fbuf = NULL;

	framebuffer_release(info);
}

static void w100_soft_reset(void)
{
	u16 val = readw((u16 __iomem *)remapped_base + cfgSTATUS);

	writew(val | 0x08, (u16 __iomem *)remapped_base + cfgSTATUS);
	udelay(100);
	writew(0x00, (u16 __iomem *)remapped_base + cfgSTATUS);
	udelay(100);
}

static void w100_update_disable(void)
{
	union disp_db_buf_cntl_wr_u disp_db_buf_wr_cntl;

	disp_db_buf_wr_cntl.f.db_buf_cntl = 0x1e;
	disp_db_buf_wr_cntl.f.update_db_buf = 0;
	disp_db_buf_wr_cntl.f.en_db_buf = 0;
	writel((u32) (disp_db_buf_wr_cntl.val), remapped_regs + mmDISP_DB_BUF_CNTL);
}

static void w100_update_enable(void)
{
	union disp_db_buf_cntl_wr_u disp_db_buf_wr_cntl;

	disp_db_buf_wr_cntl.f.db_buf_cntl = 0x1e;
	disp_db_buf_wr_cntl.f.update_db_buf = 1;
	disp_db_buf_wr_cntl.f.en_db_buf = 1;
	writel((u32) (disp_db_buf_wr_cntl.val), remapped_regs + mmDISP_DB_BUF_CNTL);
}

unsigned long w100fb_gpio_read(int port)
{
	unsigned long value;

	if (port==W100_GPIO_PORT_A)
		value = readl(remapped_regs + mmGPIO_DATA);
	else
		value = readl(remapped_regs + mmGPIO_DATA2);

	return value;
}

void w100fb_gpio_write(int port, unsigned long value)
{
	if (port==W100_GPIO_PORT_A)
		writel(value, remapped_regs + mmGPIO_DATA);
	else
		writel(value, remapped_regs + mmGPIO_DATA2);
}
EXPORT_SYMBOL(w100fb_gpio_read);
EXPORT_SYMBOL(w100fb_gpio_write);

static void w100_hw_init(struct w100fb_par *par)
{
	u32 temp32;
	union cif_cntl_u cif_cntl;
	union intf_cntl_u intf_cntl;
	union cfgreg_base_u cfgreg_base;
	union wrap_top_dir_u wrap_top_dir;
	union cif_read_dbg_u cif_read_dbg;
	union cpu_defaults_u cpu_default;
	union cif_write_dbg_u cif_write_dbg;
	union wrap_start_dir_u wrap_start_dir;
	union cif_io_u cif_io;
	struct w100_gpio_regs *gpio = par->mach->gpio;

	w100_soft_reset();

	writel(0x31, remapped_regs + mmSCRATCH_UMSK);
	for (temp32 = 0; temp32 < 10000; temp32++)
		readl(remapped_regs + mmSCRATCH_UMSK);
	writel(0x30, remapped_regs + mmSCRATCH_UMSK);

	cif_io.val = defCIF_IO;
	writel((u32)(cif_io.val), remapped_regs + mmCIF_IO);

	cif_write_dbg.val = readl(remapped_regs + mmCIF_WRITE_DBG);
	cif_write_dbg.f.dis_packer_ful_during_rbbm_timeout = 0;
	cif_write_dbg.f.en_dword_split_to_rbbm = 1;
	cif_write_dbg.f.dis_timeout_during_rbbm = 1;
	writel((u32) (cif_write_dbg.val), remapped_regs + mmCIF_WRITE_DBG);

	cif_read_dbg.val = readl(remapped_regs + mmCIF_READ_DBG);
	cif_read_dbg.f.dis_rd_same_byte_to_trig_fetch = 1;
	writel((u32) (cif_read_dbg.val), remapped_regs + mmCIF_READ_DBG);

	cif_cntl.val = readl(remapped_regs + mmCIF_CNTL);
	cif_cntl.f.dis_system_bits = 1;
	cif_cntl.f.dis_mr = 1;
	cif_cntl.f.en_wait_to_compensate_dq_prop_dly = 0;
	cif_cntl.f.intb_oe = 1;
	cif_cntl.f.interrupt_active_high = 1;
	writel((u32) (cif_cntl.val), remapped_regs + mmCIF_CNTL);

	intf_cntl.val = defINTF_CNTL;
	intf_cntl.f.ad_inc_a = 1;
	intf_cntl.f.ad_inc_b = 1;
	intf_cntl.f.rd_data_rdy_a = 0;
	intf_cntl.f.rd_data_rdy_b = 0;
	writeb((u8) (intf_cntl.val), remapped_base + cfgINTF_CNTL);

	cpu_default.val = defCPU_DEFAULTS;
	cpu_default.f.access_ind_addr_a = 1;
	cpu_default.f.access_ind_addr_b = 1;
	cpu_default.f.access_scratch_reg = 1;
	cpu_default.f.transition_size = 0;
	writeb((u8) (cpu_default.val), remapped_base + cfgCPU_DEFAULTS);

	writeb((u8) (W100_REG_BASE >> 16), remapped_base + cfgREG_BASE);

	cfgreg_base.val = defCFGREG_BASE;
	cfgreg_base.f.cfgreg_base = W100_CFG_BASE;
	writel((u32) (cfgreg_base.val), remapped_regs + mmCFGREG_BASE);

	wrap_start_dir.val = defWRAP_START_DIR;
	wrap_start_dir.f.start_addr = WRAP_BUF_BASE_VALUE >> 1;
	writel((u32) (wrap_start_dir.val), remapped_regs + mmWRAP_START_DIR);

	wrap_top_dir.val = defWRAP_TOP_DIR;
	wrap_top_dir.f.top_addr = WRAP_BUF_TOP_VALUE >> 1;
	writel((u32) (wrap_top_dir.val), remapped_regs + mmWRAP_TOP_DIR);

	writel((u32) 0x2440, remapped_regs + mmRBBM_CNTL);

	temp32 = readl(remapped_regs + mmDISP_DEBUG2);
	temp32 &= 0xff7fffff;
	temp32 |= 0x00800000;
	writel(temp32, remapped_regs + mmDISP_DEBUG2);

	if (gpio) {
		writel(gpio->init_data1, remapped_regs + mmGPIO_DATA);
		writel(gpio->init_data2, remapped_regs + mmGPIO_DATA2);
		writel(gpio->gpio_dir1,  remapped_regs + mmGPIO_CNTL1);
		writel(gpio->gpio_oe1,   remapped_regs + mmGPIO_CNTL2);
		writel(gpio->gpio_dir2,  remapped_regs + mmGPIO_CNTL3);
		writel(gpio->gpio_oe2,   remapped_regs + mmGPIO_CNTL4);
	}
}

struct power_state {
	union clk_pin_cntl_u clk_pin_cntl;
	union pll_ref_fb_div_u pll_ref_fb_div;
	union pll_cntl_u pll_cntl;
	union sclk_cntl_u sclk_cntl;
	union pclk_cntl_u pclk_cntl;
	union pwrmgt_cntl_u pwrmgt_cntl;
	int auto_mode;
};

static struct power_state w100_pwr_state;

static struct w100_pll_info xtal_12500000[] = {
	{ 50,      0,   1,       0,     0xe0,        56},
	{ 75,      0,   5,       0,     0xde,        37},
	{100,      0,   7,       0,     0xe0,        28},
	{125,      0,   9,       0,     0xe0,        22},
	{150,      0,   11,      0,     0xe0,        17},
	{  0,      0,   0,       0,        0,         0},
};

static struct w100_pll_info xtal_14318000[] = {
	{ 40,      4,   13,      0,     0xe0,        80},
	{ 50,      1,   6,       0,     0xe0,	     64},
	{ 57,      2,   11,      0,     0xe0,        53},
	{ 75,      0,   4,       3,     0xe0,	     43},
	{100,      0,   6,       0,     0xe0,        32},
	{  0,      0,   0,       0,        0,         0},
};

static struct w100_pll_info xtal_16000000[] = {
	{ 72,      1,   8,       0,     0xe0,        48},
	{ 80,      1,   9,       0,     0xe0,        13},
	{ 95,      1,   10,      7,     0xe0,        38},
	{ 96,      1,   11,      0,     0xe0,        36},
	{  0,      0,   0,       0,        0,         0},
};

static struct pll_entries {
	int xtal_freq;
	struct w100_pll_info *pll_table;
} w100_pll_tables[] = {
	{ 12500000, &xtal_12500000[0] },
	{ 14318000, &xtal_14318000[0] },
	{ 16000000, &xtal_16000000[0] },
	{ 0 },
};

struct w100_pll_info *w100_get_xtal_table(unsigned int freq)
{
	struct pll_entries *pll_entry = w100_pll_tables;

	do {
		if (freq == pll_entry->xtal_freq)
			return pll_entry->pll_table;
		pll_entry++;
	} while (pll_entry->xtal_freq);

	return NULL;
}

static unsigned int w100_get_testcount(unsigned int testclk_sel)
{
	union clk_test_cntl_u clk_test_cntl;

	udelay(5);

	clk_test_cntl.f.start_check_freq = 0x0;
	clk_test_cntl.f.testclk_sel = testclk_sel;
	clk_test_cntl.f.tstcount_rst = 0x1;
	writel((u32) (clk_test_cntl.val), remapped_regs + mmCLK_TEST_CNTL);

	clk_test_cntl.f.tstcount_rst = 0x0;
	writel((u32) (clk_test_cntl.val), remapped_regs + mmCLK_TEST_CNTL);

	clk_test_cntl.f.start_check_freq = 0x1;
	writel((u32) (clk_test_cntl.val), remapped_regs + mmCLK_TEST_CNTL);

	udelay(20);

	clk_test_cntl.val = readl(remapped_regs + mmCLK_TEST_CNTL);
	clk_test_cntl.f.start_check_freq = 0x0;
	writel((u32) (clk_test_cntl.val), remapped_regs + mmCLK_TEST_CNTL);

	return clk_test_cntl.f.test_count;
}

static int w100_pll_adjust(struct w100_pll_info *pll)
{
	unsigned int tf80;
	unsigned int tf20;

	w100_pwr_state.pll_cntl.f.pll_pwdn = 0x0;
	w100_pwr_state.pll_cntl.f.pll_reset = 0x0;
	w100_pwr_state.pll_cntl.f.pll_tcpoff = 0x1;
	w100_pwr_state.pll_cntl.f.pll_pvg = 0x0;
	w100_pwr_state.pll_cntl.f.pll_vcofr = 0x0;
	w100_pwr_state.pll_cntl.f.pll_ioffset = 0x0;
	w100_pwr_state.pll_cntl.f.pll_ring_off = 0x0;

	do {
		w100_pwr_state.pll_cntl.f.pll_dactal = 0xd;
		writel((u32) (w100_pwr_state.pll_cntl.val), remapped_regs + mmPLL_CNTL);

		tf80 = w100_get_testcount(TESTCLK_SRC_PLL);
		if (tf80 >= (pll->tfgoal)) {
			w100_pwr_state.pll_cntl.f.pll_dactal = 0x7;
			writel((u32) (w100_pwr_state.pll_cntl.val), remapped_regs + mmPLL_CNTL);

			tf20 = w100_get_testcount(TESTCLK_SRC_PLL);
			if (tf20 <= (pll->tfgoal))
				return 1;

			if ((w100_pwr_state.pll_cntl.f.pll_vcofr == 0x0) &&
				((w100_pwr_state.pll_cntl.f.pll_pvg == 0x7) ||
				(w100_pwr_state.pll_cntl.f.pll_ioffset == 0x0))) {
				w100_pwr_state.pll_cntl.f.pll_vcofr = 0x1;
				w100_pwr_state.pll_cntl.f.pll_pvg = 0x0;
				w100_pwr_state.pll_cntl.f.pll_ioffset = 0x0;
				continue;
			}
		}
		if ((w100_pwr_state.pll_cntl.f.pll_ioffset) < 0x3) {
			w100_pwr_state.pll_cntl.f.pll_ioffset += 0x1;
		} else if ((w100_pwr_state.pll_cntl.f.pll_pvg) < 0x7) {
			w100_pwr_state.pll_cntl.f.pll_ioffset = 0x0;
			w100_pwr_state.pll_cntl.f.pll_pvg += 0x1;
		} else {
			return 0;
		}
	} while(1);
}

static int w100_pll_calibration(struct w100_pll_info *pll)
{
	int status;

	mdelay(50);

	status = w100_pll_adjust(pll);

	w100_pwr_state.pll_cntl.f.pll_dactal = 0xa;
	writel((u32) (w100_pwr_state.pll_cntl.val), remapped_regs + mmPLL_CNTL);

	udelay(1);

	w100_pwr_state.pll_cntl.f.pll_tcpoff = 0x0;
	writel((u32) (w100_pwr_state.pll_cntl.val), remapped_regs + mmPLL_CNTL);

	w100_pwr_state.pll_cntl.f.pll_dactal = 0x0;
	writel((u32) (w100_pwr_state.pll_cntl.val), remapped_regs + mmPLL_CNTL);

	udelay(400);

	return status;
}

static int w100_pll_set_clk(struct w100_pll_info *pll)
{
	int status;

	if (w100_pwr_state.auto_mode == 1)
	{
		w100_pwr_state.pwrmgt_cntl.f.pwm_fast_noml_hw_en = 0x0;
		w100_pwr_state.pwrmgt_cntl.f.pwm_noml_fast_hw_en = 0x0;
		writel((u32) (w100_pwr_state.pwrmgt_cntl.val), remapped_regs + mmPWRMGT_CNTL);
	}

	w100_pwr_state.sclk_cntl.f.sclk_src_sel = CLK_SRC_XTAL;
	writel((u32) (w100_pwr_state.sclk_cntl.val), remapped_regs + mmSCLK_CNTL);

	w100_pwr_state.pll_ref_fb_div.f.pll_ref_div = pll->M;
	w100_pwr_state.pll_ref_fb_div.f.pll_fb_div_int = pll->N_int;
	w100_pwr_state.pll_ref_fb_div.f.pll_fb_div_frac = pll->N_fac;
	w100_pwr_state.pll_ref_fb_div.f.pll_lock_time = pll->lock_time;
	writel((u32) (w100_pwr_state.pll_ref_fb_div.val), remapped_regs + mmPLL_REF_FB_DIV);

	w100_pwr_state.pwrmgt_cntl.f.pwm_mode_req = 0;
	writel((u32) (w100_pwr_state.pwrmgt_cntl.val), remapped_regs + mmPWRMGT_CNTL);

	status = w100_pll_calibration(pll);

	if (w100_pwr_state.auto_mode == 1)
	{
		w100_pwr_state.pwrmgt_cntl.f.pwm_fast_noml_hw_en = 0x1;
		w100_pwr_state.pwrmgt_cntl.f.pwm_noml_fast_hw_en = 0x1;
		writel((u32) (w100_pwr_state.pwrmgt_cntl.val), remapped_regs + mmPWRMGT_CNTL);
	}
	return status;
}

static int w100_pll_compute(struct w100_pll_info *out, unsigned int xtal_hz,
			     unsigned int target_hz)
{
	unsigned int total, target_mhz;

	target_mhz = target_hz / 1000000;
	if (!xtal_hz || !target_mhz)
		return 0;

	total = (target_hz * 8) / xtal_hz;
	if (total < 8 || (total / 8 - 1) > 63)
		return 0;

	out->freq = target_mhz;
	out->M = 0;
	out->N_int = (total / 8) - 1;
	out->N_fac = total % 8;
	out->tfgoal = 0xe0;
	out->lock_time = 2800 / target_mhz;
	if (!out->lock_time)
		out->lock_time = 1;

	return 1;
}

static int w100_pll_resolve(struct w100fb_par *par, unsigned int freq,
			     struct w100_pll_info *out)
{
	struct w100_pll_info *pll = par->pll_table;

	do {
		if (freq == pll->freq) {
			*out = *pll;
			return 1;
		}
		pll++;
	} while (pll->freq);

	return w100_pll_compute(out, par->mach->xtal_freq, freq * 1000000);
}

static int w100_set_pll_freq(struct w100fb_par *par, unsigned int freq)
{
	struct w100_pll_info pll;
	int status;

	if (!w100_pll_resolve(par, freq, &pll))
		return 0;

	status = w100_pll_set_clk(&pll);
	if (status) {
		par->pll_freq_hz = freq * 1000000;
		par->pll_freq_last_good = freq;
	} else if (par->pll_freq_last_good && par->pll_freq_last_good != freq) {
		struct w100_pll_info recover;

		if (w100_pll_resolve(par, par->pll_freq_last_good, &recover) &&
		    w100_pll_set_clk(&recover))
			par->pll_freq_hz = par->pll_freq_last_good * 1000000;
	}
	return status;
}

static void w100_pwm_setup(struct w100fb_par *par)
{
	w100_pwr_state.clk_pin_cntl.f.osc_en = 0x1;
	w100_pwr_state.clk_pin_cntl.f.osc_gain = 0x1f;
	w100_pwr_state.clk_pin_cntl.f.dont_use_xtalin = 0x0;
	w100_pwr_state.clk_pin_cntl.f.xtalin_pm_en = 0x0;
	w100_pwr_state.clk_pin_cntl.f.xtalin_dbl_en = par->mach->xtal_dbl ? 1 : 0;
	w100_pwr_state.clk_pin_cntl.f.cg_debug = 0x0;
	writel((u32) (w100_pwr_state.clk_pin_cntl.val), remapped_regs + mmCLK_PIN_CNTL);

	w100_pwr_state.sclk_cntl.f.sclk_src_sel = CLK_SRC_XTAL;
	w100_pwr_state.sclk_cntl.f.sclk_post_div_fast = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_clkon_hys = 0x3;
	w100_pwr_state.sclk_cntl.f.sclk_post_div_slow = 0x0;
	w100_pwr_state.sclk_cntl.f.disp_cg_ok2switch_en = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_force_reg = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_force_disp = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_force_mc = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_force_extmc = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_force_cp = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_force_e2 = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_force_e3 = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_force_idct = 0x0;
	w100_pwr_state.sclk_cntl.f.sclk_force_bist = 0x0;
	w100_pwr_state.sclk_cntl.f.busy_extend_cp = 0x0;
	w100_pwr_state.sclk_cntl.f.busy_extend_e2 = 0x0;
	w100_pwr_state.sclk_cntl.f.busy_extend_e3 = 0x0;
	w100_pwr_state.sclk_cntl.f.busy_extend_idct = 0x0;
	writel((u32) (w100_pwr_state.sclk_cntl.val), remapped_regs + mmSCLK_CNTL);

	w100_pwr_state.pclk_cntl.f.pclk_src_sel = CLK_SRC_XTAL;
	w100_pwr_state.pclk_cntl.f.pclk_post_div = 0x1;
	w100_pwr_state.pclk_cntl.f.pclk_force_disp = 0x0;
	writel((u32) (w100_pwr_state.pclk_cntl.val), remapped_regs + mmPCLK_CNTL);

	w100_pwr_state.pll_ref_fb_div.f.pll_ref_div = 0x0;
	w100_pwr_state.pll_ref_fb_div.f.pll_fb_div_int = 0x0;
	w100_pwr_state.pll_ref_fb_div.f.pll_fb_div_frac = 0x0;
	w100_pwr_state.pll_ref_fb_div.f.pll_reset_time = 0x5;
	w100_pwr_state.pll_ref_fb_div.f.pll_lock_time = 0xff;
	writel((u32) (w100_pwr_state.pll_ref_fb_div.val), remapped_regs + mmPLL_REF_FB_DIV);

	w100_pwr_state.pll_cntl.f.pll_pwdn = 0x1;
	w100_pwr_state.pll_cntl.f.pll_reset = 0x1;
	w100_pwr_state.pll_cntl.f.pll_pm_en = 0x0;
	w100_pwr_state.pll_cntl.f.pll_mode = 0x0;
	w100_pwr_state.pll_cntl.f.pll_refclk_sel = 0x0;
	w100_pwr_state.pll_cntl.f.pll_fbclk_sel = 0x0;
	w100_pwr_state.pll_cntl.f.pll_tcpoff = 0x0;
	w100_pwr_state.pll_cntl.f.pll_pcp = 0x4;
	w100_pwr_state.pll_cntl.f.pll_pvg = 0x0;
	w100_pwr_state.pll_cntl.f.pll_vcofr = 0x0;
	w100_pwr_state.pll_cntl.f.pll_ioffset = 0x0;
	w100_pwr_state.pll_cntl.f.pll_pecc_mode = 0x0;
	w100_pwr_state.pll_cntl.f.pll_pecc_scon = 0x0;
	w100_pwr_state.pll_cntl.f.pll_dactal = 0x0;
	w100_pwr_state.pll_cntl.f.pll_cp_clip = 0x3;
	w100_pwr_state.pll_cntl.f.pll_conf = 0x2;
	w100_pwr_state.pll_cntl.f.pll_mbctrl = 0x2;
	w100_pwr_state.pll_cntl.f.pll_ring_off = 0x0;
	writel((u32) (w100_pwr_state.pll_cntl.val), remapped_regs + mmPLL_CNTL);

	w100_pwr_state.pwrmgt_cntl.f.pwm_enable = 0x0;
	w100_pwr_state.pwrmgt_cntl.f.pwm_mode_req = 0x1;
	w100_pwr_state.pwrmgt_cntl.f.pwm_wakeup_cond = 0x0;
	w100_pwr_state.pwrmgt_cntl.f.pwm_fast_noml_hw_en = 0x0;
	w100_pwr_state.pwrmgt_cntl.f.pwm_noml_fast_hw_en = 0x0;
	w100_pwr_state.pwrmgt_cntl.f.pwm_fast_noml_cond = 0x1;
	w100_pwr_state.pwrmgt_cntl.f.pwm_noml_fast_cond = 0x1;
	w100_pwr_state.pwrmgt_cntl.f.pwm_idle_timer = 0xFF;
	w100_pwr_state.pwrmgt_cntl.f.pwm_busy_timer = 0xFF;
	writel((u32) (w100_pwr_state.pwrmgt_cntl.val), remapped_regs + mmPWRMGT_CNTL);

	w100_pwr_state.auto_mode = 0;
}

static unsigned int w100_target_pll_mhz(struct w100fb_par *par, struct w100_mode *mode)
{
	if (par->pll_override)
		return par->pll_override;
	return (par->fastpll_mode && mode->fast_pll_freq) ? mode->fast_pll_freq : mode->pll_freq;
}

static void w100_init_clocks(struct w100fb_par *par)
{
	struct w100_mode *mode = par->mode;

	if (mode->pixclk_src == CLK_SRC_PLL || mode->sysclk_src == CLK_SRC_PLL)
		w100_set_pll_freq(par, w100_target_pll_mhz(par, mode));

	w100_pwr_state.sclk_cntl.f.sclk_src_sel = mode->sysclk_src;
	w100_pwr_state.sclk_cntl.f.sclk_post_div_fast = mode->sysclk_divider;
	w100_pwr_state.sclk_cntl.f.sclk_post_div_slow = mode->sysclk_divider;
	writel((u32) (w100_pwr_state.sclk_cntl.val), remapped_regs + mmSCLK_CNTL);
}

static void w100_init_lcd(struct w100fb_par *par)
{
	u32 temp32;
	struct w100_mode *mode = par->mode;
	struct w100_gen_regs *regs = par->mach->regs;
	union active_h_disp_u active_h_disp;
	union active_v_disp_u active_v_disp;
	union graphic_h_disp_u graphic_h_disp;
	union graphic_v_disp_u graphic_v_disp;
	union crtc_total_u crtc_total;

	active_h_disp.val = 0;
	active_h_disp.f.active_h_start=mode->left_margin;
	active_h_disp.f.active_h_end=mode->left_margin + mode->xres;
	writel(active_h_disp.val, remapped_regs + mmACTIVE_H_DISP);

	active_v_disp.val = 0;
	active_v_disp.f.active_v_start=mode->upper_margin;
	active_v_disp.f.active_v_end=mode->upper_margin + mode->yres;
	writel(active_v_disp.val, remapped_regs + mmACTIVE_V_DISP);

	graphic_h_disp.val = 0;
	graphic_h_disp.f.graphic_h_start=mode->left_margin;
	graphic_h_disp.f.graphic_h_end=mode->left_margin + mode->xres;
	writel(graphic_h_disp.val, remapped_regs + mmGRAPHIC_H_DISP);

	graphic_v_disp.val = 0;
	graphic_v_disp.f.graphic_v_start=mode->upper_margin;
	graphic_v_disp.f.graphic_v_end=mode->upper_margin + mode->yres;
	writel(graphic_v_disp.val, remapped_regs + mmGRAPHIC_V_DISP);

	crtc_total.val = 0;
	crtc_total.f.crtc_h_total=mode->left_margin  + mode->xres + mode->right_margin;
	crtc_total.f.crtc_v_total=mode->upper_margin + mode->yres + mode->lower_margin;
	writel(crtc_total.val, remapped_regs + mmCRTC_TOTAL);

	writel(mode->crtc_ss, remapped_regs + mmCRTC_SS);
	writel(mode->crtc_ls, remapped_regs + mmCRTC_LS);
	writel(mode->crtc_gs, remapped_regs + mmCRTC_GS);
	writel(mode->crtc_vpos_gs, remapped_regs + mmCRTC_VPOS_GS);
	writel(mode->crtc_rev, remapped_regs + mmCRTC_REV);
	writel(mode->crtc_dclk, remapped_regs + mmCRTC_DCLK);
	writel(mode->crtc_gclk, remapped_regs + mmCRTC_GCLK);
	writel(mode->crtc_goe, remapped_regs + mmCRTC_GOE);
	writel(mode->crtc_ps1_active, remapped_regs + mmCRTC_PS1_ACTIVE);

	writel(regs->lcd_format, remapped_regs + mmLCD_FORMAT);
	writel(regs->lcdd_cntl1, remapped_regs + mmLCDD_CNTL1);
	writel(regs->lcdd_cntl2, remapped_regs + mmLCDD_CNTL2);
	writel(regs->genlcd_cntl1, remapped_regs + mmGENLCD_CNTL1);
	writel(regs->genlcd_cntl2, remapped_regs + mmGENLCD_CNTL2);
	writel(regs->genlcd_cntl3, remapped_regs + mmGENLCD_CNTL3);

	writel(0x00000000, remapped_regs + mmCRTC_FRAME);
	writel(0x00000000, remapped_regs + mmCRTC_FRAME_VPOS);
	writel(0x00000000, remapped_regs + mmCRTC_DEFAULT_COUNT);
	writel(0x0000FF00, remapped_regs + mmLCD_BACKGROUND_COLOR);

	temp32 = readl(remapped_regs + mmDISP_DEBUG2);
	temp32 |= 0xc0000000;
	writel(temp32, remapped_regs + mmDISP_DEBUG2);
}

static void w100_setup_memory(struct w100fb_par *par)
{
	union mc_ext_mem_location_u extmem_location;
	union mc_fb_location_u intmem_location;
	struct w100_mem_info *mem = par->mach->mem;
	struct w100_bm_mem_info *bm_mem = par->mach->bm_mem;

	if (!par->extmem_active) {
		w100_suspend(W100_SUSPEND_EXTMEM);

		intmem_location.f.mc_fb_start = W100_FB_BASE >> 8;
		intmem_location.f.mc_fb_top = (W100_FB_BASE+MEM_INT_SIZE) >> 8;
		writel((u32) (intmem_location.val), remapped_regs + mmMC_FB_LOCATION);

		extmem_location.f.mc_ext_mem_start = MEM_EXT_BASE_VALUE >> 8;
		extmem_location.f.mc_ext_mem_top = (MEM_EXT_BASE_VALUE-1) >> 8;
		writel((u32) (extmem_location.val), remapped_regs + mmMC_EXT_MEM_LOCATION);
	} else {
		intmem_location.f.mc_fb_start = MEM_INT_BASE_VALUE >> 8;
		intmem_location.f.mc_fb_top = (MEM_INT_BASE_VALUE+MEM_INT_SIZE) >> 8;
		writel((u32) (intmem_location.val), remapped_regs + mmMC_FB_LOCATION);

		extmem_location.f.mc_ext_mem_start = W100_FB_BASE >> 8;
		extmem_location.f.mc_ext_mem_top = (W100_FB_BASE+par->mach->mem->size) >> 8;
		writel((u32) (extmem_location.val), remapped_regs + mmMC_EXT_MEM_LOCATION);

		writel(0x00007800, remapped_regs + mmMC_BIST_CTRL);
		writel(mem->ext_cntl, remapped_regs + mmMEM_EXT_CNTL);
		writel(0x00200021, remapped_regs + mmMEM_SDRAM_MODE_REG);
		udelay(100);
		writel(0x80200021, remapped_regs + mmMEM_SDRAM_MODE_REG);
		udelay(100);
		writel(par->sdram_mode_reg_override ? par->sdram_mode_reg_override : mem->sdram_mode_reg,
		       remapped_regs + mmMEM_SDRAM_MODE_REG);
		udelay(100);
		writel(mem->ext_timing_cntl, remapped_regs + mmMEM_EXT_TIMING_CNTL);
		writel(mem->io_cntl, remapped_regs + mmMEM_IO_CNTL);
		if (bm_mem) {
			writel(bm_mem->ext_mem_bw, remapped_regs + mmBM_EXT_MEM_BANDWIDTH);
			writel(bm_mem->offset, remapped_regs + mmBM_OFFSET);
			writel(bm_mem->ext_timing_ctl, remapped_regs + mmBM_MEM_EXT_TIMING_CNTL);
			writel(bm_mem->ext_cntl, remapped_regs + mmBM_MEM_EXT_CNTL);
			writel(bm_mem->mode_reg, remapped_regs + mmBM_MEM_MODE_REG);
			writel(bm_mem->io_cntl, remapped_regs + mmBM_MEM_IO_CNTL);
			writel(bm_mem->config, remapped_regs + mmBM_CONFIG);
		}
	}
}

static void w100_set_dispregs(struct w100fb_par *par)
{
	unsigned long rot=0, divider, offset=0;
	bool rotated = (par->xres != par->mode->xres);
	union graphic_ctrl_u graphic_ctrl;

	if (!rotated) {
		if (par->flip) {
			rot=3;
			offset=(par->xres * par->yres) - 1;
		}
	} else {
		if (par->flip) {
			rot=2;
			offset=par->xres - 1;
		} else {
			rot=1;
			offset=par->xres * (par->yres - 1);
		}
	}
	divider = w100_current_pixclk_divider(par, par->mode, rotated);

	graphic_ctrl.val = 0;
	switch (par->chip_id) {
		case CHIP_ID_W100:
			graphic_ctrl.f_w100.color_depth=6;
			graphic_ctrl.f_w100.en_crtc=1;
			graphic_ctrl.f_w100.en_graphic_req=1;
			graphic_ctrl.f_w100.en_graphic_crtc=1;
			graphic_ctrl.f_w100.lcd_pclk_on=1;
			graphic_ctrl.f_w100.lcd_sclk_on=1;
			graphic_ctrl.f_w100.low_power_on=0;
			graphic_ctrl.f_w100.req_freq=0;
			graphic_ctrl.f_w100.portrait_mode=rot;

			switch(par->xres) {
				case 240:
				case 320:
				default:
					graphic_ctrl.f_w100.total_req_graphic=0xa0;
					break;
				case 480:
				case 640:
					switch(rot) {
						case 0:
						case 3:
							graphic_ctrl.f_w100.low_power_on=1;
							graphic_ctrl.f_w100.req_freq=5;
						break;
						case 1:
						case 2:
							graphic_ctrl.f_w100.req_freq=4;
							break;
						default:
							break;
					}
					graphic_ctrl.f_w100.total_req_graphic=0xf0;
					break;
			}

			if (w100_force_fullrate) {
				graphic_ctrl.f_w100.low_power_on = 0;
				graphic_ctrl.f_w100.req_freq = 0;
			}
			break;
		case CHIP_ID_W3200:
		case CHIP_ID_W3220:
			graphic_ctrl.f_w32xx.color_depth=6;
			graphic_ctrl.f_w32xx.en_crtc=1;
			graphic_ctrl.f_w32xx.en_graphic_req=1;
			graphic_ctrl.f_w32xx.en_graphic_crtc=1;
			graphic_ctrl.f_w32xx.lcd_pclk_on=1;
			graphic_ctrl.f_w32xx.lcd_sclk_on=1;
			graphic_ctrl.f_w32xx.low_power_on=0;
			graphic_ctrl.f_w32xx.req_freq=0;
			graphic_ctrl.f_w32xx.total_req_graphic=par->mode->xres >> 1;
			graphic_ctrl.f_w32xx.portrait_mode=rot;
			break;
	}

	w100_pwr_state.pclk_cntl.f.pclk_src_sel = par->mode->pixclk_src;
	w100_pwr_state.pclk_cntl.f.pclk_post_div = divider;
	writel((u32) (w100_pwr_state.pclk_cntl.val), remapped_regs + mmPCLK_CNTL);

	writel(graphic_ctrl.val, remapped_regs + mmGRAPHIC_CTRL);
	writel(W100_FB_BASE + ((offset * BITS_PER_PIXEL/8)&~0x03UL), remapped_regs + mmGRAPHIC_OFFSET);
	writel((par->xres*BITS_PER_PIXEL/8), remapped_regs + mmGRAPHIC_PITCH);
}

static void calc_hsync(struct w100fb_par *par)
{
	unsigned long hsync;
	struct w100_mode *mode = par->mode;
	union crtc_ss_u crtc_ss;

	if (mode->pixclk_src == CLK_SRC_XTAL)
		hsync=par->mach->xtal_freq;
	else
		hsync=w100_target_pll_mhz(par, mode)*100000;

	hsync /= (w100_pwr_state.pclk_cntl.f.pclk_post_div + 1);

	crtc_ss.val = readl(remapped_regs + mmCRTC_SS);
	if (crtc_ss.val)
		par->hsync_len = hsync / (crtc_ss.f.ss_end-crtc_ss.f.ss_start);
	else
		par->hsync_len = 0;
}

static void w100_suspend(u32 mode)
{
	u32 val;

	writel(0x7FFF8000, remapped_regs + mmMC_EXT_MEM_LOCATION);
	writel(0x00FF0000, remapped_regs + mmMC_PERF_MON_CNTL);

	val = readl(remapped_regs + mmMEM_EXT_TIMING_CNTL);
	val &= ~(0x00100000);
	val |= 0xFF000000;
	writel(val, remapped_regs + mmMEM_EXT_TIMING_CNTL);

	val = readl(remapped_regs + mmMEM_EXT_CNTL);
	val &= ~(0x00040000);
	val |= 0x00080000;
	writel(val, remapped_regs + mmMEM_EXT_CNTL);

	udelay(1);

	if (mode == W100_SUSPEND_EXTMEM) {
		val = readl(remapped_regs + mmMEM_EXT_CNTL);
		val |= 0x40000000;
		writel(val, remapped_regs + mmMEM_EXT_CNTL);

		val = readl(remapped_regs + mmMEM_EXT_CNTL);
		val &= ~(0x00000001);
		writel(val, remapped_regs + mmMEM_EXT_CNTL);
	} else {
		writel(0x00000000, remapped_regs + mmSCLK_CNTL);
		writel(0x000000BF, remapped_regs + mmCLK_PIN_CNTL);
		writel(0x00000015, remapped_regs + mmPWRMGT_CNTL);

		udelay(5);

		val = readl(remapped_regs + mmPLL_CNTL);
		val |= 0x00000004;
		writel(val, remapped_regs + mmPLL_CNTL);

		writel(0x00000000, remapped_regs + mmLCDD_CNTL1);
		writel(0x00000000, remapped_regs + mmLCDD_CNTL2);
		writel(0x00000000, remapped_regs + mmGENLCD_CNTL1);
		writel(0x00000000, remapped_regs + mmGENLCD_CNTL2);
		writel(0x00000000, remapped_regs + mmGENLCD_CNTL3);

		val = readl(remapped_regs + mmMEM_EXT_CNTL);
		val |= 0xF0000000;
		val &= ~(0x00000001);
		writel(val, remapped_regs + mmMEM_EXT_CNTL);

		writel(0x0000001d, remapped_regs + mmPWRMGT_CNTL);
	}
}

static void w100_vsync_pause(bool tight)
{
	if (tight) {
		if (preemptible())
			usleep_range(50, 100);
		else
			udelay(20);
		return;
	}

	if (preemptible())
		usleep_range(500, 1000);
	else
		udelay(100);
}

static int w100_vsync(bool tight)
{
	u32 cntl, stat;
	u32 tmp;
	ktime_t deadline = ktime_add_ms(ktime_get(), W100_VSYNC_TIMEOUT_MS);
	bool got_vline = false;

	if (w100_vsync_mode == 1) {
		u32 start = readl(remapped_regs + mmCRTC_FRAME);

		while (ktime_before(ktime_get(), deadline)) {
			u32 cur = readl(remapped_regs + mmCRTC_FRAME);

			if (cur != start)
				return 0;
			w100_vsync_pause(tight);
		}

		if (w100_vsync_debug) {
			cntl = readl(remapped_regs + mmGEN_INT_CNTL);
			stat = readl(remapped_regs + mmGEN_INT_STATUS);
			pr_warn_ratelimited("w100fb: vsync(frame) timeout start=%#x cur=%#x int_cntl=%#x int_stat=%#x (mmCRTC_FRAME is stuck at 0 on Corgi -- use vsync_mode=0)\n",
					   start, readl(remapped_regs + mmCRTC_FRAME), cntl, stat);
		} else {
			pr_warn_ratelimited("w100fb: vsync(frame) wait timed out (mmCRTC_FRAME is stuck at 0 on Corgi -- use vsync_mode=0)\n");
		}
		return -ETIMEDOUT;
	}

	tmp = readl(remapped_regs + mmACTIVE_V_DISP);

	writel((tmp >> 16) & 0x3ff, remapped_regs + mmDISP_INT_CNTL);

	tmp = readl(remapped_regs + mmGEN_INT_CNTL);

	tmp &= ~0x00000002;
	writel(tmp, remapped_regs + mmGEN_INT_CNTL);

	writel(0x00000002, remapped_regs + mmGEN_INT_STATUS);

	writel((tmp | 0x00000002), remapped_regs + mmGEN_INT_CNTL);

	writel(0x00000002, remapped_regs + mmGEN_INT_STATUS);

	while (ktime_before(ktime_get(), deadline)) {
		if (!(readl(remapped_regs + mmGEN_INT_STATUS) & 0x00000002))
			break;
		writel(0x00000002, remapped_regs + mmGEN_INT_STATUS);
		w100_vsync_pause(tight);
	}

	while (ktime_before(ktime_get(), deadline)) {
		if (readl(remapped_regs + mmGEN_INT_STATUS) & 0x00000002) {
			got_vline = true;
			break;
		}
		w100_vsync_pause(tight);
	}

	writel(tmp, remapped_regs + mmGEN_INT_CNTL);

	writel(0x00000002, remapped_regs + mmGEN_INT_STATUS);

	if (!got_vline) {
		if (w100_vsync_debug) {
			cntl = readl(remapped_regs + mmGEN_INT_CNTL);
			stat = readl(remapped_regs + mmGEN_INT_STATUS);
			pr_warn_ratelimited("w100fb: vsync(irq) timeout int_cntl=%#x int_stat=%#x disp_int_cntl=%#x active_v=%#x frame=%#x\n",
					   cntl, stat, readl(remapped_regs + mmDISP_INT_CNTL),
					   readl(remapped_regs + mmACTIVE_V_DISP),
					   readl(remapped_regs + mmCRTC_FRAME));
		} else {
			pr_warn_ratelimited("w100fb: vsync wait timed out\n");
		}
		return -ETIMEDOUT;
	}

	return 0;
}

static struct platform_driver w100fb_driver = {
	.probe		= w100fb_probe,
	.remove		= w100fb_remove,
	.suspend	= w100fb_suspend,
	.resume		= w100fb_resume,
	.driver		= {
		.name	= "w100fb",
		.dev_groups	= w100fb_groups,
	},
};

module_platform_driver(w100fb_driver);

MODULE_DESCRIPTION("ATI Imageon w100 framebuffer driver");
MODULE_LICENSE("GPL");
