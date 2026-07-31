/*
 * fbflip -- exercise fbdev page flipping on the w100, and say plainly which
 * part of the path is broken.
 *
 * Written for the tearing investigation (2026-07-31). The DEADLETTER-W100-VSYNC
 * note asks that these throwaway diagnostics be kept rather than rebuilt from
 * scratch each time, so this lives in tools/src/.
 *
 * Build (the rootfs ships no dynamic linker -- static is mandatory):
 *   arm-unknown-linux-uclibcgnueabi-gcc -march=armv5te -O2 -static \
 *       -Wall -Wextra -o fbflip tools/src/fbflip.c
 *
 * Modes:
 *   fbflip info          report the fields that gate panning, change nothing
 *   fbflip setup         try to claim a second buffer (yres_virtual = 2*yres)
 *   fbflip flip [n]      claim, fill the two buffers with solid colours, and
 *                        flip between them n times (default 60), timing each
 *   fbflip vsync [n]     time FBIO_WAITFORVSYNC, for comparison
 *
 * What to look for in 'flip': the two buffers are solid red and solid blue.
 * A clean flip shows solid colour. A torn flip shows a band of the other
 * colour -- and on this board that band is VERTICAL, because the CRTC scans
 * out rotated 90 degrees (graphic_ctrl.portrait_mode=1), so a discontinuity
 * along the panel's scan direction maps to a vertical band in the landscape
 * image. A vertical seam is therefore the signature of a mistimed flip, not
 * of anything userspace drew.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, unsigned int)
#endif

static long elapsed_us(struct timespec *a, struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000000L +
	       (b->tv_nsec - a->tv_nsec) / 1000L;
}

static void report(const struct fb_var_screeninfo *var,
		   const struct fb_fix_screeninfo *fix)
{
	printf("fb: %ux%u %ubpp  virtual %ux%u  pan %u,%u\n",
	       var->xres, var->yres, var->bits_per_pixel,
	       var->xres_virtual, var->yres_virtual,
	       var->xoffset, var->yoffset);
	printf("    fix.ypanstep  = %u%s\n", fix->ypanstep,
	       fix->ypanstep ? "" : "   <-- 0 blocks ALL panning in the fbdev core");
	printf("    fix.ywrapstep = %u\n", fix->ywrapstep);
	printf("    fix.smem_len  = %u\n", fix->smem_len);
	printf("    fix.line_length = %u\n", fix->line_length);

	{
		unsigned long need = (unsigned long)var->xres *
				     var->yres * (var->bits_per_pixel / 8);
		printf("    one buffer = %lu bytes, two = %lu, smem_len = %u -> %s\n",
		       need, need * 2, fix->smem_len,
		       need * 2 <= fix->smem_len ? "double buffering fits"
						 : "DOES NOT FIT");
	}
}

/* Claim a second buffer. Returns 0 on success. */
static int claim_second_buffer(int fd, struct fb_var_screeninfo *var)
{
	struct fb_var_screeninfo want = *var;

	want.yres_virtual = var->yres * 2;
	want.xres_virtual = var->xres;
	want.xoffset = 0;
	want.yoffset = 0;
	want.activate = FB_ACTIVATE_NOW;

	if (ioctl(fd, FBIOPUT_VSCREENINFO, &want) < 0) {
		printf("FBIOPUT_VSCREENINFO(yres_virtual=%u): FAILED (%s)\n",
		       var->yres * 2, strerror(errno));
		return -1;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &want) < 0)
		return -1;

	if (want.yres_virtual < var->yres * 2) {
		printf("FBIOPUT_VSCREENINFO accepted but yres_virtual is still %u"
		       " -- the driver clamped it\n", want.yres_virtual);
		return -1;
	}

	printf("FBIOPUT_VSCREENINFO(yres_virtual=%u): OK\n", want.yres_virtual);
	*var = want;
	return 0;
}

static int do_pan(int fd, struct fb_var_screeninfo *var, unsigned int yoffset)
{
	struct fb_var_screeninfo p = *var;

	p.xoffset = 0;
	p.yoffset = yoffset;
	p.activate = FB_ACTIVATE_NOW;

	if (ioctl(fd, FBIOPAN_DISPLAY, &p) < 0)
		return -errno;
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "info";
	int iterations = argc > 2 ? atoi(argv[2]) : 60;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int fd, i, rc;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/fb0");
		return 1;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("FBIOGET_*SCREENINFO");
		return 1;
	}

	report(&var, &fix);

	if (!strcmp(mode, "info"))
		return 0;

	if (!strcmp(mode, "vsync")) {
		for (i = 0; i < iterations; i++) {
			struct timespec t0, t1;
			unsigned int arg = 0;

			clock_gettime(CLOCK_MONOTONIC, &t0);
			rc = ioctl(fd, FBIO_WAITFORVSYNC, &arg);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			printf("vsync %2d: ret=%d %-10s elapsed=%ld us\n",
			       i, rc, rc < 0 ? strerror(errno) : "",
			       elapsed_us(&t0, &t1));
		}
		return 0;
	}

	/* setup / flip both need the second buffer */
	if (claim_second_buffer(fd, &var) < 0) {
		printf("\nCannot double-buffer. If fix.ypanstep is 0 above, that is\n"
		       "the reason and it is a driver bug, not a userspace one.\n");
		return 1;
	}
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("FBIOGET_FSCREENINFO");
		return 1;
	}
	report(&var, &fix);

	/* A pan to yoffset=0 is a no-op; the real test is a nonzero offset. */
	rc = do_pan(fd, &var, var.yres);
	if (rc < 0) {
		printf("FBIOPAN_DISPLAY(yoffset=%u): FAILED (%s)\n",
		       var.yres, strerror(-rc));
		return 1;
	}
	printf("FBIOPAN_DISPLAY(yoffset=%u): OK\n", var.yres);

	if (!strcmp(mode, "setup")) {
		do_pan(fd, &var, 0);
		return 0;
	}

	/* flip: fill both buffers with solid colours and alternate */
	{
		size_t buf_bytes = (size_t)fix.line_length * var.yres;
		size_t total = buf_bytes * 2;
		unsigned short *fb;
		unsigned int n;
		long worst = 0, total_us = 0;

		fb = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (fb == MAP_FAILED) {
			perror("mmap");
			return 1;
		}

		/* RGB565: red = 0xF800, blue = 0x001F */
		for (n = 0; n < buf_bytes / 2; n++)
			fb[n] = 0xF800;
		for (n = 0; n < buf_bytes / 2; n++)
			fb[buf_bytes / 2 + n] = 0x001F;

		printf("\nflipping %d times -- watch for a VERTICAL seam\n",
		       iterations);
		for (i = 0; i < iterations; i++) {
			struct timespec t0, t1;
			long us;

			clock_gettime(CLOCK_MONOTONIC, &t0);
			rc = do_pan(fd, &var, (i & 1) ? 0 : var.yres);
			clock_gettime(CLOCK_MONOTONIC, &t1);

			if (rc < 0) {
				printf("flip %d FAILED: %s\n", i, strerror(-rc));
				break;
			}
			us = elapsed_us(&t0, &t1);
			total_us += us;
			if (us > worst)
				worst = us;
			usleep(60000);
		}
		printf("flips done: mean %ld us, worst %ld us\n",
		       i ? total_us / i : 0, worst);
		printf("(a blocking flip costs ~a frame, ~39000 us; a hardware\n"
		       " latch should return in tens of us)\n");

		munmap(fb, total);
		do_pan(fd, &var, 0);
	}

	return 0;
}
