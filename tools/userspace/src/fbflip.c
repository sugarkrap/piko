
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

#define W100_REGS_PHYS	0x08010000UL
#define REG_GRAPHIC_OFFSET	0x0418
#define REG_DISP_DB_BUF_CNTL	0x04D8

static volatile unsigned char *regs;

static int regs_open(void)
{
	int mfd = open("/dev/mem", O_RDONLY | O_SYNC);
	void *p;

	if (mfd < 0)
		return -1;
	p = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, mfd, W100_REGS_PHYS);
	close(mfd);
	if (p == MAP_FAILED)
		return -1;
	regs = p;
	return 0;
}

static unsigned int reg_rd(unsigned int off)
{
	return *(volatile unsigned int *)(regs + off);
}

static void dump_regs(const char *when)
{
	if (!regs)
		return;
	printf("    [%s] GRAPHIC_OFFSET=%#010x  DISP_DB_BUF_CNTL=%#010x\n",
	       when, reg_rd(REG_GRAPHIC_OFFSET), reg_rd(REG_DISP_DB_BUF_CNTL));
}

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

	if (!strcmp(mode, "buffers")) {
		size_t buf_bytes = (size_t)fix.line_length * var.yres;
		unsigned char *fb;
		size_t i2, diff = 0;
		unsigned long sum0 = 0, sum1 = 0;

		if (var.yres_virtual < var.yres * 2) {
			printf("not double buffered (yres_virtual=%u)\n",
			       var.yres_virtual);
			return 1;
		}
		fb = mmap(NULL, buf_bytes * 2, PROT_READ, MAP_SHARED, fd, 0);
		if (fb == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		for (i2 = 0; i2 < buf_bytes; i2++) {
			unsigned char a = fb[i2], b = fb[buf_bytes + i2];

			sum0 += a;
			sum1 += b;
			if (a != b)
				diff++;
		}
		printf("buffer0 checksum %lu, buffer1 checksum %lu\n", sum0, sum1);
		printf("bytes differing: %lu of %lu (%.2f%%)\n",
		       (unsigned long)diff, (unsigned long)buf_bytes,
		       100.0 * diff / buf_bytes);
		printf("%s\n", diff * 100 / buf_bytes < 2
		       ? "buffers agree -- damage bookkeeping is converging"
		       : "buffers DIVERGE -- expect flicker between two pictures");
		munmap(fb, buf_bytes * 2);
		return 0;
	}

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

	if (!strcmp(mode, "hold")) {
		int secs = argc > 2 ? atoi(argv[2]) : 5;
		size_t buf_bytes = (size_t)fix.line_length * var.yres;
		size_t total = buf_bytes * 2;
		unsigned short *fb;
		unsigned int n;

		fb = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (fb == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
		for (n = 0; n < buf_bytes / 2; n++)
			fb[buf_bytes / 2 + n] = 0x001F;

		if (regs_open() < 0)
			printf("(no /dev/mem access -- register peek disabled)\n");

		printf("\npanning to buffer 1 (solid blue) for %d s...\n", secs);
		dump_regs("before pan");
		fflush(stdout);
		if (do_pan(fd, &var, var.yres) < 0) {
			printf("pan FAILED\n");
			munmap(fb, total);
			return 1;
		}
		dump_regs("just after pan");
		sleep(secs);
		dump_regs("after holding");
		printf("panning back to buffer 0 (the desktop)\n");
		do_pan(fd, &var, 0);
		usleep(100000);
		dump_regs("after panning back");
		printf("\nexpected for this 90-degree rotated mode:\n"
		       "  yoffset=0   -> GRAPHIC_OFFSET = 0x00895b00\n"
		       "  yoffset=480 -> GRAPHIC_OFFSET = 0x0092bb00\n"
		       "if the value does not move, the ioctl succeeded but the\n"
		       "flip did not (shadow bank never promoted).\n");
		munmap(fb, total);
		return 0;
	}

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
