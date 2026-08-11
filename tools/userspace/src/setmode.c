
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
