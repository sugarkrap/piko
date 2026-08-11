
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

#define RGB_BG          0x00, 0x00, 0x00
#define RGB_BTN         0x2a, 0x2a, 0x28
#define RGB_BTN_SEL     0x3e, 0x3e, 0x3b
#define RGB_BORDER      0xac, 0xaa, 0xa5
#define RGB_BORDER_SEL  0xff, 0xff, 0xff
#define RGB_TEXT        0xff, 0xff, 0xff
#define RGB_HINT        0xac, 0xaa, 0xa5
#define RGB_RIPPLE      0xda, 0xda, 0xd5

#define BTN_W       200
#define BTN_H       72
#define BTN_GAP     40
#define BORDER_PX   3

#define RIPPLE_MS       550
#define RIPPLE_R_MIN    6
#define RIPPLE_R_MAX    64
#define MAX_RIPPLES     8
#define TEST_FRAME_MS   40

typedef struct { int x, y; } Point;

enum { MENU_CALIBRATE = 0, MENU_TEST = 1, MENU_COUNT = 2 };

static int ctl_fd = -1;

static int cal_xmin = 221, cal_xmax = 3807;
static int cal_ymin = 282, cal_ymax = 3800;

static const unsigned char Font5x7[][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 },
    { 0x7e, 0x11, 0x11, 0x11, 0x7e },
    { 0x7f, 0x49, 0x49, 0x49, 0x36 },
    { 0x3e, 0x41, 0x41, 0x41, 0x22 },
    { 0x7f, 0x41, 0x41, 0x22, 0x1c },
    { 0x7f, 0x49, 0x49, 0x49, 0x41 },
    { 0x7f, 0x09, 0x09, 0x09, 0x01 },
    { 0x3e, 0x41, 0x49, 0x49, 0x7a },
    { 0x7f, 0x08, 0x08, 0x08, 0x7f },
    { 0x00, 0x41, 0x7f, 0x41, 0x00 },
    { 0x20, 0x40, 0x41, 0x3f, 0x01 },
    { 0x7f, 0x08, 0x14, 0x22, 0x41 },
    { 0x7f, 0x40, 0x40, 0x40, 0x40 },
    { 0x7f, 0x02, 0x0c, 0x02, 0x7f },
    { 0x7f, 0x04, 0x08, 0x10, 0x7f },
    { 0x3e, 0x41, 0x41, 0x41, 0x3e },
    { 0x7f, 0x09, 0x09, 0x09, 0x06 },
    { 0x3e, 0x41, 0x51, 0x21, 0x5e },
    { 0x7f, 0x09, 0x19, 0x29, 0x46 },
    { 0x46, 0x49, 0x49, 0x49, 0x31 },
    { 0x01, 0x01, 0x7f, 0x01, 0x01 },
    { 0x3f, 0x40, 0x40, 0x40, 0x3f },
    { 0x1f, 0x20, 0x40, 0x20, 0x1f },
    { 0x3f, 0x40, 0x38, 0x40, 0x3f },
    { 0x63, 0x14, 0x08, 0x14, 0x63 },
    { 0x07, 0x08, 0x70, 0x08, 0x07 },
    { 0x61, 0x51, 0x49, 0x45, 0x43 },
    { 0x3e, 0x51, 0x49, 0x45, 0x3e },
    { 0x00, 0x42, 0x7f, 0x40, 0x00 },
    { 0x42, 0x61, 0x51, 0x49, 0x46 },
    { 0x21, 0x41, 0x45, 0x4b, 0x31 },
    { 0x18, 0x14, 0x12, 0x7f, 0x10 },
    { 0x27, 0x45, 0x45, 0x45, 0x39 },
    { 0x3c, 0x4a, 0x49, 0x49, 0x30 },
    { 0x01, 0x71, 0x09, 0x05, 0x03 },
    { 0x36, 0x49, 0x49, 0x49, 0x36 },
    { 0x06, 0x49, 0x49, 0x29, 0x1e },
    { 0x00, 0x60, 0x60, 0x00, 0x00 },
    { 0x00, 0x36, 0x36, 0x00, 0x00 },
    { 0x08, 0x08, 0x08, 0x08, 0x08 },
    { 0x00, 0x00, 0x5f, 0x00, 0x00 },
    { 0x00, 0x50, 0x30, 0x00, 0x00 },
    { 0x14, 0x14, 0x14, 0x14, 0x14 },
};

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

    slot = (slot + 1) % 2;

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

typedef struct {
    int   key;
    int   tapped;
    Point tap;
} Input;

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
        return 0;

    if (kbd_fd >= 0 && FD_ISSET (kbd_fd, &rfds)) {
        while (read (kbd_fd, &iev, sizeof (iev)) == (int) sizeof (iev)) {
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

static void
load_calibration (void)
{
    FILE *f = fopen (TOUCHSCREEN_CFG, "r");
    char line[128];
    int v;

    if (!f)
        return;

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

    mkdir ("/etc/piko", 0755);

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

static void
draw_cross (SDL_Surface *screen, int cx, int cy, Uint32 color)
{
    fill_rect (screen, cx - CROSS_ARM, cy - CROSS_THICK / 2, CROSS_ARM * 2 + 1, CROSS_THICK, color);
    fill_rect (screen, cx - CROSS_THICK / 2, cy - CROSS_ARM, CROSS_THICK, CROSS_ARM * 2 + 1, color);
}

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

static void
fill_circle (SDL_Surface *s, int cx, int cy, int r, Uint32 col)
{
    int dy;

    for (dy = -r; dy <= r; dy++) {
        int dx = isqrt (r * r - dy * dy);

        fill_rect (s, cx - dx, cy + dy, dx * 2 + 1, 1, col);
    }
}

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

static int
run_menu (SDL_Surface *screen, int ts_fd, int kbd_fd, int *choice)
{
    TouchState ts;
    int selected = MENU_CALIBRATE;

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
            case KEY_F11:
                *choice = selected;
                return 1;
            case KEY_BACKSPACE:
            case KEY_ESC:
            case KEY_F4:
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

            selected = (p.x < SCREEN_W / 2) ? MENU_CALIBRATE : MENU_TEST;
            draw_menu (screen, selected);
        }
    }
}

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
            return;
        }
        fprintf (stderr, "pikalibrate:   raw (%d,%d)\n", raw[i].x, raw[i].y);
    }

    {
        double rx1 = (raw[0].x + raw[2].x) / 2.0;
        double rx2 = (raw[1].x + raw[3].x) / 2.0;
        double ry1 = (raw[0].y + raw[1].y) / 2.0;
        double ry2 = (raw[2].y + raw[3].y) / 2.0;
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

        fill_rect (screen, 0, 0, SCREEN_W, SCREEN_H, bg);

        for (i = 0; i < MAX_RIPPLES; i++) {
            Uint32 age;
            int    r, shade;

            if (!ripples[i].active)
                continue;

            age = now - ripples[i].started;

            dirty[ndirty].x = (Sint16) (ripples[i].cx - RIPPLE_R_MAX - 1);
            dirty[ndirty].y = (Sint16) (ripples[i].cy - RIPPLE_R_MAX - 1);
            dirty[ndirty].w = (Uint16) (RIPPLE_R_MAX * 2 + 3);
            dirty[ndirty].h = (Uint16) (RIPPLE_R_MAX * 2 + 3);
            ndirty++;

            if (age >= RIPPLE_MS) {
                ripples[i].active = 0;
                continue;
            }
            any_active = 1;

            r = RIPPLE_R_MIN + (int) ((RIPPLE_R_MAX - RIPPLE_R_MIN) * age / RIPPLE_MS);
            shade = 255 - (int) (255 * age / RIPPLE_MS);

            fill_circle (screen, ripples[i].cx, ripples[i].cy, r,
                         SDL_MapRGB (screen->format,
                                     (Uint8) (0xda * shade / 255),
                                     (Uint8) (0xda * shade / 255),
                                     (Uint8) (0xd5 * shade / 255)));
        }

        draw_hint (screen, HINT_TEXT);
        if (ndirty > 0) {
            dirty[ndirty++] = hint_r;
            SDL_UpdateRects (screen, ndirty, dirty);
        }

        (void) any_active;
    }
}

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

    ts_fd = open (ts_path, O_RDONLY | O_NONBLOCK);
    if (ts_fd < 0) {
        fprintf (stderr, "pikalibrate: open %s: %s\n", ts_path, strerror (errno));
        request_resume ();
        return 1;
    }

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
