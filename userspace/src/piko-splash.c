/*
 * piko-splash -- blit a raw RGB565 image straight into /dev/fb0.
 *
 * This is the stage-2 half of the boot splash. Stage 1 (the bootstrap
 * initramfs) draws the same picture with busybox's fbsplash applet; stage 2
 * cannot, for two independent reasons:
 *
 *   - this rootfs's busybox is prebuilt in the mtd3 image and has no
 *     fbsplash applet, and it is not built from this repo, so enabling one
 *     is not a config change we can make here;
 *   - it also has no gzip/gunzip/zcat, only cat and dd.
 *
 * The obvious workaround -- pre-render raw RGB565 and `cat` it to
 * /dev/fb0 -- does NOT work on this hardware. It fails with EINVAL:
 *
 *     cat: write error: Invalid argument
 *
 * w100fb does not provide a usable write() path; the framebuffer has to be
 * mmap'd. (That is also why fbsplash works in stage 1: it mmaps.) Hence
 * this program, which is the smallest thing that can do the job -- open,
 * mmap, memcpy row by row, done.
 *
 * The image file is a headerless dump of exactly the visible screen, as
 * produced by tools/make-splash.py --raw-out: width*height*2 bytes,
 * little-endian RGB565, no palette, no metadata. Headerless on purpose --
 * there is nothing to parse and therefore nothing to get wrong at boot,
 * and the geometry is validated against the framebuffer here instead.
 *
 * Rows are copied one at a time rather than in a single memcpy because
 * fix.line_length is not required to equal xres*2; copying per row is
 * correct whether or not the driver pads its stride. var.yoffset is honoured
 * for the same reason fbtest.c honours it: this panel reports a doubled
 * virtual yres (640x960 for a 640x480 screen) and the visible region can sit
 * at a non-zero pan offset.
 *
 * Usage:
 *   piko-splash [-d /dev/fb0] <image.raw>
 *
 * Exit codes:
 *   0  drawn
 *   1  could not draw (device missing, geometry mismatch, short file)
 *
 * Callers should treat a non-zero exit as cosmetic. Failing to draw a
 * picture must never stop this machine from booting.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fb.h>

int main(int argc, char **argv)
{
    const char *dev = "/dev/fb0";
    const char *path = NULL;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    unsigned char *fb, *row;
    size_t maplen, rowbytes;
    unsigned y;
    int fd, imgfd, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            dev = argv[++i];
        else if (argv[i][0] != '-' && path == NULL)
            path = argv[i];
        else {
            fprintf(stderr, "usage: %s [-d /dev/fb0] <image.raw>\n", argv[0]);
            return 1;
        }
    }
    if (path == NULL) {
        fprintf(stderr, "usage: %s [-d /dev/fb0] <image.raw>\n", argv[0]);
        return 1;
    }

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "piko-splash: %s: %s\n", dev, strerror(errno));
        return 1;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        fprintf(stderr, "piko-splash: FBIOGET_*SCREENINFO: %s\n",
                strerror(errno));
        close(fd);
        return 1;
    }
    if (var.bits_per_pixel != 16) {
        fprintf(stderr, "piko-splash: need 16bpp, panel reports %u\n",
                var.bits_per_pixel);
        close(fd);
        return 1;
    }

    imgfd = open(path, O_RDONLY);
    if (imgfd < 0) {
        fprintf(stderr, "piko-splash: %s: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }

    rowbytes = (size_t)var.xres * 2;

    maplen = (size_t)fix.line_length * (var.yres + var.yoffset);
    fb = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        fprintf(stderr, "piko-splash: mmap: %s\n", strerror(errno));
        close(imgfd);
        close(fd);
        return 1;
    }
    fb += (size_t)var.yoffset * fix.line_length;

    row = malloc(rowbytes);
    if (row == NULL) {
        fprintf(stderr, "piko-splash: out of memory\n");
        munmap(fb - (size_t)var.yoffset * fix.line_length, maplen);
        close(imgfd);
        close(fd);
        return 1;
    }

    /* A short file is a mismatched asset, not a partial picture: draw
     * nothing rather than half a splash with garbage under it. */
    for (y = 0; y < var.yres; y++) {
        size_t got = 0;
        while (got < rowbytes) {
            ssize_t n = read(imgfd, row + got, rowbytes - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        if (got != rowbytes) {
            fprintf(stderr, "piko-splash: %s is short at row %u "
                    "(expected %ux%u)\n", path, y, var.xres, var.yres);
            free(row);
            munmap(fb - (size_t)var.yoffset * fix.line_length, maplen);
            close(imgfd);
            close(fd);
            return 1;
        }
        memcpy(fb + (size_t)y * fix.line_length, row, rowbytes);
    }

    free(row);
    munmap(fb - (size_t)var.yoffset * fix.line_length, maplen);
    close(imgfd);
    close(fd);
    return 0;
}
