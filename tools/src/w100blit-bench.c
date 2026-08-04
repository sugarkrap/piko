/*
 * w100blit-bench -- is the w100's 2D engine actually faster than the CPU
 * at getting a sprite onto the screen, and is keeping sprites in spare
 * VRAM worth it?
 *
 * The ioctls from PR #118 (W100FB_IOC_FILL/BLIT/SYNC) are verified
 * *correct* by tools/src/w100accel-test.c. Correct is not the same as
 * worth using: an ioctl has syscall overhead the CPU path does not, the
 * framebuffer is write-combined (see the w100fb_mmap() comment in the
 * driver), and VRAM is off-chip. Whether the engine wins, and above which
 * sprite size, is an empirical question about this specific board. This
 * measures it.
 *
 * Build (static, same as the other tools here -- the rootfs has no
 * dynamic linker):
 *   arm-unknown-linux-uclibcgnueabi-gcc -march=armv5te -O2 -static \
 *       -Wall -Wextra -o w100blit-bench tools/src/w100blit-bench.c
 *
 * FIVE PATHS, because "engine vs CPU" on its own would be misleading:
 *
 *   A  ram->fb   cpu     memcpy from a cached malloc'd sprite into the
 *                        visible framebuffer. THE STATUS QUO -- what any
 *                        fbdev program on this device does today.
 *   B  vram->fb  cpu     same memcpy, but the sprite lives in spare VRAM.
 *                        Included because "store sprites in VRAM" and "let
 *                        the engine composite them" are two separate
 *                        decisions, and this is what the first costs
 *                        without the second: the CPU now READS off-chip
 *                        write-combined memory, which is the slow
 *                        direction.
 *   C  vram->fb  blit    W100FB_IOC_BLIT + W100FB_IOC_SYNC every
 *                        iteration. The honest per-sprite latency if you
 *                        need the result immediately.
 *   D  vram->fb  blit*   N blits, ONE sync at the end. What a real
 *                        compositor does -- queue the frame's sprites,
 *                        sync once before flipping. Separating C and D
 *                        shows how much of the cost is the sync itself.
 *   E  ram->vram cpu     uploading a sprite into VRAM in the first place.
 *                        A one-time cost per sprite, but it has to be
 *                        amortised before C/D mean anything -- if E is
 *                        huge and sprites change every frame, the engine
 *                        never gets to pay it back.
 *
 * The answer to "should PocketSNES do this" is A vs D, with E as the
 * entry fee. C-vs-D and B are there so a surprising A-vs-D result can be
 * attributed rather than guessed at.
 *
 * NOTE it draws over whatever is on screen and does not restore it --
 * same as tools/src/fbflip.c. Nothing is corrupted, it just looks like a
 * mess until something repaints.
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

#include "../../modules/w100/w100fb_accel.h"

#define RGB565_MAGENTA	0xF81F

/* Aim for this much copied per measurement, then clamp the iteration
 * count: enough work that clock_gettime's resolution and one-off cache
 * effects don't dominate, few enough that an 8x8 sprite doesn't spend a
 * minute doing syscalls on a 400MHz PXA255. */
#define TARGET_BYTES	(2UL * 1024 * 1024)
#define MIN_ITERS	32UL
#define MAX_ITERS	4000UL

static int g_fd = -1;

struct result {
	double us_per_op;
	double mb_per_s;
};

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}

static unsigned long iters_for(unsigned long bytes)
{
	unsigned long n = bytes ? TARGET_BYTES / bytes : MAX_ITERS;

	if (n < MIN_ITERS)
		n = MIN_ITERS;
	if (n > MAX_ITERS)
		n = MAX_ITERS;
	return n;
}

/* Row-wise copy between two 16bpp surfaces of arbitrary pitch. This is
 * deliberately a plain memcpy per row rather than anything clever: it is
 * what the status-quo path in a fbdev program looks like. */
static void copy_rect(unsigned char *dst, unsigned long dst_pitch,
		       const unsigned char *src, unsigned long src_pitch,
		       unsigned w, unsigned h)
{
	unsigned y;

	for (y = 0; y < h; y++)
		memcpy(dst + (unsigned long)y * dst_pitch,
		       src + (unsigned long)y * src_pitch,
		       (size_t)w * 2);
}

static struct result bench_cpu(unsigned char *dst, unsigned long dst_pitch,
				const unsigned char *src, unsigned long src_pitch,
				unsigned w, unsigned h)
{
	unsigned long bytes = (unsigned long)w * h * 2;
	unsigned long iters = iters_for(bytes), i;
	struct result r;
	double t0, dt;

	copy_rect(dst, dst_pitch, src, src_pitch, w, h);   /* warm the path */

	t0 = now_us();
	for (i = 0; i < iters; i++)
		copy_rect(dst, dst_pitch, src, src_pitch, w, h);
	dt = now_us() - t0;

	r.us_per_op = dt / (double)iters;
	r.mb_per_s = (double)bytes * iters / dt;   /* bytes/us == MB/s */
	return r;
}

static struct result bench_blit(const struct w100fb_blit_args *proto,
				 unsigned w, unsigned h, int sync_every)
{
	unsigned long bytes = (unsigned long)w * h * 2;
	unsigned long iters = iters_for(bytes), i;
	struct w100fb_blit_args blit = *proto;
	struct result r;
	double t0, dt;

	ioctl(g_fd, W100FB_IOC_BLIT, &blit);               /* warm the path */
	ioctl(g_fd, W100FB_IOC_SYNC);

	t0 = now_us();
	for (i = 0; i < iters; i++) {
		if (ioctl(g_fd, W100FB_IOC_BLIT, &blit) < 0) {
			fprintf(stderr, "W100FB_IOC_BLIT: %s\n", strerror(errno));
			r.us_per_op = r.mb_per_s = 0;
			return r;
		}
		if (sync_every)
			ioctl(g_fd, W100FB_IOC_SYNC);
	}
	/* Always sync before stopping the clock, batched or not: without it
	 * the batched number would be "how fast can we queue work", not "how
	 * fast is the work done", and would look absurdly good. */
	ioctl(g_fd, W100FB_IOC_SYNC);
	dt = now_us() - t0;

	r.us_per_op = dt / (double)iters;
	r.mb_per_s = (double)bytes * iters / dt;
	return r;
}

int main(int argc, char **argv)
{
	static const unsigned sizes[] = { 8, 16, 32, 64, 128, 256 };
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	struct w100fb_blit_args blit;
	unsigned char *fb, *ram;
	unsigned long spare_off;
	unsigned s;
	const char *dev = (argc > 1) ? argv[1] : "/dev/fb0";
	/* Optional: force where the sprite lives, instead of "right after the
	 * virtual buffer". The default lands wherever the current mode leaves
	 * it, and that is NOT one consistent kind of memory -- at QVGA it is
	 * below the 393216-byte internal-SRAM threshold, at VGA it is well
	 * past it in external SDRAM. Being able to pin the offset is what
	 * separates "the mode is slower" from "that memory is slower". */
	unsigned long spare_override = (argc > 2) ? strtoul(argv[2], NULL, 0) : 0;

	g_fd = open(dev, O_RDWR);
	if (g_fd < 0) {
		perror(dev);
		return 1;
	}
	if (ioctl(g_fd, FBIOGET_VSCREENINFO, &var) < 0 ||
	    ioctl(g_fd, FBIOGET_FSCREENINFO, &fix) < 0) {
		perror("FBIOGET_*SCREENINFO");
		return 1;
	}
	if (ioctl(g_fd, W100FB_IOC_SYNC) < 0) {
		fprintf(stderr, "W100FB_IOC_SYNC: %s -- this kernel has no "
			"accel ioctls; run w100accel-test probe\n", strerror(errno));
		return 1;
	}

	/* Spare VRAM starts right after the whole virtual buffer, so this
	 * never lands on the visible frame or on a page-flip back buffer. */
	spare_off = (unsigned long)fix.line_length * var.yres_virtual;
	if (spare_override) {
		if (spare_override < (unsigned long)fix.line_length * var.yres_virtual) {
			fprintf(stderr, "spare offset %lu would overlap the "
				"virtual buffer (%lu bytes)\n", spare_override,
				(unsigned long)fix.line_length * var.yres_virtual);
			return 1;
		}
		spare_off = spare_override;
	}

	printf("fb %s: %ux%u visible, %ux%u virtual, %u bpp, line_length %u\n",
	       fix.id, var.xres, var.yres, var.xres_virtual, var.yres_virtual,
	       var.bits_per_pixel, fix.line_length);
	printf("smem_len %u, spare VRAM starts at %lu (%lu bytes free)\n\n",
	       fix.smem_len, spare_off,
	       (unsigned long)fix.smem_len > spare_off ?
			(unsigned long)fix.smem_len - spare_off : 0UL);

	if (var.bits_per_pixel != 16) {
		fprintf(stderr, "this benchmark assumes 16bpp RGB565\n");
		return 1;
	}

	fb = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	printf("%-6s %10s %10s %10s %10s %10s\n",
	       "size", "A ram>fb", "B vram>fb", "C blit+sy", "D blit bat", "E ram>vram");
	printf("%-6s %10s %10s %10s %10s %10s\n",
	       "", "us (MB/s)", "us (MB/s)", "us (MB/s)", "us (MB/s)", "us (MB/s)");

	for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		unsigned w = sizes[s], h = sizes[s];
		unsigned long sprite_bytes = (unsigned long)w * h * 2;
		unsigned long need = spare_off + (unsigned long)fix.line_length * h;
		struct result a, b, c, d, e;

		/* The engine clips every rect to the CURRENT mode's
		 * [0,0]-[xres,yres] whatever surface it targets, so a sprite
		 * bigger than the visible mode cannot be expressed at all. */
		if (w > var.xres || h > var.yres) {
			printf("%-6u  (skipped: larger than the %ux%u visible mode)\n",
			       w, var.xres, var.yres);
			continue;
		}
		if (need > fix.smem_len) {
			printf("%-6u  (skipped: needs %lu bytes, smem_len is %u)\n",
			       w, need, fix.smem_len);
			continue;
		}

		ram = malloc(sprite_bytes);
		if (!ram) {
			fprintf(stderr, "out of memory for a %lu-byte sprite\n",
				sprite_bytes);
			break;
		}
		memset(ram, 0x5a, sprite_bytes);

		/* Put the same sprite in spare VRAM for paths B, C and D. */
		copy_rect(fb + spare_off, fix.line_length, ram,
			  (unsigned long)w * 2, w, h);

		/* A: the status quo. Destination is the top-left of the
		 * visible frame for every path, so they are comparable. */
		a = bench_cpu(fb, fix.line_length, ram, (unsigned long)w * 2, w, h);

		/* B: sprite in VRAM, CPU still doing the copy. */
		b = bench_cpu(fb, fix.line_length, fb + spare_off,
			      fix.line_length, w, h);

		memset(&blit, 0, sizeof(blit));
		blit.src_offset = (unsigned int)spare_off;
		blit.src_pitch = fix.line_length;
		blit.dst_offset = 0;
		blit.dst_pitch = fix.line_length;
		blit.sx = 0; blit.sy = 0;
		blit.dx = 0; blit.dy = 0;
		blit.width = w; blit.height = h;

		c = bench_blit(&blit, w, h, 1);
		d = bench_blit(&blit, w, h, 0);

		/* E: the entry fee -- getting a sprite into VRAM at all. */
		e = bench_cpu(fb + spare_off, fix.line_length, ram,
			      (unsigned long)w * 2, w, h);

		printf("%-6u %5.0f(%4.0f) %5.0f(%4.0f) %5.0f(%4.0f) "
		       "%5.0f(%4.0f) %5.0f(%4.0f)\n",
		       w,
		       a.us_per_op, a.mb_per_s, b.us_per_op, b.mb_per_s,
		       c.us_per_op, c.mb_per_s, d.us_per_op, d.mb_per_s,
		       e.us_per_op, e.mb_per_s);
		fflush(stdout);

		free(ram);
	}

	printf("\nA is the status quo. The engine is worth it where D < A, and\n"
	       "only for sprites reused enough times to pay off E.\n");

	munmap(fb, fix.smem_len);
	close(g_fd);
	return 0;
}
