/*
 * w100accel-test -- verify the two w100fb fixes from PR #118 on real
 * hardware: the extmem_active/smem_len correction ("the video-mem fix")
 * and the W100FB_IOC_FILL/BLIT/SYNC 2D-accel ioctls ("the 2D blit
 * engine"). See docs/HANDOFF-2026-08-04-W100-ACCEL.md for the full
 * context -- this is the tool that doc tells you to run.
 *
 * Build (the rootfs ships no dynamic linker -- static is mandatory, same
 * as tools/src/fbflip.c):
 *   arm-unknown-linux-uclibcgnueabi-gcc -march=armv5te -O2 -static \
 *       -Wall -Wextra -o w100accel-test tools/src/w100accel-test.c
 *
 * Modes:
 *   w100accel-test probe   just check the running kernel has the new
 *                          ioctls at all -- run this first
 *   w100accel-test smem    the video-mem fix: claim a virtual buffer
 *                          bigger than internal SRAM and confirm
 *                          fix.smem_len actually covers what was claimed.
 *                          Reports SKIP, not PASS, if external SDRAM was
 *                          already mapped in before the claim -- see
 *                          mode_smem() for why that cannot isolate the fix
 *   w100accel-test accel   the 2D blit engine, on-screen only: FILL a
 *                          rect, SYNC, read back and verify; BLIT it
 *                          elsewhere, SYNC, read back and verify
 *   w100accel-test spare   both fixes together, end to end: claim spare
 *                          VRAM past the visible frame (as in "smem"),
 *                          FILL an OFF-SCREEN rect there, BLIT it onto
 *                          a visible rect, verify the visible rect shows
 *                          the fill color. This is the one that actually
 *                          exercises "keep a sprite in spare VRAM and
 *                          composite it in" -- the feature PR #118 exists
 *                          for. Fails cleanly (not silently) if either
 *                          fix is missing: no smem growth -> the kernel's
 *                          own w100fb_rect_fits() rejects the off-screen
 *                          FILL with -EINVAL before this tool's own
 *                          checks even run.
 *
 * Every mode prints its own PASS/FAIL lines and exits 0 only if
 * everything it checked passed -- safe to drive from a script:
 *   for m in probe smem accel spare; do w100accel-test $m || echo "$m FAILED"; done
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/* The one canonical copy of the command numbers and struct layout --
 * modules/w100/w100fb_accel.h, same file the kernel driver includes as
 * <video/w100fb_accel.h>. Reached by relative path rather than duplicated
 * here, so this tool can never silently drift from the real ABI. */
#include "../../modules/w100/w100fb_accel.h"

/* MEM_INT_SIZE+1 from modules/w100/w100fb_private.h -- the internal-SRAM
 * threshold the video-mem fix is about. Not worth sharing a header for a
 * single driver-private constant; if it ever changes there, change it
 * here too. */
#define MEM_INT_SIZE_PLUS_1	(0x05ffffUL + 1)

#define RGB565_RED	0xF800
#define RGB565_GREEN	0x07E0
#define RGB565_BLUE	0x001F
#define RGB565_BLACK	0x0000

static int g_fd = -1;
static int g_fail;

static void fail(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "FAIL: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
	g_fail = 1;
}

static void pass(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("PASS: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

/*
 * Neither PASS nor FAIL: the check could not be made meaningful in the
 * state the device happened to be in. Deliberately does NOT set g_fail --
 * a precondition this tool cannot establish for itself is not a defect in
 * the kernel, and reporting it as one sends the next reader hunting a bug
 * that isn't there (which is exactly what happened on 2026-08-04, see
 * mode_smem()).
 */
static void skip(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("SKIP: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

static int get_var(struct fb_var_screeninfo *var)
{
	if (ioctl(g_fd, FBIOGET_VSCREENINFO, var) < 0) {
		perror("FBIOGET_VSCREENINFO");
		return -1;
	}
	return 0;
}

static int get_fix(struct fb_fix_screeninfo *fix)
{
	if (ioctl(g_fd, FBIOGET_FSCREENINFO, fix) < 0) {
		perror("FBIOGET_FSCREENINFO");
		return -1;
	}
	return 0;
}

/*
 * 0 if the running kernel answers W100FB_IOC_SYNC (a no-op probe -- it
 * only waits for the engine to go idle, it can't corrupt anything even
 * if called first thing). Nonzero if this device is still running a
 * kernel from before PR #118 -- the whole point of "probe" mode.
 */
static int probe_ioctls(void)
{
	if (ioctl(g_fd, W100FB_IOC_SYNC) < 0) {
		fail("W100FB_IOC_SYNC: %s -- this kernel does not have the "
		     "new accel ioctls. Rebuild + redeploy per "
		     "docs/HOWTO-BUILD-DEPLOY-KERNEL.md, then re-run.",
		     strerror(errno));
		return -1;
	}
	pass("W100FB_IOC_SYNC answered -- this kernel has the new ioctls");
	return 0;
}

/*
 * Claim a virtual buffer just over the internal-SRAM threshold. Returns
 * the fb_var_screeninfo actually accepted via *out, so the caller (and
 * cleanup) has it. Returns -1 on any failure.
 */
static int claim_spare(struct fb_var_screeninfo *out)
{
	struct fb_var_screeninfo var, want;
	unsigned long per_row, need_yres_virtual;

	if (get_var(&var) < 0)
		return -1;

	per_row = (unsigned long)var.xres * (var.bits_per_pixel / 8);
	if (!per_row) {
		fail("var.xres or bits_per_pixel is zero, can't compute a target");
		return -1;
	}

	if ((unsigned long)var.xres * var.yres * (var.bits_per_pixel / 8)
	    > MEM_INT_SIZE_PLUS_1) {
		fail("current mode's VISIBLE frame (%ux%u) already exceeds "
		     "the internal-SRAM threshold (%lu bytes) on its own -- "
		     "extmem_active would be forced regardless of the fix "
		     "under test. Switch to a small mode first (e.g. "
		     "`matchbox-fbrun --qvga -- w100accel-test <mode>`) and re-run.",
		     var.xres, var.yres, MEM_INT_SIZE_PLUS_1);
		return -1;
	}

	need_yres_virtual = MEM_INT_SIZE_PLUS_1 / per_row + 2;
	if (need_yres_virtual < var.yres)
		need_yres_virtual = var.yres;

	want = var;
	want.xres_virtual = var.xres;
	want.yres_virtual = (unsigned int)need_yres_virtual;
	want.xoffset = 0;
	want.yoffset = 0;
	want.activate = FB_ACTIVATE_NOW;

	if (ioctl(g_fd, FBIOPUT_VSCREENINFO, &want) < 0) {
		fail("FBIOPUT_VSCREENINFO(yres_virtual=%lu): %s",
		     need_yres_virtual, strerror(errno));
		return -1;
	}
	if (get_var(&want) < 0)
		return -1;
	if (want.yres_virtual < need_yres_virtual) {
		fail("FBIOPUT_VSCREENINFO accepted but yres_virtual is only "
		     "%u, wanted >= %lu -- driver clamped it",
		     want.yres_virtual, need_yres_virtual);
		return -1;
	}

	*out = want;
	return 0;
}

static void restore_var(const struct fb_var_screeninfo *original)
{
	struct fb_var_screeninfo v = *original;

	v.activate = FB_ACTIVATE_NOW;
	if (ioctl(g_fd, FBIOPUT_VSCREENINFO, &v) < 0)
		fprintf(stderr, "warning: could not restore original mode: %s\n",
			strerror(errno));
}

/*
 * mode: smem -- the video-mem fix on its own.
 *
 * What is actually under test is "does smem_len cover the virtual size
 * set_par() just accepted", NOT "did smem_len get bigger". Those are not
 * the same question, and an earlier version of this asked the second one:
 * it compared smem_len before and after the claim and called any lack of
 * growth the PR #118 bug. On a device where external SDRAM is ALREADY
 * mapped in when the test starts, smem_len is already at its maximum and
 * cannot grow, so that check failed while the driver was behaving
 * perfectly -- and printed a diagnosis ("set_par() left external SDRAM
 * unmapped") that the numbers it had just printed flatly contradicted.
 *
 * That state is easy to be in and hard to see: the desktop runs
 * double-buffered VGA (640x960 virtual = 1228800 bytes), which is already
 * past the internal-SRAM threshold, and dropping to QVGA with
 * matchbox-fbrun does not power external memory back off. So the "before"
 * this test wants -- external off -- is not something running the test
 * inside a live session can produce.
 *
 * Hence: fail only on the real defect (smem_len not covering the claim),
 * and SKIP when external memory was already on beforehand, since that run
 * genuinely cannot isolate the fix either way.
 */
static int mode_smem(void)
{
	struct fb_var_screeninfo original, claimed;
	struct fb_fix_screeninfo fix_before, fix_after;
	unsigned long long claimed_bytes;

	if (get_var(&original) < 0)
		return -1;
	if (get_fix(&fix_before) < 0)
		return -1;

	printf("before: smem_len = %u bytes (internal-SRAM bucket = %lu)\n",
	       fix_before.smem_len, MEM_INT_SIZE_PLUS_1);

	if (claim_spare(&claimed) < 0)
		return -1;

	if (get_fix(&fix_after) < 0)
		return -1;

	claimed_bytes = (unsigned long long)fix_after.line_length *
			claimed.yres_virtual;

	printf("after:  smem_len = %u bytes (yres_virtual now %u, which needs "
	       "%llu)\n", fix_after.smem_len, claimed.yres_virtual,
	       claimed_bytes);

	if (fix_after.smem_len < claimed_bytes) {
		fail("smem_len (%u) does not cover the virtual size set_par() "
		     "just accepted (%llu bytes) -- this is exactly the bug "
		     "PR #118 fixes: check_var() approved this virtual size, "
		     "but set_par() left external SDRAM unmapped/suspended, so "
		     "anything past %lu bytes would land on memory that "
		     "genuinely isn't there.",
		     fix_after.smem_len, claimed_bytes, MEM_INT_SIZE_PLUS_1);
	} else if (fix_before.smem_len > MEM_INT_SIZE_PLUS_1) {
		skip("external SDRAM was already mapped in before this test "
		     "claimed anything (smem_len was %u, past the %lu "
		     "internal-SRAM bucket), so smem_len had no room left to "
		     "grow and this run cannot isolate the video-mem fix. It "
		     "does still show smem_len (%u) covering the accepted "
		     "virtual size (%llu). For a run that isolates the fix, "
		     "the device has to reach a small mode without a large "
		     "virtual buffer having been claimed first -- e.g. boot "
		     "straight into QVGA rather than dropping into it from "
		     "the double-buffered VGA desktop. `spare` mode does not "
		     "have this limitation and tests the same memory for "
		     "real.",
		     fix_before.smem_len, MEM_INT_SIZE_PLUS_1,
		     fix_after.smem_len, claimed_bytes);
	} else {
		pass("smem_len grew past the internal-SRAM bucket (%u -> %u) "
		     "once a bigger virtual buffer was claimed, and covers it "
		     "(%llu needed) -- the fix is present",
		     fix_before.smem_len, fix_after.smem_len, claimed_bytes);
	}

	restore_var(&original);
	return g_fail ? -1 : 0;
}

/* Read back `count` RGB565 pixels starting at byte `offset` of an mmap'd
 * fb and report whether they all equal `want`. */
static int verify_solid(unsigned short *fb, size_t base_px, unsigned w,
			 unsigned h, unsigned stride_px, unsigned short want,
			 const char *label)
{
	unsigned x, y;
	unsigned mismatches = 0;
	unsigned short first_bad = 0;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			unsigned short px = fb[base_px + (size_t)y * stride_px + x];
			if (px != want) {
				if (!mismatches)
					first_bad = px;
				mismatches++;
			}
		}
	}

	if (!mismatches) {
		pass("%s: all %ux%u pixels are %#06x as expected", label, w, h, want);
		return 0;
	}
	fail("%s: %u of %u pixels are wrong (wanted %#06x, e.g. saw %#06x)",
	     label, mismatches, w * h, want, first_bad);
	return -1;
}

/* mode: accel -- FILL + SYNC + BLIT + SYNC, on-screen only. Safe to run
 * even against a kernel where the smem_len fix landed but hasn't been
 * exercised, since it never touches memory past the visible frame. */
static int mode_accel(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	struct w100fb_fill_args fill;
	struct w100fb_blit_args blit;
	unsigned short *fb;
	unsigned stride_px, rw = 16, rh = 16;
	unsigned sx0 = 4, sy0 = 4, dx0, dy0;

	if (probe_ioctls() < 0)
		return -1;
	if (get_var(&var) < 0 || get_fix(&fix) < 0)
		return -1;

	if (var.xres < rw * 3 || var.yres < rh * 3) {
		fail("mode %ux%u too small for this test's fixed rects", var.xres, var.yres);
		return -1;
	}
	dx0 = var.xres - rw - 4;
	dy0 = var.yres - rh - 4;
	stride_px = fix.line_length / 2;

	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap");
		return -1;
	}

	memset(&fill, 0, sizeof(fill));
	fill.dst_offset = 0;
	fill.dst_pitch = fix.line_length;
	fill.x = sx0; fill.y = sy0;
	fill.width = rw; fill.height = rh;
	fill.color = RGB565_GREEN;
	if (ioctl(g_fd, W100FB_IOC_FILL, &fill) < 0) {
		fail("W100FB_IOC_FILL (on-screen): %s", strerror(errno));
		munmap(fb, fix.smem_len);
		return -1;
	}
	if (ioctl(g_fd, W100FB_IOC_SYNC) < 0)
		fail("W100FB_IOC_SYNC after FILL: %s", strerror(errno));

	verify_solid(fb, (size_t)sy0 * stride_px + sx0, rw, rh, stride_px,
		     RGB565_GREEN, "FILL readback");

	memset(&blit, 0, sizeof(blit));
	blit.src_offset = 0; blit.src_pitch = fix.line_length;
	blit.dst_offset = 0; blit.dst_pitch = fix.line_length;
	blit.sx = sx0; blit.sy = sy0;
	blit.dx = dx0; blit.dy = dy0;
	blit.width = rw; blit.height = rh;
	if (ioctl(g_fd, W100FB_IOC_BLIT, &blit) < 0) {
		fail("W100FB_IOC_BLIT (on-screen): %s", strerror(errno));
		munmap(fb, fix.smem_len);
		return -1;
	}
	if (ioctl(g_fd, W100FB_IOC_SYNC) < 0)
		fail("W100FB_IOC_SYNC after BLIT: %s", strerror(errno));

	verify_solid(fb, (size_t)dy0 * stride_px + dx0, rw, rh, stride_px,
		     RGB565_GREEN, "BLIT readback");

	/* Clean up: put both rects back to black rather than leaving green
	 * squares on whatever was on screen. */
	fill.color = RGB565_BLACK;
	fill.x = sx0; fill.y = sy0;
	ioctl(g_fd, W100FB_IOC_FILL, &fill);
	fill.x = dx0; fill.y = dy0;
	ioctl(g_fd, W100FB_IOC_FILL, &fill);
	ioctl(g_fd, W100FB_IOC_SYNC);

	munmap(fb, fix.smem_len);
	return g_fail ? -1 : 0;
}

/* mode: spare -- both fixes together. The point of the whole feature:
 * a sprite kept in spare VRAM (not the visible frame) composited onto
 * the screen by the 2D engine, not a CPU memcpy. */
static int mode_spare(void)
{
	struct fb_var_screeninfo original, claimed;
	struct fb_fix_screeninfo fix;
	struct w100fb_fill_args fill;
	struct w100fb_blit_args blit;
	unsigned short *fb;
	unsigned stride_px, rw = 16, rh = 16;
	unsigned dx0, dy0;
	unsigned long off_offset;

	if (probe_ioctls() < 0)
		return -1;
	if (get_var(&original) < 0)
		return -1;

	if (claim_spare(&claimed) < 0)
		return -1;
	if (get_fix(&fix) < 0) {
		restore_var(&original);
		return -1;
	}
	if (fix.smem_len <= MEM_INT_SIZE_PLUS_1) {
		fail("smem_len is still %u after claiming spare VRAM -- the "
		     "video-mem fix is missing or not working; see `smem` "
		     "mode for a focused check. Not attempting the off-screen "
		     "FILL, it would just be rejected by the kernel's own "
		     "bounds check.", fix.smem_len);
		restore_var(&original);
		return -1;
	}

	stride_px = fix.line_length / 2;
	/* Right after the visible frame's last row -- definitely off-screen,
	 * definitely within the now-larger smem_len. */
	off_offset = (unsigned long)fix.line_length * claimed.yres;

	if (off_offset + (unsigned long)fix.line_length * rh > fix.smem_len) {
		fail("off-screen rect (offset %lu, %u bytes) would not fit in "
		     "smem_len=%u -- claim_spare() didn't leave enough room "
		     "for this test's fixed rect size",
		     off_offset, fix.line_length * rh, fix.smem_len);
		restore_var(&original);
		return -1;
	}

	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap");
		restore_var(&original);
		return -1;
	}

	memset(&fill, 0, sizeof(fill));
	fill.dst_offset = (unsigned int)off_offset;
	fill.dst_pitch = fix.line_length;
	fill.x = 0; fill.y = 0;
	fill.width = rw; fill.height = rh;
	fill.color = RGB565_BLUE;
	if (ioctl(g_fd, W100FB_IOC_FILL, &fill) < 0) {
		fail("W100FB_IOC_FILL (off-screen, offset=%lu): %s -- if this "
		     "is -EINVAL, check whether the rect really fits inside "
		     "the CURRENT mode's clip (see w100fb_rect_fits() in the "
		     "driver, it clips to [0,0]-[xres,yres] regardless of "
		     "which surface is targeted)",
		     off_offset, strerror(errno));
		munmap(fb, fix.smem_len);
		restore_var(&original);
		return -1;
	}
	if (ioctl(g_fd, W100FB_IOC_SYNC) < 0)
		fail("W100FB_IOC_SYNC after off-screen FILL: %s", strerror(errno));

	dx0 = claimed.xres > rw + 8 ? 4 : 0;
	dy0 = claimed.yres > rh + 8 ? 4 : 0;

	memset(&blit, 0, sizeof(blit));
	blit.src_offset = (unsigned int)off_offset;
	blit.src_pitch = fix.line_length;
	blit.dst_offset = 0;
	blit.dst_pitch = fix.line_length;
	blit.sx = 0; blit.sy = 0;
	blit.dx = dx0; blit.dy = dy0;
	blit.width = rw; blit.height = rh;
	if (ioctl(g_fd, W100FB_IOC_BLIT, &blit) < 0) {
		fail("W100FB_IOC_BLIT (off-screen -> on-screen): %s", strerror(errno));
		munmap(fb, fix.smem_len);
		restore_var(&original);
		return -1;
	}
	if (ioctl(g_fd, W100FB_IOC_SYNC) < 0)
		fail("W100FB_IOC_SYNC after BLIT: %s", strerror(errno));

	verify_solid(fb, (size_t)dy0 * stride_px + dx0, rw, rh, stride_px,
		     RGB565_BLUE, "off-screen sprite composited on-screen");

	/* Clean up the visible rect. */
	fill.dst_offset = 0;
	fill.x = dx0; fill.y = dy0;
	fill.color = RGB565_BLACK;
	ioctl(g_fd, W100FB_IOC_FILL, &fill);
	ioctl(g_fd, W100FB_IOC_SYNC);

	munmap(fb, fix.smem_len);
	restore_var(&original);
	return g_fail ? -1 : 0;
}

static int mode_info(void)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;

	if (get_var(&var) < 0 || get_fix(&fix) < 0)
		return -1;

	printf("fb: %ux%u %ubpp  virtual %ux%u\n",
	       var.xres, var.yres, var.bits_per_pixel,
	       var.xres_virtual, var.yres_virtual);
	printf("    fix.smem_len    = %u\n", fix.smem_len);
	printf("    fix.line_length = %u\n", fix.line_length);
	printf("    internal-SRAM bucket = %lu bytes\n", MEM_INT_SIZE_PLUS_1);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "info";
	int rc;

	g_fd = open("/dev/fb0", O_RDWR);
	if (g_fd < 0) {
		perror("open /dev/fb0");
		return 1;
	}

	if (!strcmp(mode, "info"))
		rc = mode_info();
	else if (!strcmp(mode, "probe"))
		rc = probe_ioctls();
	else if (!strcmp(mode, "smem"))
		rc = mode_smem();
	else if (!strcmp(mode, "accel"))
		rc = mode_accel();
	else if (!strcmp(mode, "spare"))
		rc = mode_spare();
	else {
		fprintf(stderr, "usage: %s [info|probe|smem|accel|spare]\n", argv[0]);
		rc = -1;
	}

	close(g_fd);
	return rc < 0 || g_fail ? 1 : 0;
}
