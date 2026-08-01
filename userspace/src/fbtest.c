/*
 * fbtest -- draw a timing test pattern straight into /dev/fb0.
 *
 * Purpose: separate "the pixels are wrong" from "the panel shows them
 * wrong". fbgrab proves what is in memory; this proves what the W100 and
 * the LCD do with it, because it bypasses X entirely -- nothing but this
 * program touches the framebuffer.
 *
 * Patterns:
 *   hlines  isolated 1px horizontal lines, widely spaced, with an 8px
 *           tick ruler beside each -- for measuring a VERTICAL ghost
 *           offset (how far the doubled copy sits from the original).
 *   vlines  the same rotated 90 degrees -- a control. If hlines ghost
 *           but vlines do not, the fault is in vertical/line timing.
 *   pairs   line pairs separated by 1,2,3,4,6,8,12,16px. The separation
 *           at which the ghost stops being distinguishable brackets the
 *           offset without anyone having to eyeball a distance.
 *   blocks  solid white/black/grey rectangles -- shows edge bleed and
 *           whether the ghost is contrast- or edge-driven.
 *
 *   fbtest [hlines|vlines|pairs|blocks] [device]
 *
 * X only repaints damaged regions, so a pattern drawn under a running
 * Xfbdev survives until something dirties that area.
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

static unsigned char *fb;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;

#define RGB565(r, g, b) \
    ((unsigned short)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static void
put(int x, int y, unsigned short c)
{
    if (x < 0 || y < 0 || x >= (int)var.xres || y >= (int)var.yres)
        return;
    *(unsigned short *)(fb + (size_t)y * fix.line_length + (size_t)x * 2) = c;
}

static void
fill(int x0, int y0, int w, int h, unsigned short c)
{
    int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            put(x, y, c);
}

static void
hline(int y, unsigned short c)
{
    fill(0, y, var.xres, 1, c);
}

static void
vline(int x, unsigned short c)
{
    fill(x, 0, 1, var.yres, c);
}

int
main(int argc, char **argv)
{
    const char *pat = (argc > 1) ? argv[1] : "hlines";
    const char *dev = (argc > 2) ? argv[2] : "/dev/fb0";
    unsigned short WHITE = RGB565(255, 255, 255);
    unsigned short BLACK = RGB565(0, 0, 0);
    unsigned short GREY  = RGB565(128, 128, 128);
    size_t maplen;
    int fd, i, y, x;

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "fbtest: open %s: %s\n", dev, strerror(errno));
        return 1;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        fprintf(stderr, "fbtest: ioctl: %s\n", strerror(errno));
        return 1;
    }
    if (var.bits_per_pixel != 16) {
        fprintf(stderr, "fbtest: only 16bpp supported (got %u)\n",
                var.bits_per_pixel);
        return 1;
    }

    maplen = (size_t)fix.line_length * (var.yres + var.yoffset);
    fb = mmap(NULL, maplen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        fprintf(stderr, "fbtest: mmap: %s\n", strerror(errno));
        return 1;
    }
    fb += (size_t)var.yoffset * fix.line_length;

    fill(0, 0, var.xres, var.yres, BLACK);

    if (!strcmp(pat, "hlines")) {
        /* Isolated single-pixel rows, far apart so a ghost cannot be
         * confused with a neighbour. Ruler ticks every 8px down the
         * left edge beside each line, to gauge the offset. */
        for (i = 1; i <= 5; i++) {
            y = (int)var.yres * i / 6;
            hline(y, WHITE);
            for (x = 0; x < 8; x++) {
                fill(0, y - 32 + x * 8, 12, 1, GREY);
                fill(0, y + 8 + x * 8, 12, 1, GREY);
            }
        }
    } else if (!strcmp(pat, "vlines")) {
        for (i = 1; i <= 5; i++) {
            x = (int)var.xres * i / 6;
            vline(x, WHITE);
        }
    } else if (!strcmp(pat, "pairs")) {
        /* Second line of each pair steps away by a known gap. */
        int gaps[] = { 1, 2, 3, 4, 6, 8, 12, 16 };
        int n = (int)(sizeof(gaps) / sizeof(gaps[0]));
        for (i = 0; i < n; i++) {
            y = 40 + i * ((int)var.yres - 80) / n;
            hline(y, WHITE);
            hline(y + gaps[i], WHITE);
            /* gap size in tick marks at the right edge */
            fill((int)var.xres - 4 - gaps[i] * 4, y - 12, gaps[i] * 4, 4, GREY);
        }
    } else if (!strcmp(pat, "blocks")) {
        fill(40, 40, 160, 120, WHITE);
        fill(240, 40, 160, 120, GREY);
        fill(440, 40, 160, 120, WHITE);
        fill(40, 220, 560, 60, WHITE);
        fill(40, 320, 560, 4, WHITE);
        fill(40, 360, 560, 2, WHITE);
        fill(40, 400, 560, 1, WHITE);
    } else {
        fprintf(stderr, "fbtest: unknown pattern '%s'\n", pat);
        return 1;
    }

    fprintf(stderr, "fbtest: drew '%s' on %ux%u\n", pat, var.xres, var.yres);
    return 0;
}
