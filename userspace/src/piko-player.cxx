
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Hor_Slider.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_ask.H>
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

#define WIN_W       620
#define WIN_H       456
#define STRIP_H     78
#define VIDEO_H     (WIN_H - STRIP_H)

static pid_t mplayer_pid   = -1;
static int   to_mplayer    = -1;
static int   from_mplayer  = -1;

static double cur_pos      = 0.0;
static double cur_len      = 0.0;
static bool   have_file    = false;
static bool   is_paused    = false;
static bool   user_seeking = false;

static Fl_Button    *play_btn;
static Fl_Hor_Slider *seek_slider;
static Fl_Hor_Slider *vol_slider;
static Fl_Box       *time_box;

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

static void on_mplayer_output(int fd, void *)
{
    static char buf[4096];
    static size_t used = 0;
    ssize_t r;

    r = read(fd, buf + used, sizeof(buf) - 1 - used);
    if (r <= 0) {
        if (r < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        Fl::remove_fd(fd);
        close(from_mplayer);
        from_mplayer = -1;
        if (to_mplayer >= 0) { close(to_mplayer); to_mplayer = -1; }
        if (mplayer_pid > 0) { waitpid(mplayer_pid, NULL, 0); mplayer_pid = -1; }
        have_file = false;
        return;
    }
    used += (size_t)r;
    buf[used] = '\0';

    char *line = buf, *nl;
    while ((nl = strchr(line, '\n')) != NULL) {
        *nl = '\0';
        double v;
        if (sscanf(line, "ANS_TIME_POSITION=%lf", &v) == 1) {
            cur_pos = v;
            if (!user_seeking) {
                if (cur_len > 0.0) seek_slider->value(cur_pos);
                refresh_time_box();
            }
        } else if (sscanf(line, "ANS_LENGTH=%lf", &v) == 1) {
            cur_len = v;
            if (cur_len > 0.0) {
                seek_slider->bounds(0.0, cur_len);
                seek_slider->activate();
            }
            refresh_time_box();
        }
        line = nl + 1;
    }

    used = strlen(line);
    memmove(buf, line, used + 1);

    if (used >= sizeof(buf) - 1)
        used = 0;
}

static void poll_tick(void *)
{
    if (have_file && !is_paused) {
        mp_cmd("get_time_pos");
        if (cur_len <= 0.0)
            mp_cmd("get_time_length");
    }
    Fl::repeat_timeout(0.5, poll_tick);
}

static void set_play_label(void)
{
    play_btn->label(is_paused || !have_file ? "Play" : "Pause");
    play_btn->redraw();
}

static void open_cb(Fl_Widget *, void *)
{
    Fl_Native_File_Chooser fc;
    fc.title("Open video");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.directory("/mnt/card");

    if (fc.show() != 0)
        return;
    const char *path = fc.filename();
    if (!path || !*path)
        return;

    mp_cmd("loadfile \"%s\" 0", path);

    have_file    = true;
    is_paused    = false;
    cur_pos      = 0.0;
    cur_len      = 0.0;
    seek_slider->value(0.0);
    seek_slider->deactivate();
    refresh_time_box();
    set_play_label();
}

static void play_pause_cb(Fl_Widget *, void *)
{
    if (!have_file)
        return;
    mp_cmd("pause");
    is_paused = !is_paused;
    set_play_label();
}

static void stop_cb(Fl_Widget *, void *)
{
    if (!have_file)
        return;
    mp_cmd("stop");
    have_file = false;
    is_paused = false;
    cur_pos   = 0.0;
    cur_len   = 0.0;
    seek_slider->value(0.0);
    seek_slider->deactivate();
    refresh_time_box();
    set_play_label();
}

static void seek_cb(Fl_Widget *w, void *)
{
    Fl_Hor_Slider *s = (Fl_Hor_Slider *)w;
    int ev = Fl::event();

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
}

static const char *resolve_mplayer(void)
{
    static char buf[512];

    const char *env = getenv("PIKO_PLAYER_MPLAYER");
    if (env && *env) { snprintf(buf, sizeof(buf), "%s", env); return buf; }

    static const char *cands[] = {
        "/usr/bin/mplayer",
        "/usr/local/bin/mplayer",
        "/mnt/card/.zaurus/usr/bin/mplayer",
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
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        char wid_arg[32];
        snprintf(wid_arg, sizeof(wid_arg), "%lu", (unsigned long)wid);

        char *const args[] = {
            (char *)mp,
            (char *)"-slave", (char *)"-idle", (char *)"-quiet",
            (char *)"-vo", (char *)"x11", (char *)"-zoom",
            (char *)"-ao", (char *)"alsa",
            (char *)"-wid", wid_arg,
            (char *)"-nomouseinput",
            (char *)"-input", (char *)"nodefault-bindings",
            (char *)"-noconfig", (char *)"all",
            (char *)NULL,
        };
        execvp(mp, args);
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

static void shut_down_mplayer(void)
{
    if (mplayer_pid <= 0)
        return;
    mp_cmd("quit");
    if (to_mplayer >= 0)   { close(to_mplayer);   to_mplayer   = -1; }
    if (from_mplayer >= 0) { close(from_mplayer); from_mplayer = -1; }
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
    shut_down_mplayer();
    ((Fl_Window *)w)->hide();
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    Fl_Double_Window win(WIN_W, WIN_H, "piko-player");

    struct VideoBox : Fl_Window {
        VideoBox(int X, int Y, int W, int H) : Fl_Window(X, Y, W, H) {
            end();
            color(FL_BLACK);
        }
        void draw() {}
    };
    VideoBox video(0, 0, WIN_W, VIDEO_H);

    int y1 = VIDEO_H + 8;
    int y2 = VIDEO_H + 44;

    play_btn = new Fl_Button(8, y1, 60, 28, "Play");
    play_btn->callback(play_pause_cb);

    Fl_Button *stop_btn = new Fl_Button(72, y1, 56, 28, "Stop");
    stop_btn->callback(stop_cb);

    Fl_Button *open_btn = new Fl_Button(132, y1, 60, 28, "Open");
    open_btn->callback(open_cb);

    time_box = new Fl_Box(WIN_W - 116, y1, 108, 28, "0:00");
    time_box->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);

    seek_slider = new Fl_Hor_Slider(200, y1 + 4, WIN_W - 200 - 124, 20);
    seek_slider->type(FL_HOR_NICE_SLIDER);
    seek_slider->bounds(0.0, 0.0);
    seek_slider->value(0.0);
    seek_slider->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    seek_slider->callback(seek_cb);
    seek_slider->deactivate();

    Fl_Box *vol_lbl = new Fl_Box(8, y2, 60, 24, "Volume");
    vol_lbl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    vol_slider = new Fl_Hor_Slider(72, y2 + 3, 220, 18);
    vol_slider->type(FL_HOR_NICE_SLIDER);
    vol_slider->bounds(0.0, 100.0);
    vol_slider->value(80.0);
    vol_slider->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    vol_slider->callback(volume_cb);

    win.end();
    win.callback(window_close_cb);
    win.show(argc, argv);

    video.show();
    Fl::flush();
    Window wid = fl_xid(&video);
    if (!wid) {
        fprintf(stderr, "piko-player: video window has no X id -- is DISPLAY set?\n");
        return 1;
    }

    const char *mp = resolve_mplayer();
    if (!mp) {
        fl_alert("Could not find the MPlayer program.\n\n"
                 "MPlayer lives on the SD card on this device. Insert the "
                 "card that carries it and try again, or set "
                 "PIKO_PLAYER_MPLAYER to its path.");
        return 1;
    }

    if (start_mplayer(wid, mp) < 0) {
        fprintf(stderr, "piko-player: could not start MPlayer\n");
        return 1;
    }

    set_play_label();
    refresh_time_box();
    Fl::add_timeout(0.5, poll_tick);

    int rc = Fl::run();
    shut_down_mplayer();
    return rc;
}
