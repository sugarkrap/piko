
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Round_Button.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Table.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define XSERVER_NAME    "Xfbdev"
#define SESSION_TTY     "tty1"

#define STOP_TIMEOUT_S      15
#define FALLBACK_TIMEOUT_S  20
#define CLOSE_TIMEOUT_S      5

#define LOCK_PATH   "/tmp/matchbox-apprun.lock"

#define REENTRY_ENV "MATCHBOX_APPRUN_ACTIVE"

#define TRACE_LOG "/mnt/card/.zaurus/var/log/apprun-trace.log"

static void trace(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    int fd, len;

    va_start(ap, fmt);
    len = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (len < 0)
        return;
    if ((size_t)len >= sizeof(msg) - 1)
        len = (int)sizeof(msg) - 2;
    msg[len++] = '\n';

    mkdir("/mnt/card/.zaurus/var", 0755);
    mkdir("/mnt/card/.zaurus/var/log", 0755);

    fd = open(TRACE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    if (write(fd, msg, (size_t)len) > 0)
        fsync(fd);
    close(fd);
}

static pid_t child_pid;
static int   session_was_stopped;
static int   lock_held;
static int   cleaned_up;
static int   qvga_requested;
static int   qvga_applied;
static int   x_qvga_applied;
static int   video_touched;
static int   fast_pll_requested;
static int   fast_pll_applied;
static struct fb_var_screeninfo saved_var;

static int find_pids(const char *name, const char *needle, pid_t *out, int max)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    int n = 0;

    if (!d)
        return 0;

    while ((e = readdir(d)) != NULL && n < max) {
        char path[64], buf[1024], *base;
        int fd;
        ssize_t len;
        pid_t pid;

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        pid = (pid_t)atoi(e->d_name);
        if (pid <= 0 || pid == getpid())
            continue;

        snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;
        len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len <= 0)
            continue;
        buf[len] = '\0';

        base = strrchr(buf, '/');
        base = base ? base + 1 : buf;

        if (name && strcmp(base, name) == 0) {
            out[n++] = pid;
            continue;
        }

        if (needle) {
            ssize_t i;
            for (i = 0; i < len; i++)
                if (buf[i] == '\0')
                    buf[i] = ' ';
            if (strstr(buf, needle)) {
                out[n++] = pid;
                continue;
            }
        }
    }

    closedir(d);
    return n;
}

static int process_alive(pid_t pid)
{
    return kill(pid, 0) == 0 || errno == EPERM;
}

static void console_text_mode(void)
{
    int fd = open("/dev/tty0", O_RDWR);

    if (fd < 0)
        fd = open("/dev/console", O_RDWR);
    if (fd < 0)
        return;
    ioctl(fd, KDSETMODE, KD_TEXT);
    if (write(fd, "\033[?25h", 6) < 0)
        {   }
    close(fd);
}

static void show_splash(const char *name)
{
    int fd = open("/dev/tty0", O_RDWR);
    const char *dev = "/dev/tty0";
    char msg[256];
    int len;
    ssize_t written;

    if (fd < 0) {
        dev = "/dev/console";
        fd = open("/dev/console", O_RDWR);
    }
    if (fd < 0) {
        trace("show_splash: could not open /dev/tty0 or /dev/console: %s", strerror(errno));
        return;
    }

    len = snprintf(msg, sizeof(msg),
                    "\033[?25l\033[2J\033[H\n\n\n\n  Starting %s...\n\n"
                    "  This can take a while depending on what it has to load.\n",
                    name);
    if (len <= 0) {
        trace("show_splash: snprintf failed, len=%d", len);
        close(fd);
        return;
    }

    written = write(fd, msg, (size_t)len);
    trace("show_splash: opened %s, wrote %d of %d bytes (errno if short/negative: %s)",
          dev, (int)written, len, written < 0 ? strerror(errno) : "n/a");
    close(fd);
}

#define FB_DEV "/dev/fb0"

static int set_qvga_mode(void)
{
    struct fb_var_screeninfo var;
    int fd;

    fd = open(FB_DEV, O_RDWR);
    if (fd < 0) {
        trace("set_qvga_mode: open %s failed: %s", FB_DEV, strerror(errno));
        return 0;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &saved_var) < 0) {
        trace("set_qvga_mode: FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        close(fd);
        return 0;
    }

    var = saved_var;
    var.xres         = saved_var.xres / 2;
    var.yres         = saved_var.yres / 2;
    var.xres_virtual = var.xres;
    var.yres_virtual = var.yres;
    var.xoffset      = 0;
    var.yoffset      = 0;
    var.activate     = FB_ACTIVATE_NOW;

    if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) < 0) {
        trace("set_qvga_mode: FBIOPUT_VSCREENINFO %ux%u failed: %s",
              var.xres, var.yres, strerror(errno));
        close(fd);
        return 0;
    }
    close(fd);
    trace("set_qvga_mode: %ux%u -> %ux%u", saved_var.xres, saved_var.yres,
          var.xres, var.yres);
    return 1;
}

#define DESKTOP_XRES 640
#define DESKTOP_YRES 480
#define QVGA_XRES    320

static void restore_video_mode(void)
{
    struct fb_var_screeninfo var;
    int fd;

    qvga_applied = 0;

    fd = open(FB_DEV, O_RDWR);
    if (fd < 0) {
        trace("restore_video_mode: open %s failed: %s", FB_DEV, strerror(errno));
        return;
    }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
        trace("restore_video_mode: FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        close(fd);
        return;
    }
    if (var.xres == DESKTOP_XRES && var.yres == DESKTOP_YRES) {
        trace("restore_video_mode: already %ux%u, nothing to do", var.xres, var.yres);
        close(fd);
        return;
    }

    var.xres = DESKTOP_XRES;
    var.yres = DESKTOP_YRES;
    var.xres_virtual = DESKTOP_XRES;
    if (var.yres_virtual < DESKTOP_YRES * 2)
        var.yres_virtual = DESKTOP_YRES * 2;
    var.xoffset = 0;
    var.yoffset = 0;
    var.activate = FB_ACTIVATE_NOW;

    if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) < 0)
        trace("restore_video_mode: FBIOPUT_VSCREENINFO %ux%u failed: %s",
              var.xres, var.yres, strerror(errno));
    else
        trace("restore_video_mode: forced back to %ux%u", var.xres, var.yres);
    close(fd);
}

#define X_CTL_FIFO "/tmp/.pikalibrate-ctl"

#define X_SETTLE_TRIES  100
#define X_SETTLE_NS     20000000L

static int send_x_command(const char *cmd)
{
    int fd = open(X_CTL_FIFO, O_WRONLY | O_NONBLOCK);

    if (fd < 0) {
        trace("send_x_command: %s: open %s failed: %s", cmd, X_CTL_FIFO,
              strerror(errno));
        return 0;
    }
    if (write(fd, cmd, strlen(cmd)) < 0) {
        trace("send_x_command: %s: write failed: %s", cmd, strerror(errno));
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

static int fb_xres(void)
{
    struct fb_var_screeninfo var;
    int fd;
    int res = -1;

    fd = open(FB_DEV, O_RDONLY);
    if (fd < 0)
        return -1;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0)
        res = (int)var.xres;
    close(fd);
    return res;
}

static int wait_for_x_qvga(int want_qvga)
{
    struct timespec ts;
    int tries;

    ts.tv_sec  = 0;
    ts.tv_nsec = X_SETTLE_NS;

    for (tries = 0; tries < X_SETTLE_TRIES; tries++) {
        int xres = fb_xres();

        if (xres < 0)
            return 0;
        if (want_qvga ? (xres <= QVGA_XRES) : (xres > QVGA_XRES))
            return 1;
        nanosleep(&ts, NULL);
    }
    return 0;
}

static int set_x_qvga_mode(int qvga)
{
    if (!send_x_command(qvga ? "QVGA" : "VGA"))
        return 0;
    if (!wait_for_x_qvga(qvga)) {
        trace("set_x_qvga_mode: X did not reach %s", qvga ? "QVGA" : "VGA");
        return 0;
    }
    trace("set_x_qvga_mode: X is now %s", qvga ? "QVGA" : "VGA");
    return 1;
}

#define FASTPLL_SYSFS "/sys/devices/platform/w100fb/fastpllclk"

static int set_fast_pll(int enable)
{
    int fd = open(FASTPLL_SYSFS, O_WRONLY);

    if (fd < 0) {
        trace("set_fast_pll: open %s failed: %s", FASTPLL_SYSFS, strerror(errno));
        return 0;
    }
    if (write(fd, enable ? "1" : "0", 1) < 0) {
        trace("set_fast_pll: write(%d) failed: %s", enable, strerror(errno));
        close(fd);
        return 0;
    }
    close(fd);
    trace("set_fast_pll: %s", enable ? "enabled" : "disabled");
    return 1;
}

static void restore_fast_pll(void)
{
    fast_pll_applied = 0;
    set_fast_pll(0);
}

static int x_is_running(void)
{
    pid_t pids[8];

    return find_pids(XSERVER_NAME, NULL, pids, 8) > 0;
}

static void close_other_clients(Display *dpy)
{
    trace("close_other_clients: start");
    Atom wm_delete   = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    Atom wm_protos   = XInternAtom(dpy, "WM_PROTOCOLS", False);
    Atom client_list = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems = 0, bytes_after;
    unsigned char *data = NULL;

    if (client_list == None || wm_delete == None)
        return;

    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), client_list, 0, 1024,
                           False, XA_WINDOW, &actual_type, &actual_format,
                           &nitems, &bytes_after, &data) != Success || !data)
        return;

    {
        Window *wins = (Window *)data;
        unsigned long i;

        for (i = 0; i < nitems; i++) {
            XClientMessageEvent ev;

            memset(&ev, 0, sizeof(ev));
            ev.type         = ClientMessage;
            ev.window       = wins[i];
            ev.message_type = wm_protos;
            ev.format       = 32;
            ev.data.l[0]    = (long)wm_delete;
            ev.data.l[1]    = CurrentTime;
            XSendEvent(dpy, wins[i], False, NoEventMask, (XEvent *)&ev);
        }
        XFlush(dpy);
        if (nitems)
            sleep(CLOSE_TIMEOUT_S > 2 ? 2 : CLOSE_TIMEOUT_S);
    }

    XFree(data);
    trace("close_other_clients: done, notified %lu window(s)", nitems);
}

static int session_stop(void)
{
    pid_t pids[8];
    int n, i, waited;

    n = find_pids(XSERVER_NAME, NULL, pids, 8);
    if (n == 0) {
        trace("session_stop: %s not running, nothing to do (console-only case)", XSERVER_NAME);
        return 0;
    }

    trace("session_stop: SIGTERM to %d %s pid(s)", n, XSERVER_NAME);
    for (i = 0; i < n; i++)
        kill(pids[i], SIGTERM);

    for (waited = 0; waited < STOP_TIMEOUT_S; waited++) {
        if (find_pids(XSERVER_NAME, NULL, pids, 8) == 0)
            break;
        sleep(1);
    }
    trace("session_stop: waited %ds for SIGTERM", waited);

    n = find_pids(XSERVER_NAME, NULL, pids, 8);
    if (n) {
        trace("session_stop: %d still alive after %ds, SIGKILL", n, waited);
        for (i = 0; i < n; i++)
            kill(pids[i], SIGKILL);
        sleep(1);
    }

    for (waited = 0; waited < FALLBACK_TIMEOUT_S; waited++) {
        if (find_pids(NULL, SESSION_TTY, pids, 8) > 0)
            break;
        sleep(1);
    }
    trace("session_stop: fallback getty %s after %ds -- returning",
          waited < FALLBACK_TIMEOUT_S ? "appeared" : "NEVER APPEARED", waited);

    return 1;
}

static void session_restore(void)
{
    pid_t pids[8];
    int n, i;

    n = find_pids(NULL, SESSION_TTY, pids, 8);
    trace("session_restore: SIGTERM to %d getty pid(s) on %s", n, SESSION_TTY);
    for (i = 0; i < n; i++)
        kill(pids[i], SIGTERM);
}

static void cleanup(void)
{
    if (cleaned_up)
        return;
    cleaned_up = 1;

    trace("cleanup: entered (session_was_stopped=%d, video_touched=%d)",
          session_was_stopped, video_touched);

    if (x_qvga_applied) {
        set_x_qvga_mode(0);
        restore_fast_pll();
    } else if (video_touched) {
        restore_video_mode();
        restore_fast_pll();
    } else {
        trace("cleanup: never touched the panel, leaving it alone");
    }

    if (session_was_stopped) {
        console_text_mode();
        session_restore();
    }

    if (lock_held)
        unlink(LOCK_PATH);

    trace("cleanup: done");
}

static void sig_handler(int sig)
{
    trace("sig_handler: caught signal %d, child_pid=%d", sig, (int)child_pid);
    if (child_pid > 0)
        kill(child_pid, SIGTERM);
    cleanup();
    _exit(128 + sig);
}

static void install_handlers(void)
{
    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT };
    unsigned i;

    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        signal(sigs[i], sig_handler);
}

static int take_lock(void)
{
    int fd = open(LOCK_PATH, O_CREAT | O_EXCL | O_WRONLY, 0644);
    char buf[32];

    if (fd >= 0) {
        int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
        ssize_t written = write(fd, buf, (size_t)len);

        (void)written;
        close(fd);
        lock_held = 1;
        trace("take_lock: acquired");
        return 1;
    }

    fd = open(LOCK_PATH, O_RDONLY);
    if (fd >= 0) {
        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (len > 0) {
            buf[len] = '\0';
            pid_t owner = (pid_t)atoi(buf);
            if (owner > 0 && process_alive(owner)) {
                trace("take_lock: held by live pid=%d -- refusing (this is likely why nothing appeared to happen)", (int)owner);
                return 0;
            }
        }
    }

    trace("take_lock: stale lock, taking it over");
    unlink(LOCK_PATH);
    fd = open(LOCK_PATH, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) {
        trace("take_lock: could not recreate lock: %s", strerror(errno));
        return 0;
    }
    close(fd);
    lock_held = 1;
    trace("take_lock: acquired (after clearing stale lock)");
    return 1;
}

enum video_mode {
    VIDEO_MODE_QVGA_NORMAL = 0,
    VIDEO_MODE_QVGA_FAST,
    VIDEO_MODE_VGA_NORMAL,
    VIDEO_MODE_VGA_FAST,
    VIDEO_MODE_COUNT
};

static const char *video_mode_labels[VIDEO_MODE_COUNT] = {
    "QVGA (100MHz PLL)",
    "QVGA (125MHz PLL)",
    "VGA (75MHz PLL)",
    "VGA (100MHz PLL)",
};

static const char *video_mode_keys[VIDEO_MODE_COUNT] = {
    "qvga-normal",
    "qvga-fast",
    "vga-normal",
    "vga-fast",
};

enum app_driver {
    DRIVER_FB = 0,
    DRIVER_X11,
    DRIVER_COUNT
};

static const char *driver_keys[DRIVER_COUNT]   = { "fb", "x11" };
static const char *driver_labels[DRIVER_COUNT] = { "Framebuffer", "X11" };

static int            driver_allowed[DRIVER_COUNT] = { 1, 0 };
static enum app_driver driver_selected = DRIVER_FB;
static int            qvga_only = 0;

static int mode_is_qvga(enum video_mode m)
{
    return m == VIDEO_MODE_QVGA_NORMAL || m == VIDEO_MODE_QVGA_FAST;
}

static void parse_drivers(const char *list)
{
    int i;
    for (i = 0; i < DRIVER_COUNT; i++)
        driver_allowed[i] = 0;

    while (list && *list) {
        const char *end = list;
        while (*end && *end != ';' && *end != ',')
            end++;
        for (i = 0; i < DRIVER_COUNT; i++) {
            size_t klen = strlen(driver_keys[i]);
            if ((size_t)(end - list) == klen && !strncmp(list, driver_keys[i], klen))
                driver_allowed[i] = 1;
        }
        list = *end ? end + 1 : end;
    }

    for (i = 0; i < DRIVER_COUNT; i++)
        if (driver_allowed[i])
            break;
    if (i == DRIVER_COUNT)
        driver_allowed[DRIVER_FB] = 1;

    for (i = 0; i < DRIVER_COUNT; i++) {
        if (driver_allowed[i]) {
            driver_selected = (enum app_driver)i;
            break;
        }
    }
}

static enum video_mode mode_from_flags(int qvga, int fast_pll)
{
    if (qvga)
        return fast_pll ? VIDEO_MODE_QVGA_FAST : VIDEO_MODE_QVGA_NORMAL;
    return fast_pll ? VIDEO_MODE_VGA_FAST : VIDEO_MODE_VGA_NORMAL;
}

static void mode_to_flags(enum video_mode mode, int *qvga, int *fast_pll)
{
    *qvga     = (mode == VIDEO_MODE_QVGA_NORMAL || mode == VIDEO_MODE_QVGA_FAST);
    *fast_pll = (mode == VIDEO_MODE_QVGA_FAST    || mode == VIDEO_MODE_VGA_FAST);
}

#define APPRUN_CONFIG_DIR  "/etc/zaurus"
#define APPRUN_CONFIG_PATH "/etc/zaurus/matchbox-apprun.cfg"
#define APPRUN_LEGACY_PATH "/etc/zaurus/matchbox-heavyrun.cfg"
#define APPRUN_LEGACY2_PATH "/etc/zaurus/matchbox-fbrun.cfg"

static enum video_mode load_video_mode_config(void)
{
    FILE *f = fopen(APPRUN_CONFIG_PATH, "r");
    if (!f)
        f = fopen(APPRUN_LEGACY_PATH, "r");
    if (!f)
        f = fopen(APPRUN_LEGACY2_PATH, "r");
    enum video_mode mode = VIDEO_MODE_VGA_NORMAL;
    char line[128];

    if (!f)
        return mode;

    while (fgets(line, sizeof(line), f)) {
        char *eq, *key, *val, *nl;
        int i;

        if (line[0] == '#' || line[0] == '\n')
            continue;
        nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        key = line;
        val = eq + 1;

        if (strcmp(key, "video_mode") != 0)
            continue;
        for (i = 0; i < VIDEO_MODE_COUNT; i++) {
            if (!strcmp(val, video_mode_keys[i])) {
                mode = (enum video_mode)i;
                break;
            }
        }
    }
    fclose(f);
    trace("load_video_mode_config: video_mode=%s", video_mode_keys[mode]);
    return mode;
}

static void save_video_mode_config(enum video_mode mode)
{
    char tmp_path[sizeof(APPRUN_CONFIG_PATH) + 4];
    FILE *f;

    mkdir(APPRUN_CONFIG_DIR, 0755);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", APPRUN_CONFIG_PATH);

    f = fopen(tmp_path, "w");
    if (!f) {
        trace("save_video_mode_config: fopen %s failed: %s", tmp_path, strerror(errno));
        return;
    }
    fprintf(f, "# matchbox-apprun persisted video mode -- see the dialog's Advanced panel\n");
    fprintf(f, "video_mode=%s\n", video_mode_keys[mode]);
    fclose(f);

    if (rename(tmp_path, APPRUN_CONFIG_PATH) < 0)
        trace("save_video_mode_config: rename %s -> %s failed: %s",
              tmp_path, APPRUN_CONFIG_PATH, strerror(errno));
    else
        trace("save_video_mode_config: saved video_mode=%s", video_mode_keys[mode]);
}

class VideoModeList : public Fl_Table {
public:
    VideoModeList(int X, int Y, int W, int H)
        : Fl_Table(X, Y, W, H), selected_(VIDEO_MODE_VGA_NORMAL), row_count_(0)
    {
        rebuild();
        col_header(0);
        col_resize(0);
        row_header(0);
        row_resize(0);
        row_height_all(26);
        cols(1);
        col_width_all(W - 4);
        rows(row_count_);
        end();
        callback(table_cb, this);
        when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    }

    void selected(enum video_mode m) { selected_ = m; redraw(); }
    enum video_mode selected(void) const { return selected_; }

    void rebuild(void)
    {
        int m;
        row_count_ = 0;
        for (m = 0; m < VIDEO_MODE_COUNT; m++) {
            if (qvga_only && !mode_is_qvga((enum video_mode)m))
                continue;
            row_mode_[row_count_++] = (enum video_mode)m;
        }
        rows(row_count_);
        redraw();
    }

protected:
    void draw_cell(TableContext context, int R = 0, int C = 0,
                   int X = 0, int Y = 0, int W = 0, int H = 0)
    {
        (void)C;
        switch (context) {
        case CONTEXT_CELL: {
            enum video_mode m = (R >= 0 && R < row_count_)
                                ? row_mode_[R] : VIDEO_MODE_QVGA_NORMAL;
            int is_selected = (m == selected_);

            fl_push_clip(X, Y, W, H);
            fl_draw_box(FL_THIN_UP_BOX, X, Y, W, H,
                        is_selected ? FL_SELECTION_COLOR : FL_BACKGROUND2_COLOR);
            fl_color(is_selected ? FL_WHITE : FL_BLACK);
            fl_draw(video_mode_labels[m], X + 6, Y, W - 12, H, FL_ALIGN_LEFT);
            fl_pop_clip();
            break;
        }
        default:
            break;
        }
    }

private:
    static void table_cb(Fl_Widget *, void *v)
    {
        VideoModeList *self = (VideoModeList *)v;

        if (self->callback_context() == CONTEXT_CELL) {
            int r = self->callback_row();

            if (r >= 0 && r < self->row_count_) {
                self->selected_ = self->row_mode_[r];
                self->redraw();
            }
        }
    }

    enum video_mode selected_;
    enum video_mode row_mode_[VIDEO_MODE_COUNT];
    int             row_count_;
};

static int dialog_result = 0;

static Fl_Box *dialog_text_box = 0;
static char    dialog_text[512];
static const char *dialog_app_name = "This application";

static void build_dialog_text(const char *reason)
{
    if (driver_selected == DRIVER_X11)
        snprintf(dialog_text, sizeof(dialog_text),
                 "%s wants the whole screen.\n\n"
                 "The desktop keeps running underneath it, but the screen mode "
                 "changes while %s is open and every other window is closed first."
                 "\n%s%s",
                 dialog_app_name, dialog_app_name,
                 reason ? "\n" : "", reason ? reason : "");
    else
        snprintf(dialog_text, sizeof(dialog_text),
                 "%s needs the whole screen.\n\n"
                 "Every other application that is running will be closed, the "
                 "desktop will stop, and it comes back when %s exits.\n%s%s",
                 dialog_app_name, dialog_app_name,
                 reason ? "\n" : "", reason ? reason : "");
}

static const char *dialog_reason = 0;

static void refresh_dialog_text(void)
{
    build_dialog_text(dialog_reason);
    if (dialog_text_box) {
        dialog_text_box->label(dialog_text);
        dialog_text_box->redraw();
    }
}

static void driver_fb_cb(Fl_Widget *, void *)
{
    driver_selected = DRIVER_FB;
    refresh_dialog_text();
}

static void driver_x11_cb(Fl_Widget *, void *)
{
    driver_selected = DRIVER_X11;
    refresh_dialog_text();
}

static void abort_cb(Fl_Widget *, void *w)     { dialog_result = 0; ((Fl_Window *)w)->hide(); }
static void continue_cb(Fl_Widget *, void *w)  { dialog_result = 1; ((Fl_Window *)w)->hide(); }

#define DIALOG_BASE_W   420
#define DIALOG_BASE_H   200
#define DIALOG_EXTRA_H  185
#define DIALOG_BTN_Y    150

struct advanced_ui {
    Fl_Window        *win;
    Fl_Box           *driver_label;
    Fl_Group         *driver_group;
    Fl_Box           *mode_label;
    VideoModeList    *list;
    Fl_Button        *advanced_btn;
    Fl_Button        *abort_btn;
    Fl_Return_Button *continue_btn;
    int               expanded;
};

static void advanced_cb(Fl_Widget *, void *v)
{
    struct advanced_ui *ui = (struct advanced_ui *)v;
    int button_y;

    ui->expanded = !ui->expanded;
    button_y = DIALOG_BTN_Y + (ui->expanded ? DIALOG_EXTRA_H : 0);

    if (ui->expanded) {
        ui->driver_label->show();
        ui->driver_group->show();
        ui->mode_label->show();
        ui->list->show();
    } else {
        ui->driver_label->hide();
        ui->driver_group->hide();
        ui->mode_label->hide();
        ui->list->hide();
    }

    ui->win->size(DIALOG_BASE_W, DIALOG_BASE_H + (ui->expanded ? DIALOG_EXTRA_H : 0));
    ui->advanced_btn->position(ui->advanced_btn->x(), button_y);
    ui->abort_btn->position(ui->abort_btn->x(), button_y);
    ui->continue_btn->position(ui->continue_btn->x(), button_y);
    ui->win->redraw();
}

static int confirm(const char *name, const char *reason, enum video_mode *mode)
{
    struct advanced_ui ui;

    dialog_app_name = name;
    dialog_reason = reason;
    build_dialog_text(reason);

    Fl_Window win(DIALOG_BASE_W, DIALOG_BASE_H, "Start application");
    Fl_Box text(10, 10, 400, 130, dialog_text);
    dialog_text_box = &text;
    text.align(FL_ALIGN_WRAP | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE);

    Fl_Box driver_label(10, 145, 200, 20, "Driver:");
    driver_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    driver_label.hide();

    Fl_Group driver_group(10, 167, 400, 26);
    Fl_Round_Button driver_fb(10, 167, 150, 26, driver_labels[DRIVER_FB]);
    Fl_Round_Button driver_x11(170, 167, 150, 26, driver_labels[DRIVER_X11]);
    driver_group.end();
    driver_fb.type(FL_RADIO_BUTTON);
    driver_x11.type(FL_RADIO_BUTTON);
    driver_fb.callback(driver_fb_cb);
    driver_x11.callback(driver_x11_cb);
    if (!driver_allowed[DRIVER_FB])
        driver_fb.deactivate();
    if (!driver_allowed[DRIVER_X11])
        driver_x11.deactivate();
    if (driver_selected == DRIVER_X11)
        driver_x11.setonly();
    else
        driver_fb.setonly();
    driver_group.hide();

    Fl_Box mode_label(10, 200, 200, 20, "Video mode:");
    mode_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    mode_label.hide();

    VideoModeList mode_list(10, 223, 400, 112);
    mode_list.selected(*mode);
    mode_list.hide();

    Fl_Button advanced_btn(10, DIALOG_BTN_Y, 90, 30, "Advanced");
    Fl_Button abort_btn(210, DIALOG_BTN_Y, 100, 30, "Abort");
    Fl_Return_Button continue_btn(310, DIALOG_BTN_Y, 100, 30, "Continue");
    win.end();

    dialog_result = 0;
    ui.win = &win;
    ui.driver_label = &driver_label;
    ui.driver_group = &driver_group;
    ui.mode_label = &mode_label;
    ui.list = &mode_list;
    ui.advanced_btn = &advanced_btn;
    ui.abort_btn = &abort_btn;
    ui.continue_btn = &continue_btn;
    ui.expanded = 0;

    advanced_btn.callback(advanced_cb, &ui);
    abort_btn.callback(abort_cb, &win);
    continue_btn.callback(continue_cb, &win);
    win.set_modal();
    win.show();

    fprintf(stderr, "confirm: win.show() done, fl_xid=0x%lx, shown()=%d\n",
            (unsigned long)fl_xid(&win), win.shown());
    fflush(stderr);

    Atom window_type = XInternAtom(fl_display, "_NET_WM_WINDOW_TYPE", False);
    Atom dialog_type = XInternAtom(fl_display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    XChangeProperty(fl_display, fl_xid(&win), window_type, XA_ATOM, 32,
                     PropModeReplace, (unsigned char *)&dialog_type, 1);
    XSync(fl_display, False);

    {
        Atom actual_type = None;
        int actual_format = 0;
        unsigned long nitems = 0, bytes_after = 0;
        unsigned char *prop = NULL;

        XGetWindowProperty(fl_display, fl_xid(&win), window_type, 0, 1,
                            False, XA_ATOM, &actual_type, &actual_format,
                            &nitems, &bytes_after, &prop);
        fprintf(stderr,
                "confirm: readback type=%lu (want %lu) format=%d nitems=%lu"
                " value=%lu (want %lu)\n",
                (unsigned long)actual_type, (unsigned long)window_type,
                actual_format, nitems,
                prop ? *(Atom *)prop : 0, (unsigned long)dialog_type);
        fflush(stderr);
        if (prop) XFree(prop);
    }

    fprintf(stderr, "confirm: entering wait loop\n");
    fflush(stderr);
    trace("confirm: dialog shown, waiting for the user");

    while (win.shown())
        Fl::wait();

    fprintf(stderr, "confirm: wait loop exited, result=%d\n", dialog_result);
    fflush(stderr);
    trace("confirm: answered, result=%d (%s), video_mode=%s", dialog_result,
          dialog_result ? "Continue" : "Abort",
          video_mode_keys[mode_list.selected()]);

    *mode = mode_list.selected();
    return dialog_result;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: matchbox-apprun [-n NAME] [-r REASON] [-y] [--qvga] "
            "[--fast-pll] [--drivers=fb;x11] [--video=qvga] [--] program [args...]\n");
}

int main(int argc, char **argv)
{
    const char *name = NULL, *reason = NULL;
    int assume_yes = 0;
    int mode_flag_given = 0;
    enum video_mode mode;
    int i, status = 0;
    char **prog_argv;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)      name   = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) reason = argv[++i];
        else if (!strcmp(argv[i], "-y"))                 assume_yes = 1;
        else if (!strcmp(argv[i], "--qvga"))
            { qvga_requested = 1; mode_flag_given = 1; }
        else if (!strcmp(argv[i], "--fast-pll"))
            { fast_pll_requested = 1; mode_flag_given = 1; }
        else if (!strncmp(argv[i], "--drivers=", 10))
            parse_drivers(argv[i] + 10);
        else if (!strncmp(argv[i], "--video=", 8))
            qvga_only = !strcmp(argv[i] + 8, "qvga");
        else if (!strcmp(argv[i], "--"))                 { i++; break; }
        else if (argv[i][0] == '-')                      { usage(); return 2; }
        else                                             break;
    }

    if (mode_flag_given) {
        mode = mode_from_flags(qvga_requested, fast_pll_requested);
    } else {
        mode = load_video_mode_config();
        mode_to_flags(mode, &qvga_requested, &fast_pll_requested);
    }

    if (qvga_only && !mode_is_qvga(mode)) {
        mode = fast_pll_requested ? VIDEO_MODE_QVGA_FAST : VIDEO_MODE_QVGA_NORMAL;
        mode_to_flags(mode, &qvga_requested, &fast_pll_requested);
        trace("main: app is qvga-only, forcing %s", video_mode_keys[mode]);
    }

    if (i >= argc) {
        usage();
        return 2;
    }
    prog_argv = &argv[i];

    if (!name) {
        char *slash = strrchr(prog_argv[0], '/');
        name = slash ? slash + 1 : prog_argv[0];
    }

    {
        char argbuf[160] = "";
        int j;
        for (j = 0; prog_argv[j] && strlen(argbuf) < sizeof(argbuf) - 32; j++) {
            strncat(argbuf, prog_argv[j], sizeof(argbuf) - strlen(argbuf) - 2);
            strcat(argbuf, " ");
        }
        trace("main: invoked pid=%d ppid=%d name=%s reenter=%s target: %s",
              (int)getpid(), (int)getppid(), name,
              getenv(REENTRY_ENV) ? "yes" : "no", argbuf);
    }

    if (getenv(REENTRY_ENV)) {
        execvp(prog_argv[0], prog_argv);
        trace("main: re-entrant execvp failed: %s", strerror(errno));
        fprintf(stderr, "matchbox-apprun: cannot run %s: %s\n",
                prog_argv[0], strerror(errno));
        return 127;
    }

    if (!take_lock()) {
        trace("main: lock held by a live instance, refusing to start a second one");
        if (getenv("DISPLAY") && x_is_running()) {
            Display *dpy = XOpenDisplay(NULL);
            if (dpy) {
                XCloseDisplay(dpy);
                fl_alert("Another full-screen application is already running.\n\n"
                         "Close it before starting %s.", name);
            }
        }
        fprintf(stderr, "matchbox-apprun: another one is already running\n");
        return 0;
    }

    if (setsid() < 0)
        trace("main: setsid failed: %s (staying in the parent's session)", strerror(errno));
    else
        trace("main: setsid ok, new session id=%d", (int)getpid());

    install_handlers();
    atexit(cleanup);

    if (!assume_yes && getenv("DISPLAY") && x_is_running()) {
        Display *dpy = XOpenDisplay(NULL);

        trace("main: DISPLAY set and %s running -- %s", XSERVER_NAME,
              dpy ? "opened, showing confirm dialog" : "XOpenDisplay FAILED, skipping dialog");

        if (dpy) {
            int proceed = confirm(name, reason, &mode);

            if (!proceed) {
                trace("main: user chose Abort");
                XCloseDisplay(dpy);
                return 0;
            }

            mode_to_flags(mode, &qvga_requested, &fast_pll_requested);
            save_video_mode_config(mode);

            close_other_clients(dpy);
            XCloseDisplay(dpy);
        }
    } else {
        trace("main: skipping dialog (assume_yes=%d DISPLAY=%s %s=%s)",
              assume_yes, getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)",
              XSERVER_NAME, x_is_running() ? "running" : "not running");
    }

    if (driver_selected == DRIVER_X11) {
        trace("main: x11 driver -- leaving the session up, asking X to resize");
        if (qvga_requested)
            x_qvga_applied = set_x_qvga_mode(1);
    } else {
        session_was_stopped = session_stop();
        trace("main: session_was_stopped=%d, forking", session_was_stopped);

        if (qvga_requested) {
            video_touched = 1;
            qvga_applied = set_qvga_mode();
        }
    }
    if (fast_pll_requested) {
        video_touched = 1;
        fast_pll_applied = set_fast_pll(1);
    }

    if (driver_selected != DRIVER_X11)
        show_splash(name);

    child_pid = fork();
    if (child_pid < 0) {
        trace("main: fork failed: %s", strerror(errno));
        perror("matchbox-apprun: fork");
        return 126;
    }
    if (child_pid == 0) {
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        setenv(REENTRY_ENV, "1", 1);
        trace("main: child pid=%d exec'ing %s", (int)getpid(), prog_argv[0]);
        execvp(prog_argv[0], prog_argv);
        trace("main: child execvp failed: %s", strerror(errno));
        fprintf(stderr, "matchbox-apprun: cannot run %s: %s\n",
                prog_argv[0], strerror(errno));
        _exit(127);
    }

    trace("main: forked child pid=%d, waiting", (int)child_pid);
    while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR)
        ;

    if (WIFEXITED(status))
        trace("main: child pid=%d exited, status=%d", (int)child_pid, WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        trace("main: child pid=%d KILLED by signal %d (%s)%s", (int)child_pid,
              WTERMSIG(status), strsignal(WTERMSIG(status)),
              WTERMSIG(status) == SIGKILL ? " -- likely OOM kill, check dmesg" : "");
    else
        trace("main: child pid=%d wait returned unexpected status=0x%x", (int)child_pid, status);

    child_pid = 0;

    cleanup();

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 0;
}
