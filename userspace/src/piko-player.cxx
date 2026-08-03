/*
 * piko-player -- a small FLTK front-end for MPlayer.
 *
 * WHY THIS EXISTS: MPlayer's own GUI (gmplayer, --enable-gui) is a GTK+
 * program, and this ROM has no GTK and no intention of growing one -- the
 * whole point of putting FLTK in the image (see docs/HOWTO-FLTK.md) was to
 * "write our own GUI apps instead of only running other people's". So the
 * video engine stays MPlayer, but the window, the buttons and the seek bar
 * are ours, in FLTK, on the same Xfbdev every other client here talks to.
 *
 * HOW IT WORKS: this program owns an ordinary FLTK window. Inside it is a
 * bare child X window (VideoBox, a nested Fl_Window) whose only job is to
 * exist and have an X id. We hand that id to MPlayer with -wid, and MPlayer
 * draws its video straight into it with -vo x11 -- exactly the embedding
 * trick smplayer/gnome-mplayer/the old browser plugin all used. MPlayer is
 * launched once, in slave mode (-slave -idle), and stays alive for the whole
 * session: we drive it by writing one-line commands to its stdin
 * (loadfile/pause/stop/seek/volume) and learn where playback is by reading
 * its ANS_* answer lines back off its stdout. FLTK never touches a pixel of
 * video; MPlayer never draws a control.
 *
 * WHAT MPLAYER THIS NEEDS: an MPlayer built with -vo x11 (tools/build-
 * mplayer.sh, X11-enabled variant). The old framebuffer-only, fully static
 * MPlayer cannot render into an X window and -wid is a no-op there. The X11
 * MPlayer ships in the ROM next to us (payload puts it at /usr/bin/mplayer),
 * so a bare execvp("mplayer", ...) finds it on PATH; PIKO_PLAYER_MPLAYER
 * overrides that for testing an alternate binary.
 *
 * DELIBERATELY SMALL: open a file, play/pause, stop, seek, volume. No
 * playlist, no fullscreen, no equalizer -- this is a 400MHz PXA255 with a
 * 640x480 panel, and a core player that is solid beats a feature list that
 * stutters. Everything here is one process driving one child over two pipes.
 */

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

/* ── layout ─────────────────────────────────────────────────────────────── */

/*
 * The panel is 640x480 landscape (see build-matchbox-payload.sh's long note
 * on that being genuinely ambiguous from the outside -- trust fb0/modes).
 * matchbox draws a title bar on top, so we leave a little headroom rather
 * than claim all 480 rows and get clipped. The control strip is a fixed
 * height at the bottom; the video box takes whatever is left.
 */
#define WIN_W       620
#define WIN_H       456
#define STRIP_H     78                       /* two rows of controls        */
#define VIDEO_H     (WIN_H - STRIP_H)

/* ── child MPlayer, driven over two pipes ───────────────────────────────── */

static pid_t mplayer_pid   = -1;
static int   to_mplayer    = -1;             /* our write end of its stdin  */
static int   from_mplayer  = -1;             /* our read end of its stdout  */

/* Playback state, kept in sync from MPlayer's ANS_* replies. */
static double cur_pos      = 0.0;            /* seconds into the file       */
static double cur_len      = 0.0;            /* file length, 0 if unknown   */
static bool   have_file    = false;          /* a file is loaded            */
static bool   is_paused    = false;
static bool   user_seeking = false;          /* thumb is under a finger     */

/* Widgets the callbacks and the poll timer need to reach. */
static Fl_Button    *play_btn;
static Fl_Hor_Slider *seek_slider;
static Fl_Hor_Slider *vol_slider;
static Fl_Box       *time_box;

/*
 * Write one slave-mode command line to MPlayer. Best-effort: if the pipe is
 * gone (MPlayer died) the write fails and we simply drop the command --
 * SIGPIPE is ignored in main() so a dead child cannot take us down here.
 */
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
        { /* child gone; the fd handler will notice the EOF and clean up */ }
}

/* mm:ss, clamped at zero. Buffer must hold at least 8 bytes. */
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

/*
 * MPlayer answers get_time_pos / get_time_length with plain lines on its
 * stdout, e.g. "ANS_TIME_POSITION=12.3". Parse the two we asked for and let
 * the slider and clock follow. Called by Fl::add_fd whenever the child has
 * written something; we buffer across calls so a reply split over two reads
 * is still parsed as one line.
 */
static void on_mplayer_output(int fd, void *)
{
    static char buf[4096];
    static size_t used = 0;
    ssize_t r;

    r = read(fd, buf + used, sizeof(buf) - 1 - used);
    if (r <= 0) {
        /* 0 == EOF (MPlayer exited), <0 with a real error == same story.
         * EAGAIN just means "nothing right now" on this non-blocking fd. */
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

    /* Keep the unterminated tail for the next read. */
    used = strlen(line);
    memmove(buf, line, used + 1);

    /* A single line longer than the whole buffer (never a short ANS_* reply,
     * but be safe) would otherwise leave no room for the next read and make
     * read() return 0 -- which we'd misread as EOF and kill MPlayer. Drop it
     * instead: we only care about the ANS_* lines, and those are tiny. */
    if (used >= sizeof(buf) - 1)
        used = 0;
}

/*
 * Ask MPlayer where it is, twice a second, while something is playing. The
 * answers arrive asynchronously on stdout and are handled above. We don't
 * poll while paused or idle -- nothing is moving, so there is nothing to
 * learn, and a slave query per tick would only spin the CPU.
 */
static void poll_tick(void *)
{
    if (have_file && !is_paused) {
        mp_cmd("get_time_pos");
        if (cur_len <= 0.0)
            mp_cmd("get_time_length");
    }
    Fl::repeat_timeout(0.5, poll_tick);
}

/* ── controls ───────────────────────────────────────────────────────────── */

static void set_play_label(void)
{
    /* Plain words, not FLTK's @-symbols: clearer on a small touch panel, and
     * not every transport glyph (@|| in particular) is a symbol FLTK ships. */
    play_btn->label(is_paused || !have_file ? "Play" : "Pause");
    play_btn->redraw();
}

static void open_cb(Fl_Widget *, void *)
{
    Fl_Native_File_Chooser fc;
    fc.title("Open video");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    /* Start on the card: that is where media actually lives on this device
     * (the NAND root is ~68 MiB and chronically full). Harmless if absent --
     * the chooser just opens on its default directory. */
    fc.directory("/mnt/card");

    if (fc.show() != 0)                       /* cancelled or error */
        return;
    const char *path = fc.filename();
    if (!path || !*path)
        return;

    /* loadfile takes a quoted path; a plain double-quote inside a filename
     * would break the quoting, but that is vanishingly rare and MPlayer's
     * slave parser has no escape for it either. */
    mp_cmd("loadfile \"%s\" 0", path);

    have_file    = true;
    is_paused    = false;
    cur_pos      = 0.0;
    cur_len      = 0.0;
    seek_slider->value(0.0);
    seek_slider->deactivate();                /* re-activated once length is known */
    refresh_time_box();
    set_play_label();
}

static void play_pause_cb(Fl_Widget *, void *)
{
    if (!have_file)
        return;
    mp_cmd("pause");                          /* MPlayer's pause is a toggle */
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

/*
 * The seek slider does double duty: playback pushes the thumb along (in
 * on_mplayer_output), and the user drags it to seek. We tell the two apart
 * by the FLTK event -- a drag/push sets user_seeking so the poll tick stops
 * fighting the finger, and the release is what actually issues the seek.
 */
static void seek_cb(Fl_Widget *w, void *)
{
    Fl_Hor_Slider *s = (Fl_Hor_Slider *)w;
    int ev = Fl::event();

    if (ev == FL_PUSH || ev == FL_DRAG) {
        user_seeking = true;
        cur_pos = s->value();                      /* live readout while dragging */
        refresh_time_box();
    } else {                                       /* FL_RELEASE (or other) */
        mp_cmd("seek %.1f 2", s->value());         /* type 2 == absolute seconds */
        user_seeking = false;
    }
}

static void volume_cb(Fl_Widget *w, void *)
{
    Fl_Hor_Slider *s = (Fl_Hor_Slider *)w;
    mp_cmd("volume %.0f 1", s->value());           /* 1 == absolute 0..100 */
}

/* ── launching MPlayer ──────────────────────────────────────────────────── */

/*
 * Find the MPlayer binary. On this device it lives on the SD card by
 * deliberate design -- MPlayer with its bundled ffmpeg is far too big for
 * the ~68 MiB NAND root, so the ROM ships the player GUI but not the engine
 * (see tools/build-mplayer.sh and the manifest's "card-only destination"
 * note). We therefore look where it actually is, rather than trusting the
 * matchbox session to have the card's bindir on PATH:
 *
 *   PIKO_PLAYER_MPLAYER   explicit override, for testing an alternate build
 *   /usr/bin, /usr/local/bin   in case a future ROM does carry it
 *   /mnt/card/.zaurus/usr/bin  its real home when a card is in
 *   $PATH                 last resort
 *
 * Returns a pointer to a static buffer, or NULL if it is nowhere -- which is
 * the "no card inserted" case, worth telling the user about plainly.
 */
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

/*
 * Start the one long-lived MPlayer slave, drawing into the X window `wid`.
 * Two pipes: ours-to-write -> its stdin, its stdout -> ours-to-read. The read
 * end is non-blocking and handed to FLTK so its answers pump through the
 * normal event loop instead of a blocking read.
 */
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
        /* child: wire the pipe ends onto stdin/stdout, then become MPlayer */
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]);  close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        char wid_arg[32];
        snprintf(wid_arg, sizeof(wid_arg), "%lu", (unsigned long)wid);

        /*
         * -wid embeds video into our window; -vo x11 is the only VO that can
         *  (this device has no Xv). -zoom lets vo_x11 scale to the widget
         *  rather than clip at native size. -slave -idle keep it alive and
         *  listening on stdin with no file loaded. -quiet keeps the ANS_*
         *  answer lines while dropping the per-frame status spam. The input
         *  flags stop MPlayer from grabbing the keyboard/mouse or reading a
         *  config -- every control comes from us over the pipe.
         */
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
        execvp(mp, args);            /* mp is an absolute path from resolve_mplayer() */
        fprintf(stderr, "piko-player: cannot exec %s: %s\n", mp, strerror(errno));
        _exit(127);
    }

    /* parent */
    close(in_pipe[0]);
    close(out_pipe[1]);
    to_mplayer   = in_pipe[1];
    from_mplayer = out_pipe[0];
    fcntl(from_mplayer, F_SETFL, O_NONBLOCK);
    Fl::add_fd(from_mplayer, on_mplayer_output);

    /* Match the on-screen volume slider's starting position so the first
     * frame's audio is at the level the thumb shows. */
    mp_cmd("volume %.0f 1", vol_slider->value());
    return 0;
}

/* ── shutdown ───────────────────────────────────────────────────────────── */

static void shut_down_mplayer(void)
{
    if (mplayer_pid <= 0)
        return;
    mp_cmd("quit");                            /* ask nicely first           */
    if (to_mplayer >= 0)   { close(to_mplayer);   to_mplayer   = -1; }
    if (from_mplayer >= 0) { close(from_mplayer); from_mplayer = -1; }
    /* Give it a moment, then make sure it is gone -- a wedged decoder must
     * not outlive the window that was its only reason to run. */
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
    ((Fl_Window *)w)->hide();                   /* drops out of Fl::run()     */
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Writing to a dead MPlayer's pipe must not kill us; mp_cmd handles the
     * failed write, and on_mplayer_output does the actual cleanup. */
    signal(SIGPIPE, SIG_IGN);

    Fl_Double_Window win(WIN_W, WIN_H, "piko-player");

    /*
     * The video surface is a real child X window (a nested Fl_Window), not a
     * plain box, because MPlayer needs an X id to draw into. draw() is left
     * empty: once MPlayer owns the pixels, having FLTK repaint the box would
     * only stutter black over the video on every expose.
     */
    struct VideoBox : Fl_Window {
        VideoBox(int X, int Y, int W, int H) : Fl_Window(X, Y, W, H) {
            end();
            color(FL_BLACK);
        }
        void draw() {}                          /* MPlayer paints this window */
    };
    VideoBox video(0, 0, WIN_W, VIDEO_H);

    /* Control strip. Row 1: transport + seek + clock. Row 2: volume. */
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
    seek_slider->deactivate();                  /* nothing to seek until a file loads */

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

    /*
     * The child X window has to be realized before MPlayer can be told its
     * id: show() maps the toplevel, and a flush pushes the subwindow's
     * XCreateWindow to the server so fl_xid() returns a live window rather
     * than 0.
     */
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
