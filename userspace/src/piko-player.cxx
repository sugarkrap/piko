#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Hor_Slider.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/x.H>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define STRIP_H     58
#define MIN_VIDEO_H 64
#define OSC_HIDE_S  4.0

static pid_t mplayer_pid   = -1;
static int   to_mplayer    = -1;
static int   from_mplayer  = -1;
static FILE *log_fp        = NULL;

static double cur_pos      = 0.0;
static double cur_len      = 0.0;
static bool   have_file    = false;
static bool   is_paused    = false;
static bool   user_seeking = false;

static bool   fs_mode      = false;
static bool   fit_mode     = false;
static bool   osc_shown    = true;
static int    saved_x = 0, saved_y = 0, saved_w = 0, saved_h = 0;

static int    vid_w = 0, vid_h = 0;
static bool   needs_clear  = true;

static Window      video_wid   = 0;
static const char *mplayer_bin = NULL;
static char        cur_path[1024];

static Fl_Double_Window *win;
static Fl_Window        *video;
static Fl_Double_Window *strip;

static Fl_Button     *play_btn, *stop_btn, *open_btn, *fs_btn, *fit_btn;
static Fl_Hor_Slider *seek_slider, *vol_slider;
static Fl_Box        *time_box, *vol_lbl, *status_box;

static void relayout(void);
static void poke_osc(void);
static void request_fullscreen(bool on);
static void play_pause_cb(Fl_Widget *, void *);
static void open_cb(Fl_Widget *, void *);
static void load_file(const char *path);
static void request_clear(void);
static void bind_video_wid(void);
static int  start_mplayer(Window wid, const char *mp);
static void shut_down_mplayer(void);
static void restart_mplayer(void);

static void mp_cmd(const char *fmt, ...)
{
    char line[512];
    int n;
    va_list ap;

    if (to_mplayer < 0)
        return;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof(line) - 2)
        n = sizeof(line) - 2;
    line[n++] = '\n';
    if (write(to_mplayer, line, (size_t)n) < 0)
        {   }
}

static void request_clear(void)
{
    needs_clear = true;
    if (video)
        video->redraw();
    if (fl_display && video_wid)
        XClearArea(fl_display, video_wid, 0, 0, 0, 0, True);
}

static void set_status(const char *s)
{
    if (!status_box)
        return;
    status_box->copy_label(s ? s : "");
    status_box->redraw();
}

static void fmt_time(double secs, char *out, size_t outsz)
{
    int t = secs > 0 ? (int)(secs + 0.5) : 0;
    snprintf(out, outsz, "%d:%02d", t / 60, t % 60);
}

static void refresh_time_box(void)
{
    char pos[16], len[16], both[40];

    fmt_time(cur_pos, pos, sizeof(pos));
    if (cur_len > 0.0) {
        fmt_time(cur_len, len, sizeof(len));
        snprintf(both, sizeof(both), "%s / %s", pos, len);
    } else {
        snprintf(both, sizeof(both), "%s", pos);
    }
    time_box->copy_label(both);
}

static void set_play_label(void)
{
    play_btn->label(is_paused || !have_file ? "Play" : "Pause");
    play_btn->redraw();
}

static void reset_transport(void)
{
    have_file = false;
    is_paused = false;
    cur_pos   = 0.0;
    cur_len   = 0.0;
    seek_slider->value(0.0);
    seek_slider->bounds(0.0, 0.0);
    seek_slider->deactivate();
    refresh_time_box();
    set_play_label();
    if (video)
        video->redraw();
}

static void mplayer_gone(const char *why)
{
    if (from_mplayer >= 0) {
        Fl::remove_fd(from_mplayer);
        close(from_mplayer);
        from_mplayer = -1;
    }
    if (to_mplayer >= 0) {
        close(to_mplayer);
        to_mplayer = -1;
    }
    if (mplayer_pid > 0) {
        waitpid(mplayer_pid, NULL, 0);
        mplayer_pid = -1;
    }
    reset_transport();
    set_status(why);
}

static const char *const err_markers[] = {
    "Failed to open",
    "File not found",
    "No stream found",
    "Cannot find codec",
    "Error opening/initializing",
    "Unsupported",
    "Unknown format",
    "Unrecognized file format",
    "Could not open/initialize audio device",
};

static void handle_line(char *line)
{
    double v;

    if (log_fp) {
        fprintf(log_fp, "%s\n", line);
        fflush(log_fp);
    }

    if (sscanf(line, "ANS_TIME_POSITION=%lf", &v) == 1) {
        cur_pos = v;
        if (!user_seeking) {
            if (cur_len > 0.0)
                seek_slider->value(cur_pos);
            refresh_time_box();
        }
        return;
    }
    if (sscanf(line, "ANS_LENGTH=%lf", &v) == 1) {
        cur_len = v;
        if (cur_len > 0.0) {
            seek_slider->bounds(0.0, cur_len);
            seek_slider->activate();
        }
        refresh_time_box();
        return;
    }
    if (!strncmp(line, "ANS_", 4))
        return;

    if ((line[0] == 'A' || line[0] == 'V') && line[1] == ':')
        return;

    if (!strncmp(line, "VO: [", 5)) {
        int sw, sh, dw, dh;
        if (sscanf(line, "VO: [%*[^]]] %dx%d => %dx%d", &sw, &sh, &dw, &dh) == 4
            && sw > 0 && sh > 0) {
            vid_w = sw;
            vid_h = sh;
        }
        set_status(line);
        request_clear();
        return;
    }
    if (!strncmp(line, "Video: no video", 15)) {
        set_status("no video stream in this file");
        return;
    }

    for (size_t i = 0; i < sizeof(err_markers) / sizeof(err_markers[0]); i++)
        if (strstr(line, err_markers[i])) {
            set_status(line);
            return;
        }
}

static void on_mplayer_output(int fd, void *)
{
    static char buf[8192];
    static size_t used = 0;
    ssize_t r;

    if (used >= sizeof(buf) - 1)
        used = 0;

    do {
        r = read(fd, buf + used, sizeof(buf) - 1 - used);
    } while (r < 0 && errno == EINTR);

    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        mplayer_gone("mplayer pipe error");
        return;
    }
    if (r == 0) {
        mplayer_gone("mplayer exited");
        return;
    }

    used += (size_t)r;
    buf[used] = '\0';

    char *line = buf, *end;
    while ((end = strpbrk(line, "\r\n")) != NULL) {
        *end = '\0';
        if (*line)
            handle_line(line);
        line = end + 1;
    }

    used = strlen(line);
    memmove(buf, line, used + 1);
}

static void poll_tick(void *)
{
    if (mplayer_pid > 0 && waitpid(mplayer_pid, NULL, WNOHANG) == mplayer_pid) {
        mplayer_pid = -1;
        mplayer_gone("mplayer exited");
    }
    if (have_file && !is_paused) {
        mp_cmd("get_time_pos");
        if (cur_len <= 0.0)
            mp_cmd("get_time_length");
    }
    Fl::repeat_timeout(0.5, poll_tick);
}

static void load_file(const char *path)
{
    const char *base;

    if (!path || !*path)
        return;
    if (to_mplayer < 0) {
        set_status("mplayer is not running");
        return;
    }

    if (path != cur_path)
        snprintf(cur_path, sizeof(cur_path), "%s", path);
    mp_cmd("loadfile \"%s\" 0", path);

    have_file    = true;
    is_paused    = false;
    cur_pos      = 0.0;
    cur_len      = 0.0;
    seek_slider->value(0.0);
    seek_slider->bounds(0.0, 0.0);
    seek_slider->deactivate();
    refresh_time_box();
    set_play_label();

    base = strrchr(path, '/');
    set_status(base ? base + 1 : path);
}

static void open_cb(Fl_Widget *, void *)
{
    Fl_Native_File_Chooser fc;
    fc.title("Open video");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.directory("/mnt/card");

    if (fc.show() != 0)
        return;
    load_file(fc.filename());
}

static void play_pause_cb(Fl_Widget *, void *)
{
    if (!have_file) {
        if (!*cur_path)
            return;
        if (to_mplayer < 0)
            restart_mplayer();
        load_file(cur_path);
        poke_osc();
        return;
    }
    mp_cmd("pause");
    is_paused = !is_paused;
    set_play_label();
    poke_osc();
}

static void stop_cb(Fl_Widget *, void *)
{
    if (!have_file)
        return;
    mp_cmd("stop");
    reset_transport();
    poke_osc();
}

static void fs_cb(Fl_Widget *, void *)
{
    request_fullscreen(!fs_mode);
}

static void fit_cb(Fl_Widget *, void *)
{
    fit_mode = !fit_mode;
    fit_btn->label(fit_mode ? "1:1" : "Fit");
    fit_btn->redraw();
    request_clear();
    restart_mplayer();
    poke_osc();
}

static void seek_cb(Fl_Widget *w, void *)
{
    Fl_Hor_Slider *s = (Fl_Hor_Slider *)w;
    int ev = Fl::event();

    poke_osc();
    if (ev == FL_PUSH || ev == FL_DRAG) {
        user_seeking = true;
        cur_pos = s->value();
        refresh_time_box();
    } else {
        mp_cmd("seek %.1f 2", s->value());
        user_seeking = false;
    }
}

static void volume_cb(Fl_Widget *w, void *)
{
    Fl_Hor_Slider *s = (Fl_Hor_Slider *)w;
    mp_cmd("volume %.0f 1", s->value());
    poke_osc();
}

static void nudge_seek(double delta)
{
    if (!have_file)
        return;
    mp_cmd("seek %.0f 0", delta);
    poke_osc();
}

static void bump_volume(double delta)
{
    double v = vol_slider->value() + delta;

    if (v < 0.0)   v = 0.0;
    if (v > 100.0) v = 100.0;
    vol_slider->value(v);
    mp_cmd("volume %.0f 1", v);
    poke_osc();
}

static void layout_strip(int W, int H)
{
    const int gap = 4, bh = 26, bw = 50;
    int x = gap, y1 = 3, y2, tw, sw, lw, vw;

    play_btn->resize(x, y1, bw, bh); x += bw + gap;
    stop_btn->resize(x, y1, bw, bh); x += bw + gap;
    open_btn->resize(x, y1, bw, bh); x += bw + gap;
    fs_btn->resize(x, y1, bw, bh);   x += bw + gap;
    fit_btn->resize(x, y1, bw, bh);  x += bw + gap;

    tw = 92;
    time_box->resize(W - tw - gap, y1, tw, bh);

    sw = (W - tw - gap * 2) - x;
    if (sw < 40)
        sw = 40;
    seek_slider->resize(x, y1 + 4, sw, bh - 8);

    y2 = y1 + bh + 3;
    lw = 34;
    vol_lbl->resize(gap, y2, lw, H - y2 - 3);
    vw = W / 3;
    if (vw < 80)
        vw = 80;
    vol_slider->resize(gap + lw + gap, y2 + 2, vw, H - y2 - 7);

    x = gap + lw + gap + vw + gap * 2;
    status_box->resize(x, y2, W - x - gap, H - y2 - 3);
}

static void relayout(void)
{
    int W, H, sh, vh;

    if (!win || !video || !strip)
        return;

    W = win->w();
    H = win->h();

    sh = STRIP_H;
    if (sh > H - MIN_VIDEO_H)
        sh = H - MIN_VIDEO_H;
    if (sh < 0)
        sh = 0;

    vh = fs_mode ? H : H - sh;
    if (vh < 1)
        vh = 1;

    video->resize(0, 0, W, vh);
    strip->resize(0, H - sh, W, sh);
    layout_strip(W, sh);
    strip->redraw();
    request_clear();
}

static void bind_video_wid(void)
{
    Window w;

    Fl::flush();
    w = fl_xid(video);
    if (!w || w == video_wid)
        return;

    XSetWindowBackgroundPixmap(fl_display, w, None);

    if (video_wid == 0) {
        video_wid = w;
        return;
    }

    video_wid = w;
    restart_mplayer();
}

static void restart_mplayer(void)
{
    double resume = cur_pos;
    bool   replay = have_file;
    char   path[sizeof(cur_path)];

    snprintf(path, sizeof(path), "%s", cur_path);
    shut_down_mplayer();

    if (!mplayer_bin || video_wid == 0 || start_mplayer(video_wid, mplayer_bin) < 0) {
        set_status("could not restart mplayer");
        return;
    }
    if (replay && *path) {
        load_file(path);
        if (resume > 1.0)
            mp_cmd("seek %.1f 2", resume);
    }
}

static void raise_strip(void)
{
    Fl::flush();
    if (fl_xid(strip))
        XRaiseWindow(fl_display, fl_xid(strip));
}

static void hide_osc(void *)
{
    if (!fs_mode || !osc_shown)
        return;
    strip->hide();
    osc_shown = false;
}

static void poke_osc(void)
{
    if (!fs_mode)
        return;
    if (!osc_shown) {
        strip->show();
        osc_shown = true;
        raise_strip();
    }
    Fl::remove_timeout(hide_osc);
    Fl::add_timeout(OSC_HIDE_S, hide_osc);
}

static void set_fullscreen(bool on)
{
    if (on == fs_mode)
        return;

    if (on) {
        saved_x = win->x();
        saved_y = win->y();
        saved_w = win->w();
        saved_h = win->h();
        fs_mode = true;
        fs_btn->label("Exit");
        win->fullscreen();
        relayout();
        if (!osc_shown) {
            strip->show();
            osc_shown = true;
        }
        raise_strip();
        Fl::remove_timeout(hide_osc);
        Fl::add_timeout(OSC_HIDE_S, hide_osc);
    } else {
        fs_mode = false;
        fs_btn->label("Full");
        Fl::remove_timeout(hide_osc);
        if (!osc_shown) {
            strip->show();
            osc_shown = true;
        }
        if (saved_w > 0 && saved_h > 0)
            win->fullscreen_off(saved_x, saved_y, saved_w, saved_h);
        else
            win->fullscreen_off();
        relayout();
    }
    bind_video_wid();
    win->redraw();
}

static void apply_fullscreen(void *v)
{
    set_fullscreen(v != NULL);
}

static void request_fullscreen(bool on)
{
    Fl::remove_timeout(apply_fullscreen);
    Fl::add_timeout(0.0, apply_fullscreen, on ? (void *)1 : (void *)0);
}

struct VideoWin : Fl_Window {
    VideoWin(int X, int Y, int W, int H) : Fl_Window(X, Y, W, H)
    {
        end();
        color(FL_BLACK);
    }

    void draw()
    {
        int W = w(), H = h(), vw, vh, vx, vy;

        fl_color(FL_BLACK);

        if (!have_file || needs_clear) {
            fl_rectf(0, 0, W, H);
            needs_clear = false;
            return;
        }
        if (fit_mode || vid_w <= 0 || vid_h <= 0)
            return;

        vw = vid_w > W ? W : vid_w;
        vh = vid_h > H ? H : vid_h;
        vx = (W - vw) / 2;
        vy = (H - vh) / 2;

        if (vy > 0)
            fl_rectf(0, 0, W, vy);
        if (vy + vh < H)
            fl_rectf(0, vy + vh, W, H - vy - vh);
        if (vx > 0)
            fl_rectf(0, vy, vx, vh);
        if (vx + vw < W)
            fl_rectf(vx + vw, vy, W - vx - vw, vh);
    }
    int handle(int e)
    {
        switch (e) {
        case FL_PUSH:
            poke_osc();
            if (Fl::event_clicks()) {
                Fl::event_clicks(0);
                request_fullscreen(!fs_mode);
            }
            return 1;
        case FL_DRAG:
        case FL_RELEASE:
        case FL_MOVE:
            poke_osc();
            return 1;
        case FL_ENTER:
            return 1;
        }
        return Fl_Window::handle(e);
    }
};

struct PlayerWin : Fl_Double_Window {
    PlayerWin(int W, int H, const char *L) : Fl_Double_Window(W, H, L) {}
    void resize(int X, int Y, int W, int H)
    {
        Fl_Double_Window::resize(X, Y, W, H);
        relayout();
    }
};

static void shut_down_mplayer(void)
{
    if (mplayer_pid <= 0)
        return;
    mp_cmd("quit");
    if (to_mplayer >= 0)   { close(to_mplayer);   to_mplayer   = -1; }
    if (from_mplayer >= 0) { Fl::remove_fd(from_mplayer); close(from_mplayer); from_mplayer = -1; }
    for (int i = 0; i < 20; i++) {
        if (waitpid(mplayer_pid, NULL, WNOHANG) == mplayer_pid) {
            mplayer_pid = -1;
            return;
        }
        usleep(50 * 1000);
    }
    kill(mplayer_pid, SIGTERM);
    waitpid(mplayer_pid, NULL, 0);
    mplayer_pid = -1;
}

static void window_close_cb(Fl_Widget *w, void *)
{
    if (fs_mode && Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) {
        request_fullscreen(false);
        return;
    }
    shut_down_mplayer();
    ((Fl_Window *)w)->hide();
}

static int app_key_handler(int e)
{
    if (e != FL_SHORTCUT && e != FL_KEYDOWN)
        return 0;
    if (Fl::modal())
        return 0;

    switch (Fl::event_key()) {
    case FL_Escape:
        if (!fs_mode)
            return 0;
        request_fullscreen(false);
        return 1;
    case FL_Enter:
    case FL_KP_Enter:
    case 'f':
        request_fullscreen(!fs_mode);
        return 1;
    case ' ':
        play_pause_cb(NULL, NULL);
        return 1;
    case FL_Left:
        nudge_seek(-10);
        return 1;
    case FL_Right:
        nudge_seek(10);
        return 1;
    case FL_Up:
        bump_volume(5);
        return 1;
    case FL_Down:
        bump_volume(-5);
        return 1;
    case 'o':
        open_cb(NULL, NULL);
        return 1;
    }
    return 0;
}

static const char *resolve_mplayer(void)
{
    static char buf[512];

    const char *env = getenv("PIKO_PLAYER_MPLAYER");
    if (env && *env) { snprintf(buf, sizeof(buf), "%s", env); return buf; }

    static const char *cands[] = {
        "/mnt/card/.zaurus/usr/bin/mplayer",
        "/usr/bin/mplayer",
        "/usr/local/bin/mplayer",
    };
    for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++)
        if (access(cands[i], X_OK) == 0) {
            snprintf(buf, sizeof(buf), "%s", cands[i]);
            return buf;
        }

    const char *path = getenv("PATH");
    for (const char *p = path; p && *p; ) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        if (len > 0 && len < sizeof(buf) - sizeof("/mplayer")) {
            snprintf(buf, sizeof(buf), "%.*s/mplayer", (int)len, p);
            if (access(buf, X_OK) == 0)
                return buf;
        }
        if (!colon) break;
        p = colon + 1;
    }
    return NULL;
}

static int start_mplayer(Window wid, const char *mp)
{
    int in_pipe[2], out_pipe[2];

    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        perror("piko-player: pipe");
        return -1;
    }

    mplayer_pid = fork();
    if (mplayer_pid < 0) {
        perror("piko-player: fork");
        return -1;
    }

    if (mplayer_pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);

        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        char wid_arg[32];
        const char *args[28];
        int n = 0;

        snprintf(wid_arg, sizeof(wid_arg), "%lu", (unsigned long)wid);

        args[n++] = mp;
        args[n++] = "-slave";
        args[n++] = "-idle";
        args[n++] = "-quiet";
        args[n++] = "-vo";
        args[n++] = "x11";
        args[n++] = "-fixed-vo";
        args[n++] = "-framedrop";
        if (fit_mode)
            args[n++] = "-zoom";
        args[n++] = "-ao";
        args[n++] = "alsa";
        args[n++] = "-wid";
        args[n++] = wid_arg;
        args[n++] = "-nomouseinput";
        args[n++] = "-input";
        args[n++] = "nodefault-bindings";
        args[n++] = "-noconfig";
        args[n++] = "all";
        args[n] = NULL;

        execvp(mp, (char *const *)args);
        fprintf(stderr, "piko-player: cannot exec %s: %s\n", mp, strerror(errno));
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    to_mplayer   = in_pipe[1];
    from_mplayer = out_pipe[0];
    fcntl(from_mplayer, F_SETFL, O_NONBLOCK);
    Fl::add_fd(from_mplayer, on_mplayer_output);

    mp_cmd("volume %.0f 1", vol_slider->value());
    return 0;
}

int main(int argc, char **argv)
{
    const char *start_file = NULL;
    const char *logname;
    int sx, sy, sw, sh;

    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; i++)
        if (argv[i][0] != '-') { start_file = argv[i]; break; }

    logname = getenv("PIKO_PLAYER_LOG");
    if (logname && *logname)
        log_fp = fopen(logname, "w");

    Fl::screen_work_area(sx, sy, sw, sh);
    if (sw < 200 || sh < 160) { sw = 640; sh = 480; }
    sh -= 30;
    if (sh < MIN_VIDEO_H + STRIP_H)
        sh = MIN_VIDEO_H + STRIP_H;

    win = new PlayerWin(sw, sh, "piko-player");

    video = new VideoWin(0, 0, sw, sh - STRIP_H);

    strip = new Fl_Double_Window(0, sh - STRIP_H, sw, STRIP_H);
    strip->begin();
    strip->box(FL_FLAT_BOX);

    play_btn = new Fl_Button(0, 0, 10, 10, "Play");
    play_btn->callback(play_pause_cb);

    stop_btn = new Fl_Button(0, 0, 10, 10, "Stop");
    stop_btn->callback(stop_cb);

    open_btn = new Fl_Button(0, 0, 10, 10, "Open");
    open_btn->callback(open_cb);

    fs_btn = new Fl_Button(0, 0, 10, 10, "Full");
    fs_btn->callback(fs_cb);

    fit_btn = new Fl_Button(0, 0, 10, 10, "Fit");
    fit_btn->callback(fit_cb);

    time_box = new Fl_Box(0, 0, 10, 10, "0:00");
    time_box->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    seek_slider = new Fl_Hor_Slider(0, 0, 10, 10);
    seek_slider->type(FL_HOR_NICE_SLIDER);
    seek_slider->bounds(0.0, 0.0);
    seek_slider->value(0.0);
    seek_slider->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    seek_slider->callback(seek_cb);
    seek_slider->deactivate();

    vol_lbl = new Fl_Box(0, 0, 10, 10, "Vol");
    vol_lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    vol_slider = new Fl_Hor_Slider(0, 0, 10, 10);
    vol_slider->type(FL_HOR_NICE_SLIDER);
    vol_slider->bounds(0.0, 100.0);
    vol_slider->value(80.0);
    vol_slider->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    vol_slider->callback(volume_cb);

    status_box = new Fl_Box(0, 0, 10, 10, "");
    status_box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    status_box->labelsize(11);

    strip->end();

    for (int i = 0; i < strip->children(); i++)
        strip->child(i)->visible_focus(0);

    win->end();
    win->resizable(video);
    win->callback(window_close_cb);
    win->show();

    relayout();

    bind_video_wid();
    if (!video_wid) {
        fprintf(stderr, "piko-player: video window has no X id -- is DISPLAY set?\n");
        return 1;
    }

    Fl::add_handler(app_key_handler);

    const char *mp = resolve_mplayer();
    if (!mp) {
        fl_alert("Could not find the MPlayer program.\n\n"
                 "MPlayer lives on the SD card on this device. Insert the "
                 "card that carries it and try again, or set "
                 "PIKO_PLAYER_MPLAYER to its path.");
        return 1;
    }

    mplayer_bin = mp;
    if (start_mplayer(video_wid, mp) < 0) {
        fprintf(stderr, "piko-player: could not start MPlayer\n");
        return 1;
    }

    set_play_label();
    refresh_time_box();
    Fl::add_timeout(0.5, poll_tick);

    if (start_file)
        load_file(start_file);

    int rc = Fl::run();
    shut_down_mplayer();
    if (log_fp)
        fclose(log_fp);
    return rc;
}
