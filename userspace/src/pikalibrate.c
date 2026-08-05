/*
 * pikalibrate -- on-device touchscreen calibration for the Zaurus.
 *
 * Two modes, chosen from a menu:
 *
 *   Calibrate  shows 4 crosses in sequence (corners, inset so taps land
 *              inside the panel's actually-reachable area -- see
 *              docs/HOWTO-X11-TOUCHSCREEN.md) and writes the resulting
 *              XMIN/XMAX/YMIN/YMAX to /etc/piko/touchscreen.cfg, read at
 *              startup (and on reload) by the patched kdrive evdev driver
 *              in userspace/src/xserver/hw/kdrive/linux/evdev.c.
 *   Test       draws a fading ripple wherever you tap, using the
 *              calibration currently in force. Nothing else on the device
 *              can answer "did that actually work?": the desktop is the
 *              only other feedback and its targets are small, so a bad
 *              calibration reads as "the icons are broken" rather than as
 *              a calibration problem. Here the tap and the dot are the
 *              whole picture.
 *
 * DRAWN AND DRIVEN BY HAND. Everything on screen is SDL rectangles and a
 * 5x7 bitmap font compiled in below -- no SDL_ttf, no font file, no
 * theme. That is deliberate for a tool whose whole job is to run when the
 * touchscreen (and by extension the desktop) cannot be trusted: it has
 * exactly one shared library to load, and nothing it draws depends on a
 * file that could be missing. Input is read straight off the evdev nodes
 * for the same reason SDL's own input layer is bypassed for the
 * touchscreen (it reads /dev/input/mice, not this device) -- and it means
 * the keyboard works here even though SDL was never told about it.
 *
 * If a live Xfbdev is running, it holds the touchscreen AND the keyboard
 * open with an exclusive EVIOCGRAB, so this can't just open the devices
 * and expect real events. Xfbdev listens on a small FIFO
 * (/tmp/.pikalibrate-ctl, see hw/kdrive/linux/linux.c's Pikalibrate
 * control channel) for "SUSPEND"/"RESUME" commands that map onto its
 * existing KdSuspend()/KdResume() -- the same suspend/resume it already
 * uses for real APM events -- so the desktop briefly freezes and reappears
 * (freshly repainted) around the session, with no session restart. If the
 * FIFO doesn't exist, no X server is running (e.g. first boot, before
 * xsession starts) and both devices are simply free already.
 *
 * KEYS. Arrows move between the two buttons, Enter (or the hardware OK
 * button, which reports F11) activates, Backspace leaves whatever you are
 * in -- a mode returns to the menu, the menu exits. Backspace rather than
 * Escape because the Zaurus thumb keyboard has a Backspace keycap and no
 * Escape one; Escape and Cancel are accepted too for anyone on a USB
 * keyboard. The touchscreen also works throughout, but it is the thing
 * under test: the menu is reachable by keyboard alone precisely so a
 * digitiser that lands nowhere near where you tapped cannot lock you out
 * of the tool that fixes it.
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
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define TOUCHSCREEN_CFG     "/etc/piko/touchscreen.cfg"
#define TOUCHSCREEN_CFG_TMP "/etc/piko/touchscreen.cfg.new"
#define PIKALIBRATE_FIFO    "/tmp/.pikalibrate-ctl"
#define TOUCHSCREEN_NAME    "ADS7846 Touchscreen"
#define KEYBOARD_NAME       "matrix-keypad"
#define FALLBACK_TOUCH      "/dev/input/event2"
#define FALLBACK_KEYS       "/dev/input/event1"

#define SCREEN_W    640
#define SCREEN_H    480
#define CROSS_INSET 60
#define CROSS_ARM   12
#define CROSS_THICK 3
#define GRAB_TIMEOUT_MS 2000

/*
 * Colours. The greys are the Piko matchbox theme's own
 * (data/themes/Piko/theme.xml): #dadad5 is what the panel and titlebars
 * are painted with, #acaaa5 their border. The button FILL is not from the
 * theme -- white text on #dadad5 is barely legible, and the label has to
 * be readable from an arm's length on a reflective panel -- so the fill is
 * a dark neutral of the same family and the theme greys do the framing.
 */
#define RGB_BG          0x00, 0x00, 0x00
#define RGB_BTN         0x2a, 0x2a, 0x28
#define RGB_BTN_SEL     0x3e, 0x3e, 0x3b
#define RGB_BORDER      0xac, 0xaa, 0xa5
#define RGB_BORDER_SEL  0xff, 0xff, 0xff
#define RGB_TEXT        0xff, 0xff, 0xff
#define RGB_HINT        0xac, 0xaa, 0xa5
#define RGB_RIPPLE      0xda, 0xda, 0xd5

/* Menu geometry. */
#define BTN_W       200
#define BTN_H       72
#define BTN_GAP     40
#define BORDER_PX   3

/* Test-mode ripple: how big it grows, and how long it takes to do it. */
#define RIPPLE_MS       550
#define RIPPLE_R_MIN    6
#define RIPPLE_R_MAX    64
#define MAX_RIPPLES     8
#define TEST_FRAME_MS   40      /* ~25fps; the fb is on a slow bus */

typedef struct { int x, y; } Point;

enum { MENU_CALIBRATE = 0, MENU_TEST = 1, MENU_COUNT = 2 };

static int ctl_fd = -1;

/* The calibration currently in force. Defaults match the ones compiled
 * into the X server (cal_xmin et al in its evdev.c), so Test behaves the
 * same as the desktop does on a device that has never been calibrated. */
static int cal_xmin = 221, cal_xmax = 3807;
static int cal_ymin = 282, cal_ymax = 3800;

/* ------------------------------------------------------------------ */
/* a 5x7 font                                                          */

/* One entry per printable character used by this program, five columns
 * each, bit 0 = top row. The classic 5x7 cell: narrow enough that a whole
 * hint line fits across 640px at scale 2, blocky enough to stay legible
 * on a panel with no subpixel anything. */
static const unsigned char Font5x7[][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, /* space */
    { 0x7e, 0x11, 0x11, 0x11, 0x7e }, /* A */
    { 0x7f, 0x49, 0x49, 0x49, 0x36 }, /* B */
    { 0x3e, 0x41, 0x41, 0x41, 0x22 }, /* C */
    { 0x7f, 0x41, 0x41, 0x22, 0x1c }, /* D */
    { 0x7f, 0x49, 0x49, 0x49, 0x41 }, /* E */
    { 0x7f, 0x09, 0x09, 0x09, 0x01 }, /* F */
    { 0x3e, 0x41, 0x49, 0x49, 0x7a }, /* G */
    { 0x7f, 0x08, 0x08, 0x08, 0x7f }, /* H */
    { 0x00, 0x41, 0x7f, 0x41, 0x00 }, /* I */
    { 0x20, 0x40, 0x41, 0x3f, 0x01 }, /* J */
    { 0x7f, 0x08, 0x14, 0x22, 0x41 }, /* K */
    { 0x7f, 0x40, 0x40, 0x40, 0x40 }, /* L */
    { 0x7f, 0x02, 0x0c, 0x02, 0x7f }, /* M */
    { 0x7f, 0x04, 0x08, 0x10, 0x7f }, /* N */
    { 0x3e, 0x41, 0x41, 0x41, 0x3e }, /* O */
    { 0x7f, 0x09, 0x09, 0x09, 0x06 }, /* P */
    { 0x3e, 0x41, 0x51, 0x21, 0x5e }, /* Q */
    { 0x7f, 0x09, 0x19, 0x29, 0x46 }, /* R */
    { 0x46, 0x49, 0x49, 0x49, 0x31 }, /* S */
    { 0x01, 0x01, 0x7f, 0x01, 0x01 }, /* T */
    { 0x3f, 0x40, 0x40, 0x40, 0x3f }, /* U */
    { 0x1f, 0x20, 0x40, 0x20, 0x1f }, /* V */
    { 0x3f, 0x40, 0x38, 0x40, 0x3f }, /* W */
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, /* X */
    { 0x07, 0x08, 0x70, 0x08, 0x07 }, /* Y */
    { 0x61, 0x51, 0x49, 0x45, 0x43 }, /* Z */
    { 0x3e, 0x51, 0x49, 0x45, 0x3e }, /* 0 */
    { 0x00, 0x42, 0x7f, 0x40, 0x00 }, /* 1 */
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, /* 2 */
    { 0x21, 0x41, 0x45, 0x4b, 0x31 }, /* 3 */
    { 0x18, 0x14, 0x12, 0x7f, 0x10 }, /* 4 */
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, /* 5 */
    { 0x3c, 0x4a, 0x49, 0x49, 0x30 }, /* 6 */
    { 0x01, 0x71, 0x09, 0x05, 0x03 }, /* 7 */
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, /* 8 */
    { 0x06, 0x49, 0x49, 0x29, 0x1e }, /* 9 */
    { 0x00, 0x60, 0x60, 0x00, 0x00 }, /* . */
    { 0x00, 0x36, 0x36, 0x00, 0x00 }, /* : */
    { 0x08, 0x08, 0x08, 0x08, 0x08 }, /* - */
    { 0x00, 0x00, 0x5f, 0x00, 0x00 }, /* ! */
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, /* , */
    { 0x14, 0x14, 0x14, 0x14, 0x14 }, /* = */
};

/* Map a character to its Font5x7 row. Unknown characters become spaces
 * rather than tripping an assert -- a missing glyph should cost a hole in
 * a label, never the tool. */
static int
glyph_index (char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char) (c - 'a' + 'A');

    if (c == ' ')  return 0;
    if (c >= 'A' && c <= 'Z') return 1 + (c - 'A');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    switch (c) {
    case '.': return 37;
    case ':': return 38;
    case '-': return 39;
    case '!': return 40;
    case ',': return 41;
    case '=': return 42;
    default:  return 0;
    }
}

static void
fill_rect (SDL_Surface *s, int x, int y, int w, int h, Uint32 c)
{
    SDL_Rect r;

    if (w <= 0 || h <= 0)
        return;
    r.x = (Sint16) x;
    r.y = (Sint16) y;
    r.w = (Uint16) w;
    r.h = (Uint16) h;
    SDL_FillRect (s, &r, c);
}

static int
text_width (const char *str, int scale)
{
    int n = (int) strlen (str);

    return n > 0 ? (n * 6 - 1) * scale : 0;
}

static void
draw_text (SDL_Surface *s, int x, int y, const char *str, int scale, Uint32 col)
{
    int i, col_i, row;

    for (i = 0; str[i] != '\0'; i++) {
        const unsigned char *g = Font5x7[glyph_index (str[i])];

        for (col_i = 0; col_i < 5; col_i++)
            for (row = 0; row < 7; row++)
                if (g[col_i] & (1 << row))
                    fill_rect (s, x + (i * 6 + col_i) * scale, y + row * scale,
                               scale, scale, col);
    }
}

static void
draw_text_centered (SDL_Surface *s, int cx, int y, const char *str,
                    int scale, Uint32 col)
{
    draw_text (s, cx - text_width (str, scale) / 2, y, str, scale, col);
}

/* ------------------------------------------------------------------ */
/* input devices                                                       */

/* Scans /sys/class/input/eventN/device/name for a kernel device name,
 * since /dev/input/eventN numbering is pure enumeration order (see
 * docs/HOWTO-X11-TOUCHSCREEN.md) and can shift. */
static const char *
find_input_device (const char *want, const char *fallback)
{
    DIR *d;
    struct dirent *ent;
    static char path[2][320];
    static int  slot = 0;
    char namepath[320];
    char namebuf[64];
    char *out = path[slot];
    FILE *f;

    slot = (slot + 1) % 2;      /* two callers, two buffers */

    d = opendir ("/sys/class/input");
    if (!d)
        return fallback;

    while ((ent = readdir (d)) != NULL) {
        if (strncmp (ent->d_name, "event", 5) != 0)
            continue;
        snprintf (namepath, sizeof (namepath), "/sys/class/input/%s/device/name", ent->d_name);
        f = fopen (namepath, "r");
        if (!f)
            continue;
        if (fgets (namebuf, sizeof (namebuf), f)) {
            namebuf[strcspn (namebuf, "\n")] = '\0';
            if (strcmp (namebuf, want) == 0) {
                fclose (f);
                closedir (d);
                snprintf (out, sizeof (path[0]), "/dev/input/%s", ent->d_name);
                return out;
            }
        }
        fclose (f);
    }
    closedir (d);

    fprintf (stderr, "pikalibrate: no input device named \"%s\", falling back to %s\n",
             want, fallback);
    return fallback;
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

/* ------------------------------------------------------------------ */
/* reading taps                                                        */

/*
 * One tap, accumulated a frame at a time.
 *
 * AVERAGE WHOLE FRAMES, NOT LOOSE EVENTS. The evdev protocol reports a
 * position as a group of events terminated by EV_SYN/SYN_REPORT, and the
 * input core DROPS any absolute event whose value equals the last one it
 * sent (input_handle_event()'s duplicate filter). So a still-ish finger
 * emits ABS_X on some frames and ABS_Y on others, and the two axes do NOT
 * arrive in equal numbers.
 *
 * The first cut summed each axis as its events arrived and divided both
 * sums by the ABS_Y count. That is a mean only for Y; for X it is
 * sum(x)/n_y == mean(x) * n_x/n_y -- the true average scaled by a ratio
 * that has nothing to do with where the tap was. Every measured raw X
 * therefore came out multiplied by a constant, which the extrapolation in
 * run_calibrate() faithfully carried into XMIN and XMAX, and a calibration
 * whose span is wrong by a factor is exactly the "lands right, and further
 * right the further out you tap" error: the offset at the origin is small
 * and the gain is what is off.
 *
 * Latching both axes and accumulating one (x, y) pair per SYN_REPORT frame
 * fixes it -- an axis that did not change simply keeps its value, which is
 * what the missing event means.
 */
typedef struct {
    long sumx, sumy;
    int  n;
    int  pressure;
    int  down;
    int  curx, cury;
    int  have_x, have_y;
} TouchState;

static void
touch_reset (TouchState *t)
{
    memset (t, 0, sizeof (*t));
}

/* Feeds one event in. Returns 1 when a complete tap (press through
 * release) has been accumulated, leaving its averaged raw position in
 * *out. */
static int
touch_feed (TouchState *t, const struct input_event *ev, Point *out)
{
    switch (ev->type) {
    case EV_KEY:
        if (ev->code == BTN_TOUCH) {
            if (ev->value) {
                t->down = 1;
            } else if (t->down && t->n > 0) {
                out->x = (int) (t->sumx / t->n);
                out->y = (int) (t->sumy / t->n);
                touch_reset (t);
                return 1;
            } else {
                t->down = 0;
            }
        }
        break;
    case EV_ABS:
        if (ev->code == ABS_PRESSURE)
            t->pressure = ev->value;
        else if (ev->code == ABS_X) {
            t->curx = ev->value;
            t->have_x = 1;
        } else if (ev->code == ABS_Y) {
            t->cury = ev->value;
            t->have_y = 1;
        }
        break;
    case EV_SYN:
        /* One position per completed frame, and only while the pen is
         * genuinely down: the release frame carries pressure 0 (and often
         * a garbage coordinate with it), which is the same sample the X
         * server's own driver refuses to trust. */
        if (ev->code == SYN_REPORT && t->down && t->pressure > 0
            && t->have_x && t->have_y) {
            t->sumx += t->curx;
            t->sumy += t->cury;
            t->n++;
        }
        break;
    }
    return 0;
}

/* What poll_input() saw. */
typedef struct {
    int   key;          /* KEY_* of a key that went DOWN, else 0 */
    int   tapped;       /* 1 if a tap completed */
    Point tap;          /* its averaged raw position */
} Input;

/*
 * Waits up to timeout_ms for something to happen on either device.
 * timeout_ms < 0 blocks indefinitely. Returns 1 if ev holds an event, 0
 * on timeout -- which is what drives the Test animation, since a ripple
 * has to keep growing while nothing at all is being pressed.
 */
static int
poll_input (int ts_fd, int kbd_fd, TouchState *ts, int timeout_ms, Input *ev)
{
    struct timeval tv, *tvp = NULL;
    struct input_event iev;
    fd_set rfds;
    int maxfd = ts_fd > kbd_fd ? ts_fd : kbd_fd;
    int n;

    memset (ev, 0, sizeof (*ev));

    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    FD_ZERO (&rfds);
    if (ts_fd >= 0)  FD_SET (ts_fd, &rfds);
    if (kbd_fd >= 0) FD_SET (kbd_fd, &rfds);

    n = select (maxfd + 1, &rfds, NULL, NULL, tvp);
    if (n <= 0)
        return 0;               /* timeout, or EINTR -- caller re-polls */

    if (kbd_fd >= 0 && FD_ISSET (kbd_fd, &rfds)) {
        while (read (kbd_fd, &iev, sizeof (iev)) == (int) sizeof (iev)) {
            /* Key down only. Autorepeat (value 2) would run the menu away
             * from under a held finger. */
            if (iev.type == EV_KEY && iev.value == 1) {
                ev->key = iev.code;
                return 1;
            }
        }
    }

    if (ts_fd >= 0 && FD_ISSET (ts_fd, &rfds)) {
        while (read (ts_fd, &iev, sizeof (iev)) == (int) sizeof (iev)) {
            if (touch_feed (ts, &iev, &ev->tap)) {
                ev->tapped = 1;
                return 1;
            }
        }
    }

    return ev->key || ev->tapped;
}

/* ------------------------------------------------------------------ */
/* calibration data                                                    */

static void
load_calibration (void)
{
    FILE *f = fopen (TOUCHSCREEN_CFG, "r");
    char line[128];
    int v;

    if (!f)
        return;                 /* never calibrated: compiled-in defaults */

    while (fgets (line, sizeof (line), f)) {
        if (sscanf (line, "XMIN=%d", &v) == 1)      cal_xmin = v;
        else if (sscanf (line, "XMAX=%d", &v) == 1) cal_xmax = v;
        else if (sscanf (line, "YMIN=%d", &v) == 1) cal_ymin = v;
        else if (sscanf (line, "YMAX=%d", &v) == 1) cal_ymax = v;
    }
    fclose (f);
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

/* Raw digitiser value to screen pixel, the same mapping EvdevPtrScale()
 * does in the X server -- so what Test draws is what the desktop would
 * have done with that tap. */
static int
raw_to_screen (int v, int lo, int hi, int span)
{
    if (hi <= lo || span <= 1)
        return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (v - lo) * (span - 1) / (hi - lo);
}

static void
tap_to_screen (Point raw, Point *scr)
{
    scr->x = raw_to_screen (raw.x, cal_xmin, cal_xmax, SCREEN_W);
    scr->y = raw_to_screen (raw.y, cal_ymin, cal_ymax, SCREEN_H);
}

/* ------------------------------------------------------------------ */
/* drawing helpers                                                     */

static void
draw_cross (SDL_Surface *screen, int cx, int cy, Uint32 color)
{
    fill_rect (screen, cx - CROSS_ARM, cy - CROSS_THICK / 2, CROSS_ARM * 2 + 1, CROSS_THICK, color);
    fill_rect (screen, cx - CROSS_THICK / 2, cy - CROSS_ARM, CROSS_THICK, CROSS_ARM * 2 + 1, color);
}

/* Integer square root, so a filled circle costs no libm. */
static int
isqrt (int n)
{
    int rem = 0, root = 0, i;

    for (i = 0; i < 16; i++) {
        root <<= 1;
        rem = (rem << 2) | ((n >> 30) & 3);
        n <<= 2;
        if (root < rem) {
            rem -= root | 1;
            root += 2;
        }
    }
    return root >> 1;
}

/* Filled circle, one horizontal span per row -- far cheaper than testing
 * every pixel in the bounding box, which at r=64 would be 16k tests a
 * frame on a 400MHz part. */
static void
fill_circle (SDL_Surface *s, int cx, int cy, int r, Uint32 col)
{
    int dy;

    for (dy = -r; dy <= r; dy++) {
        int dx = isqrt (r * r - dy * dy);

        fill_rect (s, cx - dx, cy + dy, dx * 2 + 1, 1, col);
    }
}

/* The one line every mode carries, centred, so there is always a way out
 * on screen. */
static void
draw_hint (SDL_Surface *s, const char *hint)
{
    draw_text_centered (s, SCREEN_W / 2, SCREEN_H / 2 - 3, hint, 2,
                        SDL_MapRGB (s->format, RGB_HINT));
}

static SDL_Rect
hint_rect (const char *hint)
{
    SDL_Rect r;
    int w = text_width (hint, 2);

    r.x = (Sint16) (SCREEN_W / 2 - w / 2 - 4);
    r.y = (Sint16) (SCREEN_H / 2 - 3 - 4);
    r.w = (Uint16) (w + 8);
    r.h = (Uint16) (7 * 2 + 8);
    return r;
}

#define HINT_TEXT "PRESS BACKSPACE TO RETURN"

/* ------------------------------------------------------------------ */
/* the menu                                                            */

static void
menu_button_rect (int which, SDL_Rect *r)
{
    int total = MENU_COUNT * BTN_W + (MENU_COUNT - 1) * BTN_GAP;
    int x0 = (SCREEN_W - total) / 2;

    r->x = (Sint16) (x0 + which * (BTN_W + BTN_GAP));
    r->y = (Sint16) (SCREEN_H / 2 - BTN_H - 30);
    r->w = (Uint16) BTN_W;
    r->h = (Uint16) BTN_H;
}

static void
draw_menu (SDL_Surface *screen, int selected)
{
    static const char *labels[MENU_COUNT] = { "CALIBRATE", "TEST" };
    Uint32 bg     = SDL_MapRGB (screen->format, RGB_BG);
    Uint32 text   = SDL_MapRGB (screen->format, RGB_TEXT);
    Uint32 hint   = SDL_MapRGB (screen->format, RGB_HINT);
    int i;

    fill_rect (screen, 0, 0, SCREEN_W, SCREEN_H, bg);

    draw_text_centered (screen, SCREEN_W / 2, 60, "TOUCHSCREEN", 3, text);

    for (i = 0; i < MENU_COUNT; i++) {
        SDL_Rect r;
        Uint32 border = SDL_MapRGB (screen->format,
                                    i == selected ? 0xff : 0xac,
                                    i == selected ? 0xff : 0xaa,
                                    i == selected ? 0xff : 0xa5);
        Uint32 fill = SDL_MapRGB (screen->format,
                                  i == selected ? 0x3e : 0x2a,
                                  i == selected ? 0x3e : 0x2a,
                                  i == selected ? 0x3b : 0x28);
        int bw = i == selected ? BORDER_PX : 1;

        menu_button_rect (i, &r);

        /* Border as four rectangles around the fill: the selected button
         * gets a thicker, brighter one, which is the only thing marking
         * it -- so it reads at a glance and without colour vision. */
        fill_rect (screen, r.x, r.y, r.w, r.h, border);
        fill_rect (screen, r.x + bw, r.y + bw, r.w - 2 * bw, r.h - 2 * bw, fill);

        draw_text_centered (screen, r.x + r.w / 2,
                            r.y + r.h / 2 - 7,
                            labels[i], 2, text);
    }

    draw_text_centered (screen, SCREEN_W / 2, SCREEN_H / 2 + 60,
                        "ARROWS TO MOVE, ENTER TO SELECT", 2, hint);
    draw_text_centered (screen, SCREEN_W / 2, SCREEN_H / 2 + 90,
                        "BACKSPACE TO QUIT", 2, hint);

    SDL_UpdateRect (screen, 0, 0, 0, 0);
}

/* Returns 1 with *choice set, or 0 to quit. */
static int
run_menu (SDL_Surface *screen, int ts_fd, int kbd_fd, int *choice)
{
    TouchState ts;
    int selected = MENU_CALIBRATE;   /* the common case, so it is the default */

    touch_reset (&ts);
    draw_menu (screen, selected);

    for (;;) {
        Input in;

        if (!poll_input (ts_fd, kbd_fd, &ts, -1, &in))
            continue;

        if (in.key) {
            switch (in.key) {
            case KEY_LEFT:
            case KEY_UP:
                if (selected > 0) {
                    selected--;
                    draw_menu (screen, selected);
                }
                break;
            case KEY_RIGHT:
            case KEY_DOWN:
                if (selected < MENU_COUNT - 1) {
                    selected++;
                    draw_menu (screen, selected);
                }
                break;
            case KEY_ENTER:
            case KEY_KPENTER:
            case KEY_F11:       /* the hardware OK button under the screen */
                *choice = selected;
                return 1;
            case KEY_BACKSPACE:
            case KEY_ESC:
            case KEY_F4:        /* the hardware Cancel button */
                return 0;
            default:
                break;
            }
            continue;
        }

        if (in.tapped) {
            Point p;
            int i;

            tap_to_screen (in.tap, &p);

            for (i = 0; i < MENU_COUNT; i++) {
                SDL_Rect r;

                menu_button_rect (i, &r);
                if (p.x >= r.x && p.x < r.x + r.w &&
                    p.y >= r.y && p.y < r.y + r.h) {
                    *choice = i;
                    return 1;
                }
            }

            /* A tap that hit neither button still says something: move
             * the selection to whichever button is nearer, so a digitiser
             * that is off by a long way can still be steered by tapping
             * roughly at a button and confirming with Enter. */
            selected = (p.x < SCREEN_W / 2) ? MENU_CALIBRATE : MENU_TEST;
            draw_menu (screen, selected);
        }
    }
}

/* ------------------------------------------------------------------ */
/* calibrate                                                           */

/* Blocks until one full tap completes, or Backspace is pressed. Returns 1
 * with *out set, or 0 to abandon the calibration. */
static int
sample_tap (int ts_fd, int kbd_fd, Point *out)
{
    TouchState ts;

    touch_reset (&ts);

    for (;;) {
        Input in;

        if (!poll_input (ts_fd, kbd_fd, &ts, -1, &in))
            continue;

        if (in.key == KEY_BACKSPACE || in.key == KEY_ESC || in.key == KEY_F4)
            return 0;

        if (in.tapped) {
            *out = in.tap;
            return 1;
        }
    }
}

static void
run_calibrate (SDL_Surface *screen, int ts_fd, int kbd_fd)
{
    Uint32 bg    = SDL_MapRGB (screen->format, RGB_BG);
    Uint32 white = SDL_MapRGB (screen->format, RGB_TEXT);
    Point targets[4];
    Point raw[4];
    int   i;

    targets[0].x = CROSS_INSET;                targets[0].y = CROSS_INSET;
    targets[1].x = SCREEN_W - 1 - CROSS_INSET; targets[1].y = CROSS_INSET;
    targets[2].x = CROSS_INSET;                targets[2].y = SCREEN_H - 1 - CROSS_INSET;
    targets[3].x = SCREEN_W - 1 - CROSS_INSET; targets[3].y = SCREEN_H - 1 - CROSS_INSET;

    for (i = 0; i < 4; i++) {
        char progress[32];

        fill_rect (screen, 0, 0, SCREEN_W, SCREEN_H, bg);
        draw_cross (screen, targets[i].x, targets[i].y, white);
        snprintf (progress, sizeof (progress), "TAP THE CROSS  %d OF 4", i + 1);
        draw_text_centered (screen, SCREEN_W / 2, SCREEN_H / 2 - 40, progress, 2, white);
        draw_hint (screen, HINT_TEXT);
        SDL_UpdateRect (screen, 0, 0, 0, 0);

        fprintf (stderr, "pikalibrate: tap cross %d/4 at screen (%d,%d)\n",
                 i + 1, targets[i].x, targets[i].y);

        if (!sample_tap (ts_fd, kbd_fd, &raw[i])) {
            fprintf (stderr, "pikalibrate: calibration abandoned\n");
            return;             /* nothing written: the old values stand */
        }
        fprintf (stderr, "pikalibrate:   raw (%d,%d)\n", raw[i].x, raw[i].y);
    }

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
         *
         * The four crosses are averaged in pairs rather than measured
         * with two taps, and that is deliberate: both left-hand crosses
         * sit at the same screen X, so averaging their raw X halves the
         * noise on the number without biasing it, and the same for the
         * right-hand pair (and top/bottom for Y). Pair averaging is not
         * what made taps drift -- see touch_feed() above for what did.
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

        fprintf (stderr, "pikalibrate: XMIN=%d XMAX=%d YMIN=%d YMAX=%d\n",
                 xmin, xmax, ymin, ymax);

        fill_rect (screen, 0, 0, SCREEN_W, SCREEN_H, bg);
        if (write_config (xmin, xmax, ymin, ymax) == 0) {
            /* Adopt it immediately, so Test measures what was just
             * written rather than what was in force when we started. */
            cal_xmin = xmin; cal_xmax = xmax;
            cal_ymin = ymin; cal_ymax = ymax;
            draw_text_centered (screen, SCREEN_W / 2, SCREEN_H / 2 - 10, "SAVED", 3, white);
        } else {
            draw_text_centered (screen, SCREEN_W / 2, SCREEN_H / 2 - 10, "COULD NOT SAVE", 2, white);
        }
        SDL_UpdateRect (screen, 0, 0, 0, 0);
        SDL_Delay (900);
    }
}

/* ------------------------------------------------------------------ */
/* test                                                                */

typedef struct {
    int    active;
    int    cx, cy;
    Uint32 started;
} Ripple;

static void
run_test (SDL_Surface *screen, int ts_fd, int kbd_fd)
{
    Uint32  bg = SDL_MapRGB (screen->format, RGB_BG);
    Ripple  ripples[MAX_RIPPLES];
    TouchState ts;
    SDL_Rect hint_r = hint_rect (HINT_TEXT);
    int     i;

    memset (ripples, 0, sizeof (ripples));
    touch_reset (&ts);

    fill_rect (screen, 0, 0, SCREEN_W, SCREEN_H, bg);
    draw_hint (screen, HINT_TEXT);
    SDL_UpdateRect (screen, 0, 0, 0, 0);

    for (;;) {
        Uint32   now;
        Input    in;
        SDL_Rect dirty[MAX_RIPPLES + 1];
        int      ndirty = 0;
        int      any_active = 0;

        if (poll_input (ts_fd, kbd_fd, &ts, TEST_FRAME_MS, &in)) {
            if (in.key == KEY_BACKSPACE || in.key == KEY_ESC || in.key == KEY_F4)
                return;

            if (in.tapped) {
                Point p;

                tap_to_screen (in.tap, &p);
                fprintf (stderr, "pikalibrate: test tap raw (%d,%d) -> screen (%d,%d)\n",
                         in.tap.x, in.tap.y, p.x, p.y);

                /* Oldest slot wins if they are all busy: a fast tapper
                 * should never be told to wait. */
                {
                    int slot = 0;
                    Uint32 oldest = 0xffffffffu;

                    for (i = 0; i < MAX_RIPPLES; i++) {
                        if (!ripples[i].active) { slot = i; break; }
                        if (ripples[i].started < oldest) {
                            oldest = ripples[i].started;
                            slot = i;
                        }
                    }
                    ripples[slot].active  = 1;
                    ripples[slot].cx      = p.x;
                    ripples[slot].cy      = p.y;
                    ripples[slot].started = SDL_GetTicks ();
                }
            }
        }

        now = SDL_GetTicks ();

        /* Redraw into the surface, which is RAM and cheap, then push only
         * the boxes that could have changed -- the blit to the panel is
         * the expensive half and a full-screen one every frame would not
         * keep up on this bus. */
        fill_rect (screen, 0, 0, SCREEN_W, SCREEN_H, bg);

        for (i = 0; i < MAX_RIPPLES; i++) {
            Uint32 age;
            int    r, shade;

            if (!ripples[i].active)
                continue;

            age = now - ripples[i].started;

            /* A ripple's box is claimed at its FINAL size for the whole
             * of its life, so the frame that ends it repaints everything
             * it ever covered. Tracking the growing box instead leaves a
             * ring of stale pixels behind on the last frame. */
            dirty[ndirty].x = (Sint16) (ripples[i].cx - RIPPLE_R_MAX - 1);
            dirty[ndirty].y = (Sint16) (ripples[i].cy - RIPPLE_R_MAX - 1);
            dirty[ndirty].w = (Uint16) (RIPPLE_R_MAX * 2 + 3);
            dirty[ndirty].h = (Uint16) (RIPPLE_R_MAX * 2 + 3);
            ndirty++;

            if (age >= RIPPLE_MS) {
                ripples[i].active = 0;      /* cleared by this same frame */
                continue;
            }
            any_active = 1;

            /* Grows out and fades to the background as it goes: the tap
             * is where it started, not where it ended up. */
            r = RIPPLE_R_MIN + (int) ((RIPPLE_R_MAX - RIPPLE_R_MIN) * age / RIPPLE_MS);
            shade = 255 - (int) (255 * age / RIPPLE_MS);

            fill_circle (screen, ripples[i].cx, ripples[i].cy, r,
                         SDL_MapRGB (screen->format,
                                     (Uint8) (0xda * shade / 255),
                                     (Uint8) (0xda * shade / 255),
                                     (Uint8) (0xd5 * shade / 255)));
        }

        /* Last, so a ripple passing under it never hides the way out. */
        draw_hint (screen, HINT_TEXT);
        if (ndirty > 0) {
            dirty[ndirty++] = hint_r;
            SDL_UpdateRects (screen, ndirty, dirty);
        }

        (void) any_active;
    }
}

/* ------------------------------------------------------------------ */

int
main (void)
{
    const char *ts_path, *kbd_path;
    int   ts_fd, kbd_fd;
    int   using_x;
    SDL_Surface *screen;

    ts_path  = find_input_device (TOUCHSCREEN_NAME, FALLBACK_TOUCH);
    kbd_path = find_input_device (KEYBOARD_NAME, FALLBACK_KEYS);
    fprintf (stderr, "pikalibrate: touchscreen %s, keyboard %s\n", ts_path, kbd_path);

    load_calibration ();

    using_x = request_suspend ();
    if (using_x)
        fprintf (stderr, "pikalibrate: told Xfbdev to suspend, waiting for the devices...\n");

    /*
     * NON-BLOCKING, both of them, and this is not optional. poll_input()
     * drains each device with "while (read(...) == sizeof ev)" after
     * select() says it is readable; on a blocking fd that loop does not
     * end when the queue empties, it stops dead inside read() waiting for
     * an event that may never come. Stuck there on the keyboard, the
     * touchscreen is never looked at again -- so touch worked until the
     * first key was pressed (its release events refill the keyboard queue)
     * and was dead from then on, which is a far stranger symptom than the
     * mistake deserves. EAGAIN ends the loop instead.
     */
    ts_fd = open (ts_path, O_RDONLY | O_NONBLOCK);
    if (ts_fd < 0) {
        fprintf (stderr, "pikalibrate: open %s: %s\n", ts_path, strerror (errno));
        request_resume ();
        return 1;
    }

    /* Non-fatal: without a keyboard the tool is still usable by touch,
     * and refusing to start would be the wrong trade for a device whose
     * keyboard is not the thing being fixed. */
    kbd_fd = open (kbd_path, O_RDONLY | O_NONBLOCK);
    if (kbd_fd < 0)
        fprintf (stderr, "pikalibrate: open %s: %s (keys unavailable)\n",
                 kbd_path, strerror (errno));

    if (using_x && !wait_for_device (ts_fd, GRAB_TIMEOUT_MS)) {
        fprintf (stderr, "pikalibrate: timed out waiting for Xfbdev to release the touchscreen\n");
        close (ts_fd);
        if (kbd_fd >= 0) close (kbd_fd);
        request_resume ();
        return 1;
    }

    if (SDL_Init (SDL_INIT_VIDEO) != 0) {
        fprintf (stderr, "pikalibrate: SDL_Init failed: %s\n", SDL_GetError ());
        close (ts_fd);
        if (kbd_fd >= 0) close (kbd_fd);
        request_resume ();
        return 1;
    }
    screen = SDL_SetVideoMode (SCREEN_W, SCREEN_H, 16, SDL_SWSURFACE);
    if (!screen) {
        fprintf (stderr, "pikalibrate: SDL_SetVideoMode failed: %s\n", SDL_GetError ());
        SDL_Quit ();
        close (ts_fd);
        if (kbd_fd >= 0) close (kbd_fd);
        request_resume ();
        return 1;
    }
    SDL_WM_SetCaption ("pikalibrate", "pikalibrate");

    for (;;) {
        int choice;

        if (!run_menu (screen, ts_fd, kbd_fd, &choice))
            break;

        if (choice == MENU_CALIBRATE)
            run_calibrate (screen, ts_fd, kbd_fd);
        else
            run_test (screen, ts_fd, kbd_fd);
    }

    fill_rect (screen, 0, 0, SCREEN_W, SCREEN_H, SDL_MapRGB (screen->format, RGB_BG));
    SDL_UpdateRect (screen, 0, 0, 0, 0);

    SDL_Quit ();
    close (ts_fd);
    if (kbd_fd >= 0) close (kbd_fd);

    request_resume ();
    fprintf (stderr, "pikalibrate: done\n");
    return 0;
}
