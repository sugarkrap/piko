/*
 * matchbox-fbrun -- run a program that needs the whole machine.
 *
 * Some applications on this device (Quake, emulators, video players) want
 * every megabyte and every cycle, and want to own /dev/fb0 outright. That
 * cannot be shared with an X server: Xfbdev maps the framebuffer and holds
 * an EVIOCGRAB on the keyboard and touchscreen, so a framebuffer app started
 * under it renders but never receives a keystroke -- it looks alive and is
 * completely uncontrollable.
 *
 * Rather than a bespoke wrapper per application, this is the one place that
 * knows how to hand the machine over and take it back:
 *
 *   1. ask the user, on screen, while X is still up;
 *   2. close the other graphical clients politely;
 *   3. stop the graphical session;
 *   4. run the program with the console to itself;
 *   5. put everything back, whatever happened to it.
 *
 * Step 5 is unconditional. Restoring only on a clean exit is how you end up
 * with a device that boots to a black screen and looks bricked.
 *
 * WITH NO X RUNNING this degrades to "run the program, then restore the
 * console" -- which is exactly what the old shell fbrun did, so this
 * replaces it rather than sitting beside it.
 *
 * USAGE
 *   matchbox-fbrun [-n NAME] [-r REASON] [-y] [--qvga] [--fast-pll] [--] program [args...]
 *
 *   -n NAME      application name for the dialog (default: the binary's name)
 *   -r REASON    extra line of explanation in the dialog
 *   -y           skip the dialog and proceed (for scripts and for the
 *                console case, where there is nothing to ask about)
 *   --qvga       switch /dev/fb0 to half its native resolution before running
 *                the program, and back afterwards. Several of this ROM's
 *                devices pixel-double a QVGA framebuffer back up to their
 *                native VGA glass in the LCD controller itself, so a program
 *                that struggles at native resolution can ask for a quarter of
 *                the pixels, for the same physical screen size, without
 *                knowing anything about the panel underneath it.
 *   --fast-pll   raise the w100's PLL (see the "fastpllclk" sysfs attribute
 *                in modules/w100/w100fb_patched.c): 100->125MHz in QVGA,
 *                75->100MHz in VGA. See docs/DEADLETTER-W100-CLOCK-DOMAINS.md
 *                for which combinations are actually proven safe on hardware
 *                -- this flag does not itself validate anything, it only
 *                asks the kernel driver for the mode's own fast_pll_freq.
 *
 * When neither --qvga nor --fast-pll is given on the command line, the
 * effective video mode instead comes from whatever was last chosen in the
 * confirmation dialog's Advanced panel, persisted in
 * /etc/zaurus/matchbox-fbrun.cfg (see load_video_mode_config()). An explicit
 * flag always overrides that persisted default for the current run; picking
 * a different mode in the dialog updates it for the next one.
 *
 * EXIT STATUS
 *   the program's own exit status, or 0 if the user chose Abort, or
 *   126/127 in the manner of a shell if it could not be run at all.
 */

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Box.H>
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
#include <unistd.h>

/* ── configuration ──────────────────────────────────────────────────────── */

/*
 * How the graphical session is stopped and restarted is the only genuinely
 * piko-specific part of this program, so it is confined to these two
 * constants and the two functions that use them.
 *
 * /etc/init.d/xsession is a `respawn` entry on tty1 in inittab. When the X
 * server dies, that script's own fallback path execs a getty in the same
 * pid; killing THAT getty is what makes init re-run the script and bring
 * the desktop back. We therefore never start X ourselves -- init does, and
 * it is much better at it than we would be.
 */
#define XSERVER_NAME    "Xfbdev"
#define SESSION_TTY     "tty1"

#define STOP_TIMEOUT_S      15   /* wait for the X server to go away */
#define FALLBACK_TIMEOUT_S  20   /* wait for the getty to take its place */
#define CLOSE_TIMEOUT_S      5   /* wait for other clients to close */

#define LOCK_PATH   "/tmp/matchbox-fbrun.lock"

/*
 * Set for the child, so a nested invocation knows the machine has already
 * been handed over and simply runs the program.
 *
 * This composition is easy to reach by accident and fails badly without the
 * guard: a launcher script that calls us, invoked from a .desktop that is
 * marked X-Piko-Heavy, gets wrapped twice -- and the inner one would find
 * the outer one's lock held and exit without ever starting the application.
 */
#define REENTRY_ENV "MATCHBOX_FBRUN_ACTIVE"

/*
 * Persistent phase trace, independent of every fd this process might have
 * inherited. Found necessary live: the obvious place to look,
 * matchbox-session's own stderr, runs through /etc/init.d/xsession's
 * `> $SLOG 2>&1` -- which TRUNCATES on every session restart, so a crash
 * during the very session-restart this program triggers can erase the
 * output that would explain it. And an OOM kill (SIGKILL, which nothing
 * -- not atexit(), not a handler -- can intercept) can take this process
 * out mid-handover with no chance to report anything through a normal
 * exit path at all.
 *
 * The fix for both is the same: log to a fixed file on the card, opened
 * fresh and fsync()'d after every line, so whatever was written before
 * the kill is what's on disk after it -- the trace ends exactly where
 * execution did, which is the diagnostic value of the whole thing. Card
 * rather than NAND on purpose: NAND is the ~68 MiB root this device is
 * chronically near-full on, and a jffs2 GC stall is a real way for a
 * write here to itself become the failure being diagnosed.
 *
 * Best-effort throughout: tracing must never be why the actual handover
 * fails, so every error here is silently swallowed.
 */
#define TRACE_LOG "/mnt/card/.zaurus/var/log/fbrun-trace.log"

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
        len = (int)sizeof(msg) - 2;   /* leave room for the newline below */
    msg[len++] = '\n';

    /* Cheap and idempotent (EEXIST on every call after the first) rather
     * than tracked with a static flag: a stat() to check first would cost
     * the same syscall this saves, and the target directory not existing
     * yet (no card, or nothing has made it before) must not be fatal. */
    mkdir("/mnt/card/.zaurus/var", 0755);
    mkdir("/mnt/card/.zaurus/var/log", 0755);

    fd = open(TRACE_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    if (write(fd, msg, (size_t)len) > 0)
        fsync(fd);
    close(fd);
}

/* ── state the cleanup path needs ───────────────────────────────────────── */

static pid_t child_pid;
static int   session_was_stopped;
static int   lock_held;
static int   cleaned_up;
static int   qvga_requested;
static int   qvga_applied;
static int   fast_pll_requested;
static int   fast_pll_applied;
static struct fb_var_screeninfo saved_var;

/* ── /proc scanning ─────────────────────────────────────────────────────── */

/*
 * There is no pidof or pgrep in this rootfs's busybox, and its ash has no
 * kill builtin either -- which is precisely the sort of thing that makes the
 * shell version of this awkward and the C version straightforward.
 *
 * Matches a process whose argv[0] basename equals `name`, or whose full
 * command line contains `needle` when one is given. Returns the number of
 * pids found, filling `out` up to `max`.
 */
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

        /* argv[0] is the first NUL-terminated string. */
        base = strrchr(buf, '/');
        base = base ? base + 1 : buf;

        if (name && strcmp(base, name) == 0) {
            out[n++] = pid;
            continue;
        }

        if (needle) {
            ssize_t i;
            /* Flatten the NULs so a plain substring search works. */
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

/* ── console ────────────────────────────────────────────────────────────── */

/*
 * Put the console back into text mode. A framebuffer app sets KD_GRAPHICS so
 * fbcon stops drawing over it, and can revert that on a clean exit -- but
 * SIGKILL, a crash or an OOM kill cannot be caught by anything, and leave the
 * console stuck with no visible shell. Doing it here is the safety net the
 * standalone fbtext used to provide.
 *
 * Also restores the text cursor (DECTCEM show, \033[?25h) hidden by
 * show_splash() below. KD_GRAPHICS on this fbcon does not reliably blank
 * the cursor by itself -- found live as a cursor-shaped artifact bleeding
 * into the framebuffer during the splash text phase, while still in
 * KD_TEXT -- so it is hidden and shown explicitly rather than assumed to
 * follow the text/graphics mode switch.
 *
 * Only called from cleanup() when session_was_stopped -- see that call
 * site's comment. Calling this unconditionally (found live, 2026-08-06)
 * forced KD_TEXT on the ACTIVE console even when the graphical session was
 * never touched, e.g. on Abort: main() returns straight after the dialog,
 * before session_stop() ever runs, but atexit(cleanup) still fired this.
 * The result was a visible flash of the text console (a stray cursor,
 * looking like a quick VT switch) over the still-running X session, which
 * then had to reassert its own KD_GRAPHICS to repaint over it.
 */
static void console_text_mode(void)
{
    int fd = open("/dev/tty0", O_RDWR);

    if (fd < 0)
        fd = open("/dev/console", O_RDWR);
    if (fd < 0)
        return;
    ioctl(fd, KDSETMODE, KD_TEXT);
    if (write(fd, "\033[?25h", 6) < 0)
        { /* best-effort: a shell prompt with no cursor is a cosmetic issue, not this function's to fail over */ }
    close(fd);
}

/*
 * The console is still in KD_TEXT mode at this point -- whatever the
 * launched program sets KD_GRAPHICS itself, later, is what stops fbcon
 * drawing over it, and that switch (found live, in vid_fbdev.c for one
 * fbdev-native engine) can be many seconds into the program's own init.
 * Until then the screen is just whatever fbcon last had on it: usually
 * stale boot dmesg, since nothing else writes to the console in normal
 * operation. That reads as "hung" from the chair, not "starting" --
 * there is no other signal a slow-starting program has that this device
 * can show. Fixing that does not need a font renderer or pixel access:
 * fbcon already renders text, so writing to it plainly (the ANSI clear
 * first, so this replaces the stale text rather than sitting after it)
 * is the whole fix.
 */
static void show_splash(const char *name)
{
    /* O_RDWR, not O_WRONLY -- matching console_text_mode() above, the
     * proven-working open on this same device, rather than assuming a
     * write-only open behaves identically. */
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

    /* \033[?25l (DECTCEM hide) first: KD_GRAPHICS later does not reliably
     * blank this fbcon's cursor by itself -- see console_text_mode()'s
     * comment, which shows it again on the way back. */
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

/* ── video mode ─────────────────────────────────────────────────────────── */

#define FB_DEV "/dev/fb0"

/*
 * Halves xres/yres from whatever mode the console is currently in, rather
 * than hard-coding 240x320: that tracks the device's native orientation
 * (portrait vs. landscape framebuffer) without this file needing to know
 * it, and matches how these panels actually double QVGA pixels back up to
 * their native glass -- see corgi_lcd_set_mode() in the LCD driver. Saves
 * the pre-switch mode unconditionally so restore_video_mode() can put it
 * back exactly, the same "step 5 is unconditional" guarantee the rest of
 * this file gives the console and the session.
 */
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

/* No-op unless set_qvga_mode() actually switched something -- self-guarded
 * via qvga_applied, so it is safe to call unconditionally from cleanup()
 * regardless of how far main() got (e.g. Abort, where it was never set). */
static void restore_video_mode(void)
{
    int fd;

    if (!qvga_applied)
        return;
    qvga_applied = 0;

    fd = open(FB_DEV, O_RDWR);
    if (fd < 0) {
        trace("restore_video_mode: open %s failed: %s", FB_DEV, strerror(errno));
        return;
    }
    saved_var.activate = FB_ACTIVATE_NOW;
    if (ioctl(fd, FBIOPUT_VSCREENINFO, &saved_var) < 0)
        trace("restore_video_mode: FBIOPUT_VSCREENINFO %ux%u failed: %s",
              saved_var.xres, saved_var.yres, strerror(errno));
    else
        trace("restore_video_mode: restored %ux%u", saved_var.xres, saved_var.yres);
    close(fd);
}

#define FASTPLL_SYSFS "/sys/devices/platform/w100fb/fastpllclk"

/*
 * Write 1/0 to the w100fb driver's "fastpllclk" sysfs attribute. Does not
 * decide which frequency that resolves to -- that is entirely the current
 * w100_mode's own fast_pll_freq (100->125MHz in QVGA, 75->100MHz in VGA;
 * see modules/mach-pxa/corgi_patched.c), fixed by the kernel's mode table.
 * See docs/DEADLETTER-W100-CLOCK-DOMAINS.md for which combinations of this
 * and --qvga are actually proven safe on hardware -- this function does not
 * itself validate anything, it only asks the driver for what it already
 * exposes.
 */
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

/* No-op unless set_fast_pll(1) actually succeeded -- same self-guarded,
 * unconditional-from-cleanup() shape as restore_video_mode() above. */
static void restore_fast_pll(void)
{
    if (!fast_pll_applied)
        return;
    fast_pll_applied = 0;
    set_fast_pll(0);
}

/* ── the graphical session ──────────────────────────────────────────────── */

static int x_is_running(void)
{
    pid_t pids[8];

    return find_pids(XSERVER_NAME, NULL, pids, 8) > 0;
}

/*
 * Ask every other toplevel to close itself before pulling the server out
 * from under it. This is what gives applications a chance to save; killing X
 * first would deny them that. Best-effort by design -- anything still up
 * afterwards simply loses its connection, which it must survive anyway.
 */
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
        return 0;                 /* nothing to stop; console-only case */
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

    /* Refuses to go quietly; it is holding the framebuffer we need. */
    n = find_pids(XSERVER_NAME, NULL, pids, 8);
    if (n) {
        trace("session_stop: %d still alive after %ds, SIGKILL", n, waited);
        for (i = 0; i < n; i++)
            kill(pids[i], SIGKILL);
        sleep(1);
    }

    /*
     * Wait for xsession's fallback getty to appear before handing the
     * console over, so the VT has settled rather than being reconfigured
     * underneath the application a moment after it starts.
     */
    for (waited = 0; waited < FALLBACK_TIMEOUT_S; waited++) {
        if (find_pids(NULL, SESSION_TTY, pids, 8) > 0)
            break;
        sleep(1);
    }
    trace("session_stop: fallback getty %s after %ds -- returning",
          waited < FALLBACK_TIMEOUT_S ? "appeared" : "NEVER APPEARED", waited);

    return 1;
}

/*
 * Killing the fallback getty is what makes init respawn the tty1 entry,
 * which is /etc/init.d/xsession -- so the desktop comes back on its own. If
 * there is no getty, the slot has already exited and init is restarting it
 * for us; either way we do not start X by hand.
 */
static void session_restore(void)
{
    pid_t pids[8];
    int n, i;

    n = find_pids(NULL, SESSION_TTY, pids, 8);
    trace("session_restore: SIGTERM to %d getty pid(s) on %s", n, SESSION_TTY);
    for (i = 0; i < n; i++)
        kill(pids[i], SIGTERM);
}

/* ── cleanup ────────────────────────────────────────────────────────────── */

static void cleanup(void)
{
    if (cleaned_up)
        return;
    cleaned_up = 1;

    trace("cleanup: entered (session_was_stopped=%d)", session_was_stopped);

    restore_video_mode();
    restore_fast_pll();

    /* Both gated on session_was_stopped: nothing below this point was ever
     * touched if we never got past the confirmation dialog (e.g. Abort),
     * so there is nothing to put back -- see console_text_mode()'s comment
     * for what forcing it unconditionally broke. */
    if (session_was_stopped) {
        console_text_mode();
        session_restore();
    }

    if (lock_held)
        unlink(LOCK_PATH);

    trace("cleanup: done");
}

/*
 * Signals have to be handled, not ignored: without this a plain `kill` of
 * the wrapper would leave the console in graphics mode and the desktop
 * stopped -- indistinguishable, from the user's chair, from a dead device.
 */
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

/* ── single instance ────────────────────────────────────────────────────── */

/*
 * Two of these at once would each stop the session and each try to restore
 * it, and two framebuffer applications would fight over /dev/fb0 -- an easy
 * state to reach with a double tap on a desktop icon.
 */
static int take_lock(void)
{
    int fd = open(LOCK_PATH, O_CREAT | O_EXCL | O_WRONLY, 0644);
    char buf[32];

    if (fd >= 0) {
        int len = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
        ssize_t written = write(fd, buf, (size_t)len);

        (void)written;   /* the pid is advisory; the file's existence is the lock */
        close(fd);
        lock_held = 1;
        trace("take_lock: acquired");
        return 1;
    }

    /* Stale lock from something that died badly? Take it over. */
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

/* ── video mode preference (persisted) ──────────────────────────────────── */

/*
 * The four combinations --qvga/--fast-pll can actually reach, in the order
 * they appear in the dialog's Advanced list. Labelled with the PLL
 * frequency each one actually runs at (modules/mach-pxa/corgi_patched.c's
 * w100_mode table), not just "normal"/"fast", since that is the number
 * that matters for docs/DEADLETTER-W100-CLOCK-DOMAINS.md's validation
 * table.
 */
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

/* Stable on-disk names, independent of the enum's own ordering/values --
 * so reordering video_mode later can't silently reinterpret an old
 * /etc/zaurus/matchbox-fbrun.cfg written by a previous build. */
static const char *video_mode_keys[VIDEO_MODE_COUNT] = {
    "qvga-normal",
    "qvga-fast",
    "vga-normal",
    "vga-fast",
};

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

#define FBRUN_CONFIG_DIR  "/etc/zaurus"
#define FBRUN_CONFIG_PATH "/etc/zaurus/matchbox-fbrun.cfg"

/*
 * Same convention as mb-volume.c's CONFIG_PATH/brightd.c's CONFIG_PATH:
 * key=value, one per line, '#' comments and blank lines skipped, unknown
 * keys silently ignored (forward-compatible with a future key this build
 * doesn't know about). Falls back to VIDEO_MODE_VGA_NORMAL -- today's
 * default behaviour with no flags at all -- if the file is missing or has
 * no recognised video_mode line.
 */
static enum video_mode load_video_mode_config(void)
{
    FILE *f = fopen(FBRUN_CONFIG_PATH, "r");
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

/* Atomic tmp+rename write, same as mb-volume.c's save_config() -- this is
 * flash storage, and a config file half-written by a kill mid-write must
 * never be what the next launch reads back. */
static void save_video_mode_config(enum video_mode mode)
{
    char tmp_path[sizeof(FBRUN_CONFIG_PATH) + 4];
    FILE *f;

    mkdir(FBRUN_CONFIG_DIR, 0755);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", FBRUN_CONFIG_PATH);

    f = fopen(tmp_path, "w");
    if (!f) {
        trace("save_video_mode_config: fopen %s failed: %s", tmp_path, strerror(errno));
        return;
    }
    fprintf(f, "# matchbox-fbrun persisted video mode -- see the dialog's Advanced panel\n");
    fprintf(f, "video_mode=%s\n", video_mode_keys[mode]);
    fclose(f);

    if (rename(tmp_path, FBRUN_CONFIG_PATH) < 0)
        trace("save_video_mode_config: rename %s -> %s failed: %s",
              tmp_path, FBRUN_CONFIG_PATH, strerror(errno));
    else
        trace("save_video_mode_config: saved video_mode=%s", video_mode_keys[mode]);
}

/*
 * The Advanced panel's mode list. Same table-of-hand-drawn-cells technique
 * as piko-sync's TransferTable/pikostore's HistoryTable (see
 * userspace/src/piko-sync/transfer_table.h) -- this project's own
 * established list-widget idiom, rather than Fl_Browser (used nowhere in
 * this codebase outside the bundled FLTK library itself). One column, one
 * row per video_mode, click-to-select via Fl_Table's own callback/context
 * mechanism rather than a handle() override.
 */
class VideoModeList : public Fl_Table {
public:
    VideoModeList(int X, int Y, int W, int H)
        : Fl_Table(X, Y, W, H), selected_(VIDEO_MODE_VGA_NORMAL)
    {
        col_header(0);
        col_resize(0);
        row_header(0);
        row_resize(0);
        row_height_all(26);
        cols(1);
        col_width_all(W - 4);
        rows(VIDEO_MODE_COUNT);
        end();
        callback(table_cb, this);
        when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
    }

    void selected(enum video_mode m) { selected_ = m; redraw(); }
    enum video_mode selected(void) const { return selected_; }

protected:
    void draw_cell(TableContext context, int R = 0, int C = 0,
                   int X = 0, int Y = 0, int W = 0, int H = 0)
    {
        (void)C;
        switch (context) {
        case CONTEXT_CELL: {
            int is_selected = (R == (int)selected_);

            fl_push_clip(X, Y, W, H);
            fl_draw_box(FL_THIN_UP_BOX, X, Y, W, H,
                        is_selected ? FL_SELECTION_COLOR : FL_BACKGROUND2_COLOR);
            fl_color(is_selected ? FL_WHITE : FL_BLACK);
            fl_draw(video_mode_labels[R], X + 6, Y, W - 12, H, FL_ALIGN_LEFT);
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

            if (r >= 0 && r < VIDEO_MODE_COUNT) {
                self->selected_ = (enum video_mode)r;
                self->redraw();
            }
        }
    }

    enum video_mode selected_;
};

/* ── the dialog ─────────────────────────────────────────────────────────── */

/*
 * A hand-rolled dialog instead of fl_choice(): fl_choice()'s window carries
 * no _NET_WM_WINDOW_TYPE and no WM_TRANSIENT_FOR (FLTK only sets the latter
 * when this process already has another Fl_Window open, which it never
 * does -- this dialog is the only GUI object matchbox-fbrun ever creates).
 * With neither hint, matchbox-window-manager's classifier
 * (wm_make_new_client() in wm.c) falls through to treating it as a plain
 * MBCLIENT_TYPE_APP window -- the same category as the application it is
 * about to launch -- instead of MBCLIENT_TYPE_DIALOG. An app window mapped
 * while the desktop is showing can end up stacked *below* the desktop's
 * full-screen window; a dialog never can (dialog_client_show() force-raises
 * unconditionally). Result: the confirmation is live and pumping X events
 * (a "hung" matchbox-fbrun is really just this, waiting forever) but never
 * visible, so it can never be answered.
 *
 * Tagging the window _NET_WM_WINDOW_TYPE_DIALOG ourselves (wm.c:2039) is
 * what routes it into that always-visible path. Must happen before show()
 * maps the window -- see the comment at the XChangeProperty call below.
 */
static int dialog_result = 0;

static void abort_cb(Fl_Widget *, void *w)     { dialog_result = 0; ((Fl_Window *)w)->hide(); }
static void continue_cb(Fl_Widget *, void *w)  { dialog_result = 1; ((Fl_Window *)w)->hide(); }

/* Collapsed dialog height (today's original size) and how much taller it
 * grows to fit the "Video mode:" label + list when Advanced is toggled on.
 * The button row (advanced/abort/continue) sits at DIALOG_BTN_Y normally,
 * or DIALOG_BTN_Y + DIALOG_EXTRA_H once expanded -- everything else about
 * the layout (text box, label, list) stays at fixed positions and is only
 * shown/hidden. */
#define DIALOG_BASE_W   420
#define DIALOG_BASE_H   200
#define DIALOG_EXTRA_H  150
#define DIALOG_BTN_Y    150

struct advanced_ui {
    Fl_Window        *win;
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
        ui->mode_label->show();
        ui->list->show();
    } else {
        ui->mode_label->hide();
        ui->list->hide();
    }

    ui->win->size(DIALOG_BASE_W, DIALOG_BASE_H + (ui->expanded ? DIALOG_EXTRA_H : 0));
    ui->advanced_btn->position(ui->advanced_btn->x(), button_y);
    ui->abort_btn->position(ui->abort_btn->x(), button_y);
    ui->continue_btn->position(ui->continue_btn->x(), button_y);
    ui->win->redraw();
}

/*
 * Asked while X is still up, because afterwards there is nothing left to ask
 * with. Returns non-zero to proceed. *mode is both the Advanced panel's
 * initial selection (the caller's already-resolved CLI-flags-or-config
 * choice) and, on return, whatever the user left it at -- unchanged if they
 * never opened Advanced.
 */
static int confirm(const char *name, const char *reason, enum video_mode *mode)
{
    char msg[512];
    struct advanced_ui ui;

    snprintf(msg, sizeof(msg),
             "%s needs the whole screen.\n\n"
             "Every other application that is running will be closed, and "
             "the desktop will come back when %s exits.\n%s%s",
             name, name,
             reason ? "\n" : "",
             reason ? reason : "");

    Fl_Window win(DIALOG_BASE_W, DIALOG_BASE_H, "Start application");
    Fl_Box text(10, 10, 400, 130, msg);
    text.align(FL_ALIGN_WRAP | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE);

    Fl_Box mode_label(10, 145, 200, 20, "Video mode:");
    mode_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    mode_label.hide();

    VideoModeList mode_list(10, 168, 400, 112);
    mode_list.selected(*mode);
    mode_list.hide();

    /* Advanced sits bottom-left; Abort/Continue stay bottom-right, still
     * rendering right to left (Continue rightmost/default), matching
     * fl_choice()'s old layout and the thumb-ergonomics reasoning above. */
    Fl_Button advanced_btn(10, DIALOG_BTN_Y, 90, 30, "Advanced");
    Fl_Button abort_btn(210, DIALOG_BTN_Y, 100, 30, "Abort");
    Fl_Return_Button continue_btn(310, DIALOG_BTN_Y, 100, 30, "Continue");
    win.end();

    dialog_result = 0;
    ui.win = &win;
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

    /*
     * Must land before the window manager processes the MapRequest that
     * win.show() just triggered, not after: with SubstructureRedirect set
     * on the root window (which every window manager sets), a client's own
     * XMapWindow doesn't map anything by itself, it only asks the X
     * server to notify the window manager -- so the actual property value
     * the window manager reads back on its own subsequent
     * XGetWindowProperty call is whatever was last written by the time it
     * gets around to asking, regardless of ordering against our show()
     * call. X serialises requests from a single connection, so this
     * XChangeProperty is guaranteed visible to any later query from any
     * client, including the window manager, once this call returns.
     */
    Atom window_type = XInternAtom(fl_display, "_NET_WM_WINDOW_TYPE", False);
    Atom dialog_type = XInternAtom(fl_display, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    XChangeProperty(fl_display, fl_xid(&win), window_type, XA_ATOM, 32,
                     PropModeReplace, (unsigned char *)&dialog_type, 1);
    XSync(fl_display, False);

    /* Read back what the server actually stored, not what we think we
     * sent -- confirms the property really landed on this window rather
     * than silently failing (e.g. BadWindow from an xid that wasn't
     * really ready yet). */
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

/* ── main ───────────────────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
            "usage: matchbox-fbrun [-n NAME] [-r REASON] [-y] [--qvga] "
            "[--fast-pll] [--] program [args...]\n");
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
        else if (!strcmp(argv[i], "--"))                 { i++; break; }
        else if (argv[i][0] == '-')                      { usage(); return 2; }
        else                                             break;
    }

    /* An explicit --qvga/--fast-pll always wins for this run. With neither
     * given, fall back to whatever the Advanced panel last saved -- see
     * the usage comment at the top of this file. */
    if (mode_flag_given) {
        mode = mode_from_flags(qvga_requested, fast_pll_requested);
    } else {
        mode = load_video_mode_config();
        mode_to_flags(mode, &qvga_requested, &fast_pll_requested);
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

    /*
     * Already inside one of these: the session is stopped and the console is
     * ours, so there is nothing to ask, close, or restore. Just become the
     * program -- the outer instance is what puts everything back.
     */
    if (getenv(REENTRY_ENV)) {
        execvp(prog_argv[0], prog_argv);
        trace("main: re-entrant execvp failed: %s", strerror(errno));
        fprintf(stderr, "matchbox-fbrun: cannot run %s: %s\n",
                prog_argv[0], strerror(errno));
        return 127;
    }

    if (!take_lock()) {
        fprintf(stderr, "matchbox-fbrun: another one is already running\n");
        return 0;
    }

    /*
     * Detach from whatever session forked us -- found necessary live, via
     * the trace log above: matchbox-desktop is one of the toplevels
     * close_other_clients() below asks to close (it owns a window, same as
     * any other client), and if it treats its own WM_DELETE_WINDOW as "quit"
     * and exits, it was this process's session leader -- so the kernel
     * SIGHUPs the whole foreground process group, us included, BEFORE
     * fork() ever happens. session_stop() was still mid-wait when that hit,
     * so session_was_stopped (assigned only when it returns) was still 0,
     * and cleanup() -- seeing that -- skipped session_restore(): the
     * fallback getty that had already appeared was never told to get out of
     * the way, leaving the device stuck at a console with nothing left
     * running to fix it. setsid() makes this process (and, by inheritance,
     * the child it forks below) its own session leader, immune to
     * whatever happens to the one that launched it. Failure is not fatal --
     * traced and otherwise ignored, since the alternative is not running at
     * all over something that, at worst, restores the older racy behaviour.
     */
    if (setsid() < 0)
        trace("main: setsid failed: %s (staying in the parent's session)", strerror(errno));
    else
        trace("main: setsid ok, new session id=%d", (int)getpid());

    install_handlers();
    atexit(cleanup);

    /*
     * Only ask when there is a session to lose. Run from a console -- no X,
     * or no DISPLAY to reach it through -- there is nothing to close and
     * nothing to warn about, and this behaves like the old fbrun.
     */
    if (!assume_yes && getenv("DISPLAY") && x_is_running()) {
        Display *dpy = XOpenDisplay(NULL);

        trace("main: DISPLAY set and %s running -- %s", XSERVER_NAME,
              dpy ? "opened, showing confirm dialog" : "XOpenDisplay FAILED, skipping dialog");

        if (dpy) {
            int proceed = confirm(name, reason, &mode);

            if (!proceed) {
                trace("main: user chose Abort");
                XCloseDisplay(dpy);
                return 0;         /* atexit puts the lock back */
            }

            /* Sync back from whatever the Advanced panel was left at (a
             * no-op if the user never opened it) and persist it as the
             * new default for next time. */
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

    session_was_stopped = session_stop();
    trace("main: session_was_stopped=%d, forking", session_was_stopped);

    if (qvga_requested)
        qvga_applied = set_qvga_mode();
    if (fast_pll_requested)
        fast_pll_applied = set_fast_pll(1);

    show_splash(name);

    child_pid = fork();
    if (child_pid < 0) {
        trace("main: fork failed: %s", strerror(errno));
        perror("matchbox-fbrun: fork");
        return 126;
    }
    if (child_pid == 0) {
        /*
         * Default disposition for the signals we trapped: the application
         * should be free to handle them itself, and must not inherit our
         * cleanup handler and run it a second time.
         */
        signal(SIGINT,  SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        setenv(REENTRY_ENV, "1", 1);
        trace("main: child pid=%d exec'ing %s", (int)getpid(), prog_argv[0]);
        execvp(prog_argv[0], prog_argv);
        trace("main: child execvp failed: %s", strerror(errno));
        fprintf(stderr, "matchbox-fbrun: cannot run %s: %s\n",
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
