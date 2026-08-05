/*
 * setmode -- set an exact fb_var_screeninfo geometry via FBIOPUT_VSCREENINFO,
 * with no ambiguity about what xoffset/yoffset/virtual size get sent.
 *
 * Written for the w100 clock-domain bring-up (2026-08-05), see
 * docs/DEADLETTER-W100-CLOCK-DOMAINS.md. `fbset -g` reads the CURRENT var via
 * FBIOGET first and only overwrites the fields you pass, so a stale
 * yoffset/yres_virtual left over from a prior double-buffered mode (e.g. the
 * desktop's own 640x960 virtual VGA mode) can make FBIOPUT_VSCREENINFO fail
 * with -EINVAL for reasons that have nothing to do with the geometry you
 * actually asked for -- see w100fb_check_var()'s
 * `var->yoffset + var->yres > var->yres_virtual` check. This tool always
 * sends xoffset=yoffset=0 and an explicit xres_virtual=xres, so the only
 * unknowns are the ones you pass on the command line.
 *
 * Also the tool for the "switch to QVGA and back" protocol
 * docs/DEADLETTER-W100-CLOCK-DOMAINS.md and the w100 clock-domain handoff
 * both depend on: dropping to a small enough resolution powers external
 * SDRAM off entirely (w100_setup_memory()'s !extmem_active branch), which is
 * what makes a QVGA excursion safe to test PLL/pixclk changes in without
 * external-memory risk. Confirm that actually happened with
 * `w100accel-test info`'s fix.smem_len -- do not assume it from the
 * resolution alone (see the docs/DEADLETTER-W100-CLOCK-DOMAINS.md bug entry).
 *
 * Build (the rootfs ships no dynamic linker -- static is mandatory, same as
 * tools/src/fbflip.c and tools/src/w100accel-test.c):
 *   arm-unknown-linux-uclibcgnueabi-gcc -march=armv5te -mfloat-abi=soft -O2 \
 *       -static -Wall -Wextra -o setmode tools/src/setmode.c
 *
 * Usage:
 *   setmode xres yres [yres_virtual]
 *
 * yres_virtual defaults to yres (no spare pannable buffer, smallest possible
 * `needed` in w100fb_set_par() -- what you want for the QVGA-off-memory
 * trick). Pass the desktop's own value explicitly to restore double-buffered
 * panning, e.g. `setmode 640 480 960` to return to the stock VGA desktop
 * geometry after a QVGA excursion.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct fb_var_screeninfo var;
	int fd;
	unsigned int xres, yres, yres_virtual;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s xres yres [yres_virtual]\n", argv[0]);
		return 1;
	}
	xres = (unsigned int)atoi(argv[1]);
	yres = (unsigned int)atoi(argv[2]);
	yres_virtual = argc == 4 ? (unsigned int)atoi(argv[3]) : yres;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open /dev/fb0 failed: %s\n", strerror(errno));
		return 1;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
		fprintf(stderr, "FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	var.xres = xres;
	var.yres = yres;
	var.xres_virtual = xres;
	var.yres_virtual = yres_virtual;
	var.xoffset = 0;
	var.yoffset = 0;
	var.bits_per_pixel = 16;

	if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) != 0) {
		fprintf(stderr, "FBIOPUT_VSCREENINFO failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0) {
		fprintf(stderr, "FBIOGET_VSCREENINFO (verify) failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("ok: xres=%u yres=%u xres_virtual=%u yres_virtual=%u bpp=%u\n",
	       var.xres, var.yres, var.xres_virtual, var.yres_virtual, var.bits_per_pixel);

	close(fd);
	return 0;
}
