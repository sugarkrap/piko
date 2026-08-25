#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include "pikovideo.h"

#define FB_DEV          "/dev/fb0"
#define X_CTL_FIFO      "/tmp/.pikalibrate-ctl"
#define FASTPLL_SYSFS   "/sys/devices/platform/w100fb/fastpllclk"

#define DESKTOP_XRES 640
#define DESKTOP_YRES 480
#define QVGA_XRES    320
#define QVGA_YRES    240

#define X_SETTLE_TRIES 100
#define X_SETTLE_NS    20000000L

#define CONFIG_PATH  "/etc/zaurus/matchbox-apprun.cfg"
#define LEGACY_PATH  "/etc/zaurus/matchbox-heavyrun.cfg"
#define LEGACY2_PATH "/etc/zaurus/matchbox-fbrun.cfg"

static const char *mode_keys[PIKOVIDEO_MODE_COUNT] = {
    "qvga-normal", "qvga-fast", "vga-normal", "vga-fast"
};

static const char *mode_labels[PIKOVIDEO_MODE_COUNT] = {
    "QVGA (100MHz PLL)", "QVGA (125MHz PLL)",
    "VGA (75MHz PLL)",   "VGA (100MHz PLL)"
};

static void (*trace_fn)(const char *msg);
static struct fb_var_screeninfo saved_var;
static int saved_valid;
static int mode_applied;
static int pll_applied;

static void trace(const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    if (trace_fn == NULL)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    trace_fn(msg);
}

void pikovideo_set_trace(void (*fn)(const char *msg))
{
    trace_fn = fn;
}

const char *pikovideo_mode_key(enum pikovideo_mode m)
{
    if (m < 0 || m >= PIKOVIDEO_MODE_COUNT) return "";
    return mode_keys[m];
}

const char *pikovideo_mode_label(enum pikovideo_mode m)
{
    if (m < 0 || m >= PIKOVIDEO_MODE_COUNT) return "";
    return mode_labels[m];
}

int pikovideo_mode_from_key(const char *key, enum pikovideo_mode *out)
{
    int i;

    if (key == NULL || out == NULL)
        return 0;
    for (i = 0; i < PIKOVIDEO_MODE_COUNT; i++) {
        if (strcmp(key, mode_keys[i]) == 0) {
            *out = (enum pikovideo_mode)i;
            return 1;
        }
    }
    if (strcmp(key, "qvga") == 0) { *out = PIKOVIDEO_QVGA_NORMAL; return 1; }
    if (strcmp(key, "vga") == 0)  { *out = PIKOVIDEO_VGA_NORMAL;  return 1; }
    return 0;
}

int pikovideo_mode_is_qvga(enum pikovideo_mode m)
{
    return m == PIKOVIDEO_QVGA_NORMAL || m == PIKOVIDEO_QVGA_FAST;
}

int pikovideo_mode_is_fast_pll(enum pikovideo_mode m)
{
    return m == PIKOVIDEO_QVGA_FAST || m == PIKOVIDEO_VGA_FAST;
}

enum pikovideo_mode pikovideo_mode_from_flags(int qvga, int fast_pll)
{
    if (qvga)
        return fast_pll ? PIKOVIDEO_QVGA_FAST : PIKOVIDEO_QVGA_NORMAL;
    return fast_pll ? PIKOVIDEO_VGA_FAST : PIKOVIDEO_VGA_NORMAL;
}

void pikovideo_mode_to_flags(enum pikovideo_mode m, int *qvga, int *fast_pll)
{
    if (qvga)     *qvga = pikovideo_mode_is_qvga(m);
    if (fast_pll) *fast_pll = pikovideo_mode_is_fast_pll(m);
}

void pikovideo_mode_size(enum pikovideo_mode m, int *w, int *h)
{
    if (pikovideo_mode_is_qvga(m)) {
        if (w) *w = QVGA_XRES;
        if (h) *h = QVGA_YRES;
    } else {
        if (w) *w = DESKTOP_XRES;
        if (h) *h = DESKTOP_YRES;
    }
}

int pikovideo_current_xres(void)
{
    struct fb_var_screeninfo var;
    int fd, res = -1;

    fd = open(FB_DEV, O_RDONLY);
    if (fd < 0)
        return -1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0)
        res = (int)var.xres;
    close(fd);
    return res;
}

static int fb_set_size(int w, int h)
{
    struct fb_var_screeninfo var;
    int fd;

    fd = open(FB_DEV, O_RDWR);
    if (fd < 0) {
        trace("fb_set_size: open " FB_DEV " failed");
        return 0;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        trace("fb_set_size: FBIOGET_VSCREENINFO failed");
        close(fd);
        return 0;
    }
    if (!saved_valid) {
        saved_var = var;
        saved_valid = 1;
    }
    if ((int)var.xres == w && (int)var.yres == h) {
        close(fd);
        return 1;
    }
    var.xres = (unsigned)w;
    var.yres = (unsigned)h;
    var.xres_virtual = (unsigned)w;
    if ((int)var.yres_virtual < h * 2)
        var.yres_virtual = (unsigned)(h * 2);
    var.xoffset = 0;
    var.yoffset = 0;
    var.activate = FB_ACTIVATE_NOW;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) < 0) {
        trace("fb_set_size: FBIOPUT_VSCREENINFO failed");
        close(fd);
        return 0;
    }
    close(fd);
    trace("fb_set_size: now %dx%d", w, h);
    return 1;
}

static int send_x_command(const char *cmd)
{
    int fd = open(X_CTL_FIFO, O_WRONLY | O_NONBLOCK);

    if (fd < 0) {
        trace("send_x_command: open " X_CTL_FIFO " failed");
        return 0;
    }
    if (write(fd, cmd, strlen(cmd)) < 0) {
        trace("send_x_command: write failed");
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

static int wait_for_x_qvga(int want_qvga)
{
    struct timespec ts;
    int tries;

    ts.tv_sec = 0;
    ts.tv_nsec = X_SETTLE_NS;
    for (tries = 0; tries < X_SETTLE_TRIES; tries++) {
        int xres = pikovideo_current_xres();

        if (xres < 0)
            return 0;
        if (want_qvga ? (xres <= QVGA_XRES) : (xres > QVGA_XRES))
            return 1;
        nanosleep(&ts, NULL);
    }
    return 0;
}

static int set_fast_pll(int enable)
{
    int fd = open(FASTPLL_SYSFS, O_WRONLY);

    if (fd < 0) {
        trace("set_fast_pll: open " FASTPLL_SYSFS " failed");
        return 0;
    }
    if (write(fd, enable ? "1" : "0", 1) < 0) {
        trace("set_fast_pll: write failed");
        close(fd);
        return 0;
    }
    close(fd);
    trace("set_fast_pll: %s", enable ? "enabled" : "disabled");
    return 1;
}

int pikovideo_apply(enum pikovideo_mode m, enum pikovideo_driver drv)
{
    int qvga = pikovideo_mode_is_qvga(m);
    int fast = pikovideo_mode_is_fast_pll(m);
    int w, h, ok;

    pikovideo_mode_size(m, &w, &h);

    if (drv == PIKOVIDEO_DRIVER_X11) {
        if (!send_x_command(qvga ? "QVGA" : "VGA"))
            return 0;
        if (!wait_for_x_qvga(qvga)) {
            trace("pikovideo_apply: X did not reach %s", qvga ? "QVGA" : "VGA");
            return 0;
        }
        ok = 1;
    } else {
        ok = fb_set_size(w, h);
        if (!ok)
            return 0;
    }

    if (fast && set_fast_pll(1))
        pll_applied = 1;

    mode_applied = 1;
    trace("pikovideo_apply: %s via %s", pikovideo_mode_key(m),
          drv == PIKOVIDEO_DRIVER_X11 ? "x11" : "fb");
    return ok;
}

void pikovideo_restore(void)
{
    if (pll_applied) {
        set_fast_pll(0);
        pll_applied = 0;
    }
    if (!mode_applied)
        return;
    mode_applied = 0;

    if (pikovideo_current_xres() <= QVGA_XRES)
        send_x_command("VGA");
    fb_set_size(DESKTOP_XRES, DESKTOP_YRES);
}

enum pikovideo_mode pikovideo_load_config(void)
{
    const char *paths[3] = { CONFIG_PATH, LEGACY_PATH, LEGACY2_PATH };
    enum pikovideo_mode mode = PIKOVIDEO_VGA_NORMAL;
    char line[128];
    FILE *f = NULL;
    int i;

    for (i = 0; i < 3 && f == NULL; i++)
        f = fopen(paths[i], "r");
    if (f == NULL)
        return mode;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *eq, *p = line;
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0')
            continue;
        eq = strchr(p, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        if (strcmp(p, "video_mode") == 0)
            pikovideo_mode_from_key(eq + 1, &mode);
    }
    fclose(f);
    trace("pikovideo_load_config: %s", pikovideo_mode_key(mode));
    return mode;
}

int pikovideo_save_config(enum pikovideo_mode m)
{
    char tmp_path[sizeof(CONFIG_PATH) + 8];
    FILE *f;

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", CONFIG_PATH);
    f = fopen(tmp_path, "w");
    if (f == NULL)
        return 0;
    fprintf(f, "video_mode=%s\n", pikovideo_mode_key(m));
    fclose(f);
    if (rename(tmp_path, CONFIG_PATH) < 0) {
        unlink(tmp_path);
        return 0;
    }
    return 1;
}
