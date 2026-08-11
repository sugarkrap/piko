
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
