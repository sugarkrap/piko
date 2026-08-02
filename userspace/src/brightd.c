/*
 * brightd -- backlight policy daemon for the Sharp Zaurus C7x0 (corgi).
 *
 * Does five things, all of them by watching the evdev nodes directly:
 *
 *   1. Fn+3 / Fn+4 step the backlight down / up.
 *   2. After an idle period the backlight dims, then blanks; any key or
 *      touch restores it.
 *   3. Between dim and blank, shows a screensaver (see SCREENSAVER CONTENT
 *      below) so the panel is not just sitting dim before it goes dark.
 *   4. Closing the lid blanks immediately; opening it restores.
 *   5. Optionally (suspend_on_lid=yes, off by default), closing the lid
 *      also suspends the whole system, not just the backlight. See
 *      SUSPEND ON LID below before turning this on.
 *
 * WHY NOT X
 * ---------
 * The obvious implementation is an X client using XScreenSaverQueryInfo
 * plus matchbox keybindings. Both are wrong for this device:
 *
 *   * There is no xset, no xdpyinfo and no xrandr on this rootfs, and
 *     libXss is not built, so the X idle path would mean adding a
 *     dependency chain to get a number we can derive from evdev for free.
 *   * The hotkeys cannot go through matchbox's kbdconfig. matchbox grabs
 *     a binding with an explicit modifier mask (keys_grab() in
 *     matchbox-window-manager/src/keys.c), and userspace/xkb/symbols/zaurus
 *     declares ISO_Level3_Shift on <FK03> with NO modifier_map entry, so
 *     which real modifier bit Fn sets -- if any -- is not defined by the
 *     layout. Both possible answers break the binding: if Fn does set a
 *     real bit then a mask-0 grab never fires on Fn+4, and if it sets none
 *     then that same grab fires on a bare "4" and the digit becomes
 *     untypable. At the evdev layer Fn is just KEY_F3 being held and the
 *     chord is unambiguous.
 *   * Doing it here also means brightness keys and idle dimming work on
 *     the console and before the graphical session starts, which matters
 *     on a machine whose only remote access is WiFi.
 *
 * The XKB layout still maps Fn+3/Fn+4 to XF86MonBrightness{Down,Up} so the
 * keys report something meaningful to anything that looks; nothing grabs
 * those keysyms, so there is no double-stepping.
 *
 * WHY IT SHELLS OUT FOR THE HOTKEYS
 * ---------------------------------
 * Stepping calls /usr/sbin/bright, so the step ladder is defined in
 * exactly one place. Dim/blank/restore are done here with direct sysfs
 * writes instead, because those are on the hot path and must not disturb
 * the level the user chose (which is what we restore to).
 *
 * COST OF A WRITE
 * ---------------
 * Every write to .../brightness is an SSP transaction on the bus shared
 * with the touchscreen and the battery ADC, and corgi_lcd's kick_battery
 * hook additionally pokes sharpsl-pm on each one. So: no fades, no
 * animation, and never write a value that is already set.
 *
 * NO SIGNAL PROTOCOL
 * ------------------
 * This rootfs's BusyBox has no kill, killall or pkill -- the only way to
 * signal anything is the project's own /usr/sbin/pkillx, which matches on
 * the process basename ("pkillx brightd" stops this daemon). That is fine
 * for stopping it, but it is a poor control channel, so runtime state is
 * not driven by signals: to suppress dimming temporarily (video playback,
 * which produces no input events for minutes at a time) create the
 * inhibit file instead; see INHIBIT below.
 *
 * CONFIGURATION
 * -------------
 * /etc/zaurus/power-management.cfg, if present, overrides the DEF_*
 * defaults below with "key=value" lines -- same format matchbox's
 * kbdconfig uses (matchbox-window-manager/src/keys.c). Lines starting
 * with '#' are comments; there is no quoting, escaping, or trailing-
 * comment support, so keep it to bare key=value per line. Recognised
 * keys: dim_secs, toast_secs, blank_secs, dim_level, suspend_on_lid.
 *
 * Reloaded automatically: the file's mtime is checked once per main-loop
 * iteration (see load_config()), so a saved edit takes effect within
 * TICK_SECS with no signal needed -- convenient, since NO SIGNAL PROTOCOL
 * above means there would be no way to ask for a reload otherwise.
 * Deleting the file does NOT revert to compiled defaults; whatever was
 * last loaded stays in effect, since a missing file is not a clear signal
 * of intent to reset (could just as easily be a botched edit mid-save).
 *
 * -d/-b/-l on the command line always win over the config file, even
 * across a reload -- see dim_secs_cli et al. below -- so a manual test
 * ("brightd -d 5" while debugging) is never silently overridden by
 * whatever the file says. suspend_on_lid has no command-line equivalent;
 * it is config-only, for the reason in SUSPEND ON LID below.
 *
 * SUSPEND ON LID (opt-in, default off)
 * -------------------------------------
 * suspend_on_lid=yes makes closing the lid do a real system suspend
 * (echo mem > /sys/power/state), not just blank the backlight. "mem"
 * specifically: this board's cpu_pm_fns.valid is suspend_valid_only_mem
 * (modules/mach-pxa/pxa25x_patched.c), the only state the kernel accepts.
 *
 * Default OFF is deliberate, not timid. Per AGENTS.md this project's
 * board is the LAST SPARE ONE, with no serial console and no USB -- the
 * only way in is WiFi -> SSH. A suspend that goes to sleep but never
 * wakes (a bad resume path, a wedged driver) is not a "power-cycle and
 * try again" problem the way it would be on ordinary hardware: it is the
 * one remote-access path gone, indefinitely, with nobody local to press
 * anything. Confirm `echo mem > /sys/power/state` resumes reliably by
 * hand over SSH, repeatedly, before ever setting suspend_on_lid=yes.
 *
 * SCREENSAVER CONTENT (toast_secs, default 120)
 * ----------------------------------------------
 * Between dim and blank, once idle >= toast_secs, brightd forks/execs
 * /usr/local/bin/toasters (userspace/src/toasters.c -- a small Xlib
 * "flying toasters" animation) so the panel shows something before it
 * actually goes dark. Killed with SIGTERM the instant there is activity
 * (go_active()) or it is time to really blank the backlight (go_blank()):
 * this is content on top of the existing idle timer, not a new state to
 * track, and brightd still does not link X (see WHY NOT X above) -- it
 * only forks a binary and checks for /tmp/.X11-unix/X0 first, the same
 * marker rootfs/etc/init.d/xsession itself waits on at startup.
 *
 * 0 disables it, same convention as dim_secs/blank_secs. Expected ordering
 * is dim_secs <= toast_secs <= blank_secs, but nothing enforces that --
 * set it outside that range and you get exactly what the numbers say.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <linux/input.h>

#define BL_DIR     "/sys/class/backlight/corgi_bl"
#define BRIGHT_CMD "/usr/sbin/bright"
#define TOASTERS_BIN "/usr/local/bin/toasters"

/*
 * Presence of this file suspends dimming and blanking entirely (the
 * hotkeys and the lid switch keep working). Intended for video playback,
 * which produces no input events for minutes at a time and would
 * otherwise dim in the user's face. A file rather than a signal because
 * there is no kill on this device.
 */
#define INHIBIT    "/tmp/brightd.inhibit"

/* See CONFIGURATION above. */
#define CONFIG_PATH "/etc/zaurus/power-management.cfg"

/*
 * Event channel from the X server.
 *
 * Xfbdev EVIOCGRABs the keyboard and touchscreen (see input_is_grabbed()),
 * so while X runs it is the ONLY process that can see them. Our xserver
 * fork therefore feeds this FIFO and brightd keeps owning policy -- X is
 * a pure event source and makes no backlight decisions of its own.
 *
 * One byte per message:
 *
 *   'h'  heartbeat / hello. "I am alive and I am feeding you events."
 *        NOT activity. Sent on open and then every few seconds
 *        regardless of input.
 *   'a'  input activity (any key or touch). Rate-limited by the sender;
 *        this is a wake-up, not an event log.
 *   'u'  brightness up   (Fn+4)
 *   'd'  brightness down (Fn+3)
 *   's'  screen saver ON.  X's own DPMS/screensaver core wants the panel
 *        blanked -- sent by fbdevDPMS() (hw/kdrive/fbdev/fbdev.c), on a
 *        writer fd of its own, since real hardware blanking on this board
 *        stops at bl_power and brightd is the only thing that touches it.
 *        Must NOT count as activity, or go_blank() would be immediately
 *        undone by the activity handling below.
 *   'w'  screen saver OFF. Counts as activity: X only sends this because
 *        something legitimate happened (DPMSForceLevel, XSetScreenSaver
 *        reset, a future screensaver client, ...), same as 'u'/'d'.
 *
 * The heartbeat is what makes the grab survivable. Without it we cannot
 * tell "X is grabbing input and forwarding it, and the user really is
 * idle" (dim!) from "X is grabbing input and telling us nothing" (do not
 * dim, we are blind). Absence of 'a' means both. Presence of a recent
 * 'h' distinguishes them.
 */
#define FIFO_PATH  "/tmp/brightd.fifo"

/*
 * THE ON-SCREEN DISPLAY
 *
 * mb-brightness (a hidden panel applet -- it docks no icon and exists
 * only to draw) shows a bar at the top of the screen with the backlight
 * level in it. It owns none of the backlight: we poke it with one byte
 * and it then READS /sys/class/backlight itself, so it can never
 * disagree with what actually happened here. Same one-owner rule this
 * daemon already has with /usr/sbin/bright.
 *
 *   's'  show the OSD, re-reading the level from sysfs
 *
 * Poked from run_bright() and nowhere else, which is exactly the set of
 * EXPLICIT user changes -- Fn+3/Fn+4, whether they arrive from evdev
 * directly (console) or over FIFO_PATH from the X server. Idle dimming,
 * lid blanking and the DPMS path deliberately do NOT poke it: they call
 * go_dim()/go_blank()/go_active(), which write sysfs directly and never
 * go through run_bright(). An OSD lighting the screen up as it dims to
 * save power would be both absurd and self-defeating.
 *
 * Opened per poke rather than held open like our own FIFO above: the
 * applet comes and goes with the X session, so a cached descriptor would
 * spend most of its life stale. At autorepeat rates the open/write/close
 * is lost in the noise next to the fork+exec run_bright() already does.
 */
#define OSD_FIFO   "/tmp/mb-brightness.fifo"

/* How long a heartbeat vouches for the X event source. Must be
 * comfortably longer than the sender's heartbeat interval. */
#define HEARTBEAT_TTL 30

/* Defaults, all overridable from the command line -- see usage(). */
#define DEF_DIM_SECS    60
#define DEF_TOAST_SECS  120
#define DEF_BLANK_SECS  300
#define DEF_DIM_LEVEL   5

/* Poll granularity. 2s is far coarser than input latency (input wakes us
 * immediately via select) and only bounds how promptly the idle timer
 * fires, so it costs essentially nothing on a 400MHz part. */
#define TICK_SECS       2

/* How often to re-check whether our input nodes are grabbed. See
 * input_is_grabbed(). */
#define GRAB_PROBE_SECS 30

static const char *dev_switch = "/dev/input/event0";  /* gpio-keys-polled: SW_LID  */
static const char *dev_keys   = "/dev/input/event1";  /* matrix-keypad             */
static const char *dev_touch  = "/dev/input/event2";  /* ADS7846 touchscreen       */

static int dim_secs   = DEF_DIM_SECS;
static int toast_secs = DEF_TOAST_SECS;
static int blank_secs = DEF_BLANK_SECS;
static int dim_level  = DEF_DIM_LEVEL;
static int verbose    = 0;
static int suspend_on_lid = 0;

/* Set when the corresponding value came from the command line, so a
 * config (re)load never clobbers an explicit manual override. */
static int dim_secs_cli = 0, blank_secs_cli = 0, dim_level_cli = 0,
	toast_secs_cli = 0;

/* pid of a running userspace/src/toasters.c, or -1 if none. See
 * start_toaster()/stop_toaster() and SCREENSAVER CONTENT above. */
static pid_t toaster_pid = -1;

/* mtime of CONFIG_PATH as of the last successful load. 0 means "never
 * loaded" -- distinct from a real mtime of 0, which cannot happen on a
 * device whose clock is always well past 1970. */
static time_t cfg_mtime = 0;

/* Backlight state machine. */
enum { ST_ACTIVE, ST_DIMMED, ST_BLANKED };
static int state = ST_ACTIVE;

/* Level to return to when activity resumes; captured at the moment we dim
 * so that whatever the user had chosen is what comes back. */
static int saved_level = -1;

/* Whether someone else (X) holds an exclusive grab on our input, and when
 * we last checked. See input_is_grabbed(). */
static int starved = 0;
static time_t last_probe = 0;

/* When we last heard a heartbeat on FIFO_PATH. 0 means never. */
static time_t last_heartbeat = 0;

/* When the last input activity was seen. File-scope (not a main() local)
 * because do_suspend() needs to reset it after a resume. */
static time_t last_activity = 0;

static void
usage(void)
{
	puts("brightd -- backlight policy daemon");
	puts("  -d SECS   idle seconds before dimming   (0 disables, default 60)");
	puts("  -t SECS   idle seconds before the toasters screensaver");
	puts("                                           (0 disables, default 120)");
	puts("  -b SECS   idle seconds before blanking  (0 disables, default 300)");
	puts("  -l LEVEL  level to dim to               (default 5)");
	puts("  -v        log transitions to stdout");
	puts("");
	puts("Fn+3 / Fn+4 step brightness. Closing the lid blanks.");
	puts("Create /tmp/brightd.inhibit to suspend dimming (e.g. video).");
	puts("");
	puts("/etc/zaurus/power-management.cfg overrides dim_secs/blank_secs/");
	puts("dim_level above and adds suspend_on_lid=yes; re-read live on change.");
	puts("-d/-b/-l here always win over the config file.");
}

static int
read_int(const char *name)
{
	char path[128];
	char buf[32];
	int fd, n;

	snprintf(path, sizeof(path), "%s/%s", BL_DIR, name);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return atoi(buf);
}

static int
write_int(const char *name, int value)
{
	char path[128];
	char buf[32];
	int fd, n, len;

	snprintf(path, sizeof(path), "%s/%s", BL_DIR, name);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	len = snprintf(buf, sizeof(buf), "%d\n", value);
	n = write(fd, buf, len);
	close(fd);
	return n == len ? 0 : -1;
}

/*
 * Deliberately reads "brightness" and never "actual_brightness".
 * corgi_bl_set_intensity() adds 0x10 to any value above 0x10 and stores
 * the result as the value .get_brightness returns, so actual_brightness
 * reports 47 when the panel is at 31 and 63 when it is at 47 -- past its
 * own max_brightness. Measured on hardware 2026-07-31.
 */
static int
current_level(void)
{
	return read_int("brightness");
}

static time_t
now_mono(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		return ts.tv_sec;
	return time(NULL);
}

static int
inhibited(void)
{
	struct stat st;

	return stat(INHIBIT, &st) == 0;
}

/*
 * Can we actually still see input?
 *
 * Xfbdev (kdrive) calls EVIOCGRAB on both the keyboard and the pointer --
 * hw/kdrive/linux/evdev.c, in the enable path for each. An evdev grab is
 * taken on the input *device* (input_grab_device sets dev->grab), not on
 * the handler, so while it is held EVERY other reader is starved: other
 * evdev clients and other handlers such as mousedev alike. Confirmed on
 * hardware 2026-07-31 -- with X up, event1 and event2 both refuse a grab
 * with EBUSY while event0 (the lid) stays free.
 *
 * That matters enormously here: a starved brightd sees no activity at
 * all, concludes the machine is idle, and dims and then blanks the panel
 * while the user is typing. Idle policy is therefore suspended whenever
 * the keyboard is grabbed by someone else. The lid switch keeps working,
 * because nothing grabs event0.
 *
 * The probe is a grab attempt, which is the only reliable answer. It is
 * cheap but not entirely free: on an UNgrabbed device we hold the grab
 * for the instant between the two ioctls, so it is rate-limited below and
 * only consulted when the idle timer is about to act.
 */
static int
input_is_grabbed(int fd)
{
	if (fd < 0)
		return 0;
	if (ioctl(fd, EVIOCGRAB, 1) == 0) {
		ioctl(fd, EVIOCGRAB, 0);
		return 0;
	}
	return 1;
}

static void
say(const char *msg)
{
	if (verbose) {
		puts(msg);
		fflush(stdout);
	}
}

/*
 * See CONFIGURATION in the file header. Safe to call every main-loop
 * iteration: a stat() against an unchanged mtime is the only cost when
 * nothing has changed, and this daemon has no other way to be told to
 * reload (NO SIGNAL PROTOCOL above).
 */
static void
load_config(void)
{
	struct stat st;
	FILE *f;
	char line[256];

	if (stat(CONFIG_PATH, &st) < 0)
		return;		/* no file: keep whatever is already in effect */
	if (cfg_mtime && st.st_mtime == cfg_mtime)
		return;		/* unchanged since last load */

	f = fopen(CONFIG_PATH, "r");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *nl, *eq, *key, *val;

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

		if (!strcmp(key, "dim_secs")) {
			if (!dim_secs_cli)
				dim_secs = atoi(val);
		} else if (!strcmp(key, "blank_secs")) {
			if (!blank_secs_cli)
				blank_secs = atoi(val);
		} else if (!strcmp(key, "dim_level")) {
			if (!dim_level_cli)
				dim_level = atoi(val);
		} else if (!strcmp(key, "toast_secs")) {
			if (!toast_secs_cli)
				toast_secs = atoi(val);
		} else if (!strcmp(key, "suspend_on_lid")) {
			suspend_on_lid = !strcmp(val, "yes") || !strcmp(val, "1");
		}
		/* Unrecognised keys are ignored rather than rejected, so the
		 * file can grow without an old brightd refusing to start. */
	}
	fclose(f);

	cfg_mtime = st.st_mtime;
	say("brightd: loaded " CONFIG_PATH);
}

/*
 * See SCREENSAVER CONTENT in the file header.
 *
 * Checks for the X socket rather than trying and swallowing a failure:
 * toasters itself would just exit(1) with nothing to connect to, but
 * that would mean forking a doomed child every TICK_SECS for as long as
 * the machine sits idle at the console with no X running at all.
 */
static void
start_toaster(void)
{
	struct stat st;

	if (toaster_pid > 0)
		return;
	if (stat("/tmp/.X11-unix/X0", &st) < 0)
		return;

	toaster_pid = fork();
	if (toaster_pid < 0) {
		toaster_pid = -1;
		return;
	}
	if (toaster_pid == 0) {
		setenv("DISPLAY", ":0", 1);
		execl(TOASTERS_BIN, "toasters", (char *)NULL);
		_exit(127);
	}
	say("brightd: toasters started");
}

/*
 * SIGTERM, not the FIFO/signal-free approach used everywhere else in this
 * file: toasters is a child *this process* forked, so kill(2) here is a
 * plain libc call, not the "no kill applet on this rootfs" problem NO
 * SIGNAL PROTOCOL above is about -- that is about signalling some *other*
 * process from the shell. Blocking on waitpid() is fine: an Xlib client
 * gets no chance to ignore SIGTERM's default disposition, so this returns
 * about as fast as the X server processes the connection closing.
 */
static void
stop_toaster(void)
{
	if (toaster_pid <= 0)
		return;
	kill(toaster_pid, SIGTERM);
	waitpid(toaster_pid, NULL, 0);
	toaster_pid = -1;
	say("brightd: toasters stopped");
}

static void
go_dim(void)
{
	int cur = current_level();
	int target;

	if (cur < 0)
		return;

	saved_level = cur;

	/* Dimming must never brighten: if the user is already below the dim
	 * level, leave the panel alone and just record the state. */
	target = cur < dim_level ? cur : dim_level;
	if (target != cur)
		write_int("brightness", target);

	state = ST_DIMMED;
	say("brightd: dim");
}

static void
go_blank(void)
{
	stop_toaster();

	/* Only capture the level if we did not already do so on the way
	 * through ST_DIMMED, otherwise we would save the dim level and
	 * restore to that. */
	if (state == ST_ACTIVE) {
		int cur = current_level();
		if (cur >= 0)
			saved_level = cur;
	}

	/*
	 * bl_power rather than brightness 0: the driver keeps the brightness
	 * value intact while blanked (verified on hardware -- brightness
	 * still read 47 with bl_power=4), so unblanking is one write and
	 * cannot lose the user's level.
	 */
	write_int("bl_power", 4);
	state = ST_BLANKED;
	say("brightd: blank");
}

static void
go_active(void)
{
	stop_toaster();

	if (state == ST_ACTIVE)
		return;

	if (state == ST_BLANKED)
		write_int("bl_power", 0);

	if (saved_level >= 0 && current_level() != saved_level)
		write_int("brightness", saved_level);

	saved_level = -1;
	state = ST_ACTIVE;
	say("brightd: active");
}

/*
 * See SUSPEND ON LID in the file header. Only called when
 * suspend_on_lid=yes, from the lid-close edge.
 *
 * Blocks for the duration of the sleep -- the CPU is off, so there is
 * nothing else this process could usefully be doing meanwhile. Whatever
 * eventually wakes the board is handled by the normal event loop once
 * this write returns; go_active() here just guarantees the backlight
 * itself comes back rather than staying at whatever bl_power/brightness
 * state the kernel happened to resume into.
 */
static void
do_suspend(void)
{
	int fd = open("/sys/power/state", O_WRONLY);

	if (fd < 0) {
		fprintf(stderr, "brightd: cannot open /sys/power/state: %s\n",
			strerror(errno));
		return;
	}

	say("brightd: suspending (echo mem > /sys/power/state)");
	if (write(fd, "mem", 3) != 3)
		fprintf(stderr, "brightd: suspend failed: %s\n", strerror(errno));
	close(fd);

	say("brightd: resumed");
	go_active();
	last_activity = now_mono();
}

/*
 * Run "bright up" / "bright down". fork+exec rather than duplicating the
 * step ladder in C -- /usr/sbin/bright stays the single definition of what
 * a step is. Waits for the child so two fast presses cannot race each
 * other's read-modify-write of the sysfs value.
 */
/*
 * Tell mb-brightness to show the OSD (see OSD_FIFO above). Entirely
 * best-effort and silent: ENOENT means the applet has never run and
 * ENXIO means it is not running now, both of which are the ordinary
 * state of affairs on a console-only boot. O_NONBLOCK is what keeps
 * those from becoming a blocking open, and SIGPIPE is already SIG_IGN
 * here so a vanished reader gives EPIPE rather than killing us.
 */
static void
notify_osd(void)
{
	int fd = open(OSD_FIFO, O_WRONLY | O_NONBLOCK);

	if (fd < 0)
		return;

	/* Droppable by design -- a missed poke costs one un-drawn OSD. */
	(void)write(fd, "s", 1);
	close(fd);
}

static void
run_bright(const char *arg)
{
	pid_t pid = fork();

	if (pid < 0)
		return;

	if (pid == 0) {
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			if (fd > STDERR_FILENO)
				close(fd);
		}
		execl(BRIGHT_CMD, "bright", arg, (char *)NULL);
		_exit(127);
	}

	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;

	/* After the wait, never before: the applet reads the level out of
	 * sysfs for itself, so it has to be poked once the child has
	 * actually written it or it would draw the previous step. */
	notify_osd();
}

/*
 * Open the X event channel.
 *
 * We hold a second, write-side descriptor on purpose. A FIFO whose last
 * writer closes goes to permanent EOF: read() returns 0 and select()
 * reports it readable forever, which would spin this loop at 100% CPU the
 * moment X exited. Keeping one writer of our own open means there is
 * always at least one, so the read side simply blocks (well, returns
 * EAGAIN) instead of ever seeing EOF.
 */
static int
open_fifo(int *write_fd)
{
	int fd;

	if (mkfifo(FIFO_PATH, 0666) < 0 && errno != EEXIST) {
		fprintf(stderr, "brightd: mkfifo %s: %s\n",
			FIFO_PATH, strerror(errno));
		return -1;
	}

	fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "brightd: open %s: %s\n",
			FIFO_PATH, strerror(errno));
		return -1;
	}

	*write_fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
	return fd;
}

static int
open_input(const char *path)
{
	int fd = open(path, O_RDONLY | O_NONBLOCK);

	if (fd < 0)
		fprintf(stderr, "brightd: cannot open %s: %s\n",
			path, strerror(errno));
	return fd;
}

int
main(int argc, char **argv)
{
	int fd_sw, fd_key, fd_touch, maxfd;
	int fd_fifo, fd_fifo_w = -1;
	int fn_held = 0;
	int lid_closed = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			dim_secs = atoi(argv[++i]);
			dim_secs_cli = 1;
		} else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			toast_secs = atoi(argv[++i]);
			toast_secs_cli = 1;
		} else if (!strcmp(argv[i], "-b") && i + 1 < argc) {
			blank_secs = atoi(argv[++i]);
			blank_secs_cli = 1;
		} else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
			dim_level = atoi(argv[++i]);
			dim_level_cli = 1;
		} else if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else {
			usage();
			return strcmp(argv[i], "-h") && strcmp(argv[i], "--help");
		}
	}

	if (read_int("max_brightness") < 0) {
		fprintf(stderr, "brightd: no backlight at %s -- is corgi_lcd bound?\n",
			BL_DIR);
		return 1;
	}

	fd_key   = open_input(dev_keys);
	fd_touch = open_input(dev_touch);
	fd_sw    = open_input(dev_switch);

	/* The keyboard is the one node we genuinely cannot work without:
	 * it carries both the hotkeys and most activity. The switch and
	 * touchscreen nodes are optional (the touchscreen driver is a
	 * module and may not be loaded yet). */
	if (fd_key < 0) {
		fprintf(stderr, "brightd: no keyboard input node, giving up\n");
		return 1;
	}

	/* Non-fatal: without it we simply have no X event source and fall
	 * back to the console-only behaviour. */
	fd_fifo = open_fifo(&fd_fifo_w);

	load_config();

	last_activity = now_mono();

	for (;;) {
		struct timeval tv;
		fd_set rfds;
		int activity = 0;
		int ready;

		load_config();

		FD_ZERO(&rfds);
		maxfd = -1;
		if (fd_key >= 0)   { FD_SET(fd_key, &rfds);   if (fd_key > maxfd)   maxfd = fd_key; }
		if (fd_touch >= 0) { FD_SET(fd_touch, &rfds); if (fd_touch > maxfd) maxfd = fd_touch; }
		if (fd_sw >= 0)    { FD_SET(fd_sw, &rfds);    if (fd_sw > maxfd)    maxfd = fd_sw; }
		if (fd_fifo >= 0)  { FD_SET(fd_fifo, &rfds);  if (fd_fifo > maxfd)  maxfd = fd_fifo; }

		tv.tv_sec = TICK_SECS;
		tv.tv_usec = 0;

		ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "brightd: select: %s\n", strerror(errno));
			return 1;
		}

		if (ready > 0) {
			struct input_event ev;
			int fds[3];
			int n;

			fds[0] = fd_key;
			fds[1] = fd_touch;
			fds[2] = fd_sw;

			for (n = 0; n < 3; n++) {
				int fd = fds[n];

				if (fd < 0 || !FD_ISSET(fd, &rfds))
					continue;

				while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
					if (ev.type == EV_SW && ev.code == SW_LID) {
						lid_closed = ev.value ? 1 : 0;
						if (lid_closed) {
							go_blank();
							if (suspend_on_lid)
								do_suspend();
						} else {
							go_active();
							last_activity = now_mono();
						}
						continue;
					}

					if (ev.type != EV_KEY)
						continue;

					/* Track the Fn chord. KEY_F3 is what the
					 * matrix keypad reports for the physical
					 * Fn key (CORGI_KEY_FN in corgi.c); XKB
					 * turns it into ISO_Level3_Shift, but at
					 * this layer it is still F3. */
					if (ev.code == KEY_F3) {
						if (ev.value == 1)
							fn_held = 1;
						else if (ev.value == 0)
							fn_held = 0;
						activity = 1;
						continue;
					}

					/* value 2 is autorepeat -- deliberately
					 * honoured, so holding Fn+4 ramps. */
					if (fn_held && ev.value != 0 &&
					    (ev.code == KEY_3 || ev.code == KEY_4)) {
						/* Waking from dim/blank should not
						 * also consume the keypress: come
						 * back to the saved level first,
						 * then apply the step to it. */
						go_active();
						run_bright(ev.code == KEY_4 ? "up" : "down");
						activity = 1;
						continue;
					}

					activity = 1;
				}
			}

			/* Messages from the X server (see FIFO_PATH). */
			if (fd_fifo >= 0 && FD_ISSET(fd_fifo, &rfds)) {
				char buf[64];
				ssize_t got;

				while ((got = read(fd_fifo, buf, sizeof(buf))) > 0) {
					ssize_t k;

					for (k = 0; k < got; k++) {
						switch (buf[k]) {
						case 'h':
							if (!last_heartbeat)
								say("brightd: X event source connected");
							last_heartbeat = now_mono();
							break;
						case 'a':
							activity = 1;
							break;
						case 'u':
						case 'd':
							go_active();
							run_bright(buf[k] == 'u' ? "up" : "down");
							activity = 1;
							break;
						case 's':
							/* Not activity -- see the
							 * protocol comment above. */
							if (state != ST_BLANKED)
								go_blank();
							break;
						case 'w':
							go_active();
							activity = 1;
							break;
						default:
							/* Unknown opcode: ignore rather
							 * than guess, so the protocol
							 * can grow without breaking
							 * an older daemon. */
							break;
						}
					}
				}
			}
		}

		if (activity) {
			last_activity = now_mono();
			/* The lid being shut outranks stray input: a closed
			 * lid can still rattle the touchscreen. */
			if (!lid_closed)
				go_active();
		}

		if (lid_closed || inhibited())
			continue;

		{
			time_t idle = now_mono() - last_activity;

			/*
			 * Before acting on the idle timer, make sure the timer
			 * means anything: if the keyboard is grabbed we have
			 * not been receiving events, so "idle" is an artefact
			 * of being starved rather than a fact about the user.
			 * Re-probed periodically rather than once at startup
			 * because xsession can start and restart X under us.
			 */
			if ((blank_secs > 0 && idle >= blank_secs) ||
			    (dim_secs > 0 && idle >= dim_secs)) {
				time_t t = now_mono();

				int x_alive = last_heartbeat &&
					      (t - last_heartbeat) < HEARTBEAT_TTL;

				if (t - last_probe >= GRAB_PROBE_SECS) {
					int now_starved = input_is_grabbed(fd_key);

					if (now_starved != starved) {
						starved = now_starved;
						say(starved
						    ? "brightd: keyboard grabbed (X)"
						    : "brightd: keyboard readable");
					}
					last_probe = t;
				}

				/*
				 * Blind means grabbed AND with nobody feeding
				 * us instead. A grab on its own is fine once
				 * the X event source is heartbeating: then the
				 * absence of activity is real idleness rather
				 * than our own deafness, and dimming is
				 * exactly right.
				 */
				if (starved && !x_alive) {
					/* Never hold the panel dark on the
					 * strength of an idle timer we cannot
					 * trust. */
					if (state != ST_ACTIVE)
						go_active();
					continue;
				}
			}

			if (blank_secs > 0 && idle >= blank_secs) {
				if (state != ST_BLANKED)
					go_blank();
			} else if (dim_secs > 0 && idle >= dim_secs) {
				if (state == ST_ACTIVE)
					go_dim();
			}

			/* Independent of the state machine above (not a new
			 * ST_* value): go_blank() already stops this on the
			 * way to ST_BLANKED, and go_active() stops it on any
			 * activity, so this only needs to start it. */
			if (toast_secs > 0 && idle >= toast_secs)
				start_toaster();
		}
	}

	return 0;
}
