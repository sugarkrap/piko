/*
 * pikalibrate -- on-device touchscreen calibration for the Zaurus.
 *
 * Shows 4 crosses in sequence (corners, inset so taps land inside the
 * panel's actually-reachable area -- see docs/HOWTO-X11-TOUCHSCREEN.md),
 * reads raw ads7846 events directly (bypassing SDL's own input layer,
 * which reads /dev/input/mice, not this device), and writes the
 * resulting XMIN/XMAX/YMIN/YMAX to /etc/piko/touchscreen.cfg -- read at
 * startup (and on reload) by the patched kdrive evdev driver in
 * userspace/src/xserver/hw/kdrive/linux/evdev.c.
 *
 * If a live Xfbdev is running, it holds the touchscreen open with an
 * exclusive EVIOCGRAB, so this can't just open the device and expect
 * real events. Xfbdev listens on a small FIFO (/tmp/.pikalibrate-ctl,
 * see hw/kdrive/linux/linux.c's Pikalibrate control channel) for
 * "SUSPEND"/"RESUME" commands that map onto its existing
 * KdSuspend()/KdResume() -- the same suspend/resume it already uses for
 * real APM events -- so the desktop briefly freezes and reappears
 * (freshly repainted) around the calibration, with no session restart.
 * If the FIFO doesn't exist, no X server is running (e.g. first boot,
 * before xsession starts) and the touchscreen is simply free already.
 *
 * Usage: pikalibrate
 * Exit codes: 0 ok, 1 on any failure (message on stderr).
 */

#include <SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TOUCHSCREEN_CFG     "/etc/piko/touchscreen.cfg"
#define TOUCHSCREEN_CFG_TMP "/etc/piko/touchscreen.cfg.new"
#define PIKALIBRATE_FIFO    "/tmp/.pikalibrate-ctl"
#define TOUCHSCREEN_NAME    "ADS7846 Touchscreen"
#define FALLBACK_DEVICE     "/dev/input/event2"

#define SCREEN_W    640
#define SCREEN_H    480
#define CROSS_INSET 60
#define CROSS_ARM   12
#define CROSS_THICK 3
#define GRAB_TIMEOUT_MS 2000

typedef struct { int x, y; } Point;

static int ctl_fd = -1;

/* Scans /sys/class/input/eventN/device/name for the ads7846 kernel
 * device name, since /dev/input/eventN numbering is pure enumeration
 * order (see docs/HOWTO-X11-TOUCHSCREEN.md) and can shift. */
static const char *
find_touchscreen_device (void)
{
    DIR *d;
    struct dirent *ent;
    static char path[320];
    char namepath[320];
    char namebuf[64];
    FILE *f;

    d = opendir ("/sys/class/input");
    if (!d)
        return FALLBACK_DEVICE;

    while ((ent = readdir (d)) != NULL) {
        if (strncmp (ent->d_name, "event", 5) != 0)
            continue;
        snprintf (namepath, sizeof (namepath), "/sys/class/input/%s/device/name", ent->d_name);
        f = fopen (namepath, "r");
        if (!f)
            continue;
        if (fgets (namebuf, sizeof (namebuf), f)) {
            namebuf[strcspn (namebuf, "\n")] = '\0';
            if (strcmp (namebuf, TOUCHSCREEN_NAME) == 0) {
                fclose (f);
                closedir (d);
                snprintf (path, sizeof (path), "/dev/input/%s", ent->d_name);
                return path;
            }
        }
        fclose (f);
    }
    closedir (d);

    fprintf (stderr, "pikalibrate: no input device named \"%s\" found, falling back to %s\n",
              TOUCHSCREEN_NAME, FALLBACK_DEVICE);
    return FALLBACK_DEVICE;
}

/* Returns 1 if a live Xfbdev answered and has been told to suspend
 * (ctl_fd is then open, to be closed by request_resume()); 0 if there's
 * no FIFO to talk to (no X server running) -- not an error either way. */
static int
request_suspend (void)
{
    ctl_fd = open (PIKALIBRATE_FIFO, O_WRONLY | O_NONBLOCK);
    if (ctl_fd < 0)
        return 0;
    if (write (ctl_fd, "SUSPEND\n", 8) != 8) {
        close (ctl_fd);
        ctl_fd = -1;
        return 0;
    }
    return 1;
}

static void
request_resume (void)
{
    if (ctl_fd < 0)
        return;
    if (write (ctl_fd, "RESUME\n", 7) != 7)
        fprintf (stderr, "pikalibrate: warning: RESUME write failed: %s\n", strerror (errno));
    close (ctl_fd);
    ctl_fd = -1;
}

/* Polls EVIOCGRAB on our own fd until it succeeds (proof Xfbdev's own
 * grab has actually been released -- EVIOCGRAB fails EBUSY while another
 * fd holds it) or times out. We don't need to hold the grab ourselves,
 * just prove nobody else does. */
static int
wait_for_device (int fd, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        if (ioctl (fd, EVIOCGRAB, 1) == 0) {
            ioctl (fd, EVIOCGRAB, 0);
            return 1;
        }
        usleep (50000);
        waited += 50;
    }
    return 0;
}

static void
fill_rect (SDL_Surface *s, int x, int y, int w, int h, Uint32 c)
{
    SDL_Rect r;
    r.x = (Sint16) x;
    r.y = (Sint16) y;
    r.w = (Uint16) w;
    r.h = (Uint16) h;
    SDL_FillRect (s, &r, c);
}

static void
draw_cross (SDL_Surface *screen, int cx, int cy, Uint32 color)
{
    fill_rect (screen, cx - CROSS_ARM, cy - CROSS_THICK / 2, CROSS_ARM * 2 + 1, CROSS_THICK, color);
    fill_rect (screen, cx - CROSS_THICK / 2, cy - CROSS_ARM, CROSS_THICK, CROSS_ARM * 2 + 1, color);
}

/* Blocks until one full tap (BTN_TOUCH down then up) completes, returning
 * the raw ABS_X/ABS_Y averaged over every sample seen while ABS_PRESSURE
 * was nonzero -- pen-up samples carry stale coordinates and would skew
 * the result (same caution as the manual re-calibration procedure this
 * replaces, see docs/HOWTO-X11-TOUCHSCREEN.md). */
static Point
sample_tap (int fd)
{
    struct input_event ev;
    long sumx = 0, sumy = 0;
    int  n = 0, pressure = 0, down = 0;
    Point p;

    for (;;) {
        if (read (fd, &ev, sizeof (ev)) != (int) sizeof (ev))
            continue;

        switch (ev.type) {
        case EV_KEY:
            if (ev.code == BTN_TOUCH) {
                if (ev.value)
                    down = 1;
                else if (down && n > 0)
                    goto done;
                else
                    down = 0;
            }
            break;
        case EV_ABS:
            if (ev.code == ABS_PRESSURE)
                pressure = ev.value;
            else if (pressure > 0 && ev.code == ABS_X)
                sumx += ev.value;
            else if (pressure > 0 && ev.code == ABS_Y) {
                sumy += ev.value;
                n++;
            }
            break;
        }
    }

done:
    p.x = (int) (sumx / n);
    p.y = (int) (sumy / n);
    return p;
}

static int
write_config (int xmin, int xmax, int ymin, int ymax)
{
    FILE *f;

    mkdir ("/etc/piko", 0755); /* ignore EEXIST -- ships tracked, this is a fallback */

    f = fopen (TOUCHSCREEN_CFG_TMP, "w");
    if (!f) {
        fprintf (stderr, "pikalibrate: fopen %s: %s\n", TOUCHSCREEN_CFG_TMP, strerror (errno));
        return -1;
    }
    fprintf (f, "XMIN=%d\nXMAX=%d\nYMIN=%d\nYMAX=%d\n", xmin, xmax, ymin, ymax);
    fclose (f);

    if (rename (TOUCHSCREEN_CFG_TMP, TOUCHSCREEN_CFG) != 0) {
        fprintf (stderr, "pikalibrate: rename to %s: %s\n", TOUCHSCREEN_CFG, strerror (errno));
        return -1;
    }
    return 0;
}

int
main (void)
{
    const char *ts_path;
    int   ts_fd;
    int   using_x;
    SDL_Surface *screen;
    Uint32 black, white;
    Point targets[4];
    Point raw[4];
    int   i;

    ts_path = find_touchscreen_device ();
    fprintf (stderr, "pikalibrate: using touchscreen device %s\n", ts_path);

    using_x = request_suspend ();
    if (using_x)
        fprintf (stderr, "pikalibrate: told Xfbdev to suspend, waiting for the touchscreen...\n");

    ts_fd = open (ts_path, O_RDONLY);
    if (ts_fd < 0) {
        fprintf (stderr, "pikalibrate: open %s: %s\n", ts_path, strerror (errno));
        request_resume ();
        return 1;
    }

    if (using_x && !wait_for_device (ts_fd, GRAB_TIMEOUT_MS)) {
        fprintf (stderr, "pikalibrate: timed out waiting for Xfbdev to release the touchscreen\n");
        close (ts_fd);
        request_resume ();
        return 1;
    }

    if (SDL_Init (SDL_INIT_VIDEO) != 0) {
        fprintf (stderr, "pikalibrate: SDL_Init failed: %s\n", SDL_GetError ());
        close (ts_fd);
        request_resume ();
        return 1;
    }
    screen = SDL_SetVideoMode (SCREEN_W, SCREEN_H, 16, SDL_SWSURFACE);
    if (!screen) {
        fprintf (stderr, "pikalibrate: SDL_SetVideoMode failed: %s\n", SDL_GetError ());
        SDL_Quit ();
        close (ts_fd);
        request_resume ();
        return 1;
    }
    SDL_WM_SetCaption ("pikalibrate", "pikalibrate");

    black = SDL_MapRGB (screen->format, 0, 0, 0);
    white = SDL_MapRGB (screen->format, 255, 255, 255);

    targets[0].x = CROSS_INSET;               targets[0].y = CROSS_INSET;
    targets[1].x = SCREEN_W - 1 - CROSS_INSET; targets[1].y = CROSS_INSET;
    targets[2].x = CROSS_INSET;               targets[2].y = SCREEN_H - 1 - CROSS_INSET;
    targets[3].x = SCREEN_W - 1 - CROSS_INSET; targets[3].y = SCREEN_H - 1 - CROSS_INSET;

    for (i = 0; i < 4; i++) {
        fill_rect (screen, 0, 0, SCREEN_W, SCREEN_H, black);
        draw_cross (screen, targets[i].x, targets[i].y, white);
        SDL_UpdateRect (screen, 0, 0, 0, 0);

        fprintf (stderr, "pikalibrate: tap cross %d/4 at screen (%d,%d)\n", i + 1, targets[i].x, targets[i].y);
        raw[i] = sample_tap (ts_fd);
        fprintf (stderr, "pikalibrate:   raw (%d,%d)\n", raw[i].x, raw[i].y);
    }

    fill_rect (screen, 0, 0, SCREEN_W, SCREEN_H, black);
    SDL_UpdateRect (screen, 0, 0, 0, 0);

    SDL_Quit ();
    close (ts_fd);

    {
        /* Raw taps at the two measured screen X's (both crosses on the
         * left share targets[].x, both on the right share the other) and
         * likewise for Y. EvdevPtrScale() in the patched X server treats
         * XMIN/XMAX as the raw values at screen x=0 and x=SCREEN_W-1 --
         * but the crosses are drawn CROSS_INSET pixels inside those edges
         * (the panel doesn't reliably register taps right at the true
         * edge), so the raw averages measured here are NOT yet XMIN/XMAX.
         * Linearly extrapolate the two measured points out to x=0 and
         * x=SCREEN_W-1 (same for Y) using the panel's own measured
         * slope. Skipping this step was the first cut's bug: taps landed
         * dead-on at screen center but drifted further off the further
         * out you tapped, because the calibration window was narrower
         * than the panel's actual usable range.
         */
        double rx1 = (raw[0].x + raw[2].x) / 2.0; /* left-side taps, at targets[0].x */
        double rx2 = (raw[1].x + raw[3].x) / 2.0; /* right-side taps, at targets[1].x */
        double ry1 = (raw[0].y + raw[1].y) / 2.0; /* top-side taps, at targets[0].y */
        double ry2 = (raw[2].y + raw[3].y) / 2.0; /* bottom-side taps, at targets[2].y */
        double sx = (rx2 - rx1) / (double) (targets[1].x - targets[0].x);
        double sy = (ry2 - ry1) / (double) (targets[2].y - targets[0].y);

        int xmin = (int) (rx1 - sx * targets[0].x + 0.5);
        int xmax = (int) (rx2 + sx * ((SCREEN_W - 1) - targets[1].x) + 0.5);
        int ymin = (int) (ry1 - sy * targets[0].y + 0.5);
        int ymax = (int) (ry2 + sy * ((SCREEN_H - 1) - targets[2].y) + 0.5);

        fprintf (stderr, "pikalibrate: XMIN=%d XMAX=%d YMIN=%d YMAX=%d\n", xmin, xmax, ymin, ymax);
        if (write_config (xmin, xmax, ymin, ymax) != 0) {
            request_resume ();
            return 1;
        }
    }

    request_resume ();
    fprintf (stderr, "pikalibrate: done\n");
    return 0;
}
