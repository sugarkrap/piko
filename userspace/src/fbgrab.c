/*
 * fbgrab -- dump the raw framebuffer to stdout.
 *
 * The device's /dev/fb0 (pxafb) does not support read(2) -- dd returns
 * EINVAL -- so the only way to get the screen contents off the board is
 * to mmap it. This exists because "take a photo of the screen" is not a
 * usable debugging channel for pixel-level problems.
 *
 * Output is the raw visible framebuffer, line_length stripped down to
 * xres*bpp/8 bytes per row, so the host side only needs to know
 * width/height/bpp -- which are printed to stderr.
 *
 *   fbgrab [device] > screen.raw
 *
 * Decode with tools/decode-fb.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

int
main(int argc, char **argv)
{
    const char *dev = (argc > 1) ? argv[1] : "/dev/fb0";
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    unsigned char *fb;
    size_t maplen;
    unsigned int y, rowbytes;
    int fd;

    fd = open(dev, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "fbgrab: open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        fprintf(stderr, "fbgrab: ioctl: %s\n", strerror(errno));
        return 1;
    }

    rowbytes = var.xres * var.bits_per_pixel / 8;

    /* Map from the start of the page containing the visible origin. */
    maplen = (size_t)fix.line_length * (var.yres + var.yoffset);

    fb = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        fprintf(stderr, "fbgrab: mmap %lu bytes: %s\n",
                (unsigned long)maplen, strerror(errno));
        return 1;
    }

    fprintf(stderr, "fbgrab: %ux%u %ubpp line_length=%u offset=+%u+%u\n",
            var.xres, var.yres, var.bits_per_pixel,
            fix.line_length, var.xoffset, var.yoffset);

    for (y = 0; y < var.yres; y++) {
        unsigned char *row = fb + (size_t)(y + var.yoffset) * fix.line_length
                                + (size_t)var.xoffset * var.bits_per_pixel / 8;
        size_t left = rowbytes;
        while (left) {
            ssize_t n = write(STDOUT_FILENO, row, left);
            if (n <= 0) {
                if (n < 0 && errno == EINTR)
                    continue;
                fprintf(stderr, "fbgrab: write: %s\n", strerror(errno));
                return 1;
            }
            row += n;
            left -= (size_t)n;
        }
    }

    munmap(fb, maplen);
    close(fd);
    return 0;
}
