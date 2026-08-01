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
 *   matchbox-fbrun [-n NAME] [-r REASON] [-y] [--] program [args...]
 *
 *   -n NAME    application name for the dialog (default: the binary's name)
 *   -r REASON  extra line of explanation in the dialog
 *   -y         skip the dialog and proceed (for scripts and for the
 *              console case, where there is nothing to ask about)
 *
 * EXIT STATUS
 *   the program's own exit status, or 0 if the user chose Abort, or
 *   126/127 in the manner of a shell if it could not be run at all.
 */

#include <FL/Fl.H>
#include <FL/fl_ask.H>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <signal.h>
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

/* ── state the cleanup path needs ───────────────────────────────────────── */

static pid_t child_pid;
static int   session_was_stopped;
static int   lock_held;
static int   cleaned_up;

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
 * console stuck with no visible shell. Doing it here unconditionally is the
 * safety net the standalone fbtext used to provide.
 */
static void console_text_mode(void)
{
    int fd = open("/dev/tty0", O_RDWR);

    if (fd < 0)
        fd = open("/dev/console", O_RDWR);
    if (fd < 0)
        return;
    ioctl(fd, KDSETMODE, KD_TEXT);
    close(fd);
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
}

static int session_stop(void)
{
    pid_t pids[8];
    int n, i, waited;

    n = find_pids(XSERVER_NAME, NULL, pids, 8);
    if (n == 0)
        return 0;                 /* nothing to stop; console-only case */

    for (i = 0; i < n; i++)
        kill(pids[i], SIGTERM);

    for (waited = 0; waited < STOP_TIMEOUT_S; waited++) {
        if (find_pids(XSERVER_NAME, NULL, pids, 8) == 0)
            break;
        sleep(1);
    }

    /* Refuses to go quietly; it is holding the framebuffer we need. */
    n = find_pids(XSERVER_NAME, NULL, pids, 8);
    for (i = 0; i < n; i++)
        kill(pids[i], SIGKILL);
    if (n)
        sleep(1);

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
    for (i = 0; i < n; i++)
        kill(pids[i], SIGTERM);
}

/* ── cleanup ────────────────────────────────────────────────────────────── */

static void cleanup(void)
{
    if (cleaned_up)
        return;
    cleaned_up = 1;

    console_text_mode();

    if (session_was_stopped)
        session_restore();

    if (lock_held)
        unlink(LOCK_PATH);
}

/*
 * Signals have to be handled, not ignored: without this a plain `kill` of
 * the wrapper would leave the console in graphics mode and the desktop
 * stopped -- indistinguishable, from the user's chair, from a dead device.
 */
static void sig_handler(int sig)
{
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
            if (owner > 0 && process_alive(owner))
                return 0;
        }
    }

    unlink(LOCK_PATH);
    fd = open(LOCK_PATH, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0)
        return 0;
    close(fd);
    lock_held = 1;
    return 1;
}

/* ── the dialog ─────────────────────────────────────────────────────────── */

/*
 * Asked while X is still up, because afterwards there is nothing left to ask
 * with. Returns non-zero to proceed.
 */
static int confirm(const char *name, const char *reason)
{
    char msg[512];

    snprintf(msg, sizeof(msg),
             "%s needs the whole screen.\n\n"
             "Every other application that is running will be closed, and "
             "the desktop will come back when %s exits.\n%s%s",
             name, name,
             reason ? "\n" : "",
             reason ? reason : "");

    fl_message_title("Start application");

    /* Buttons render right to left, so index 0 is the safe default. */
    return fl_choice("%s", "Abort", "Continue", (const char *)0, msg) == 1;
}

/* ── main ───────────────────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
            "usage: matchbox-fbrun [-n NAME] [-r REASON] [-y] [--] "
            "program [args...]\n");
}

int main(int argc, char **argv)
{
    const char *name = NULL, *reason = NULL;
    int assume_yes = 0;
    int i, status = 0;
    char **prog_argv;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)      name   = argv[++i];
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) reason = argv[++i];
        else if (!strcmp(argv[i], "-y"))                 assume_yes = 1;
        else if (!strcmp(argv[i], "--"))                 { i++; break; }
        else if (argv[i][0] == '-')                      { usage(); return 2; }
        else                                             break;
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

    /*
     * Already inside one of these: the session is stopped and the console is
     * ours, so there is nothing to ask, close, or restore. Just become the
     * program -- the outer instance is what puts everything back.
     */
    if (getenv(REENTRY_ENV)) {
        execvp(prog_argv[0], prog_argv);
        fprintf(stderr, "matchbox-fbrun: cannot run %s: %s\n",
                prog_argv[0], strerror(errno));
        return 127;
    }

    if (!take_lock()) {
        fprintf(stderr, "matchbox-fbrun: another one is already running\n");
        return 0;
    }

    install_handlers();
    atexit(cleanup);

    /*
     * Only ask when there is a session to lose. Run from a console -- no X,
     * or no DISPLAY to reach it through -- there is nothing to close and
     * nothing to warn about, and this behaves like the old fbrun.
     */
    if (!assume_yes && getenv("DISPLAY") && x_is_running()) {
        Display *dpy = XOpenDisplay(NULL);

        if (dpy) {
            int proceed = confirm(name, reason);

            if (!proceed) {
                XCloseDisplay(dpy);
                return 0;         /* atexit puts the lock back */
            }
            close_other_clients(dpy);
            XCloseDisplay(dpy);
        }
    }

    session_was_stopped = session_stop();

    child_pid = fork();
    if (child_pid < 0) {
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
        execvp(prog_argv[0], prog_argv);
        fprintf(stderr, "matchbox-fbrun: cannot run %s: %s\n",
                prog_argv[0], strerror(errno));
        _exit(127);
    }

    while (waitpid(child_pid, &status, 0) < 0 && errno == EINTR)
        ;
    child_pid = 0;

    cleanup();

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 0;
}
