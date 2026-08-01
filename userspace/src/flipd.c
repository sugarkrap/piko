/*
 * flipd -- screen-rotation daemon for the Sharp Zaurus C7x0 swivel hinge.
 *
 * Watches the tablet-mode switch and turns the display around when the
 * screen is swivelled and folded flat over the keyboard, so the desktop
 * stays the right way up in both postures.
 *
 * THE ROTATION IS FREE. THIS DAEMON DOES NOT MOVE PIXELS.
 * ------------------------------------------------------
 * The w100's CRTC rotates during scanout -- graphic_ctrl.portrait_mode in
 * w100_set_dispregs() (modules/w100/w100fb_patched.c) -- and this board
 * already relies on it: the panel is physically 480x640 portrait, and the
 * 640x480 landscape framebuffer everything draws into only becomes
 * landscape because the platform data asks for INIT_MODE_ROTATED, i.e.
 * portrait_mode=1 (90 degrees). So rotation on this machine is not a
 * feature to be added, it is a register field that is already in use.
 *
 * w100fb exposes the other half of that field as a sysfs file:
 *
 *   /sys/devices/platform/w100fb/flip
 *
 * With the framebuffer in its rotated (landscape) geometry, flip=0 gives
 * portrait_mode=1 (90 degrees) and flip=1 gives portrait_mode=2 (270) --
 * exactly 180 degrees apart, which is exactly the transform the swivel
 * needs. Writing that file costs one register write plus a mode-register
 * refresh. Nothing re-renders, no pixel is touched, no memory is
 * allocated, and the cost does not depend on what is on screen.
 *
 * That is worth stating plainly because the obvious alternative is far
 * worse. Xfbdev CAN rotate (`xrandr -o inverted`): kdrive would set
 * scrpriv->randr and swap shadowUpdatePacked for shadowUpdateRotate16_180
 * in fbdevSetShadow() (hw/kdrive/fbdev/fbdev.c). That turns every damage
 * flush from a per-row memcpy into a reversed per-pixel copy, on a
 * 400MHz PXA255 whose panel already only manages ~26Hz, forever, in
 * exchange for a result the CRTC gives away for nothing. Do not "simplify"
 * this daemon into an xrandr call.
 *
 * WHY 180 AND NOT 90/270
 * ----------------------
 * The C7x0 lid swivels about its own vertical axis and folds back down,
 * so in tablet posture the panel is the same landscape rectangle in the
 * same place, upside down. 180 degrees is the whole correction.
 *
 * True portrait (a 480x640 desktop) is a different operation: it needs
 * FBIOPUT_VSCREENINFO with xres/yres swapped, which changes the screen
 * dimensions under a running X server -- and kdrive cannot resize a live
 * screen, so it would mean restarting X and losing the session. Not done
 * here. See docs/HOWTO-SCREEN-ROTATION.md.
 *
 * THE INPUT HALF
 * --------------
 * Turning the panel around without turning the touchscreen around leaves
 * a display that looks right and taps 180 degrees off, which is worse
 * than not rotating at all. The touchscreen is EVIOCGRAB'd by Xfbdev
 * while X runs, so the correction has to happen inside the server: our
 * xserver fork inverts absolute pointer coordinates in EvdevPtrAbsolute()
 * (hw/kdrive/linux/evdev.c) when the panel is flipped.
 *
 * X reads the flip state from the same sysfs file this daemon writes --
 * there is deliberately no second copy of the state to get out of sync.
 * This daemon only has to tell X *when* to look, which it does by writing
 * "ROTATE" to the server's control FIFO (the channel pikalibrate already
 * uses; see PikalibrateWakeup() in hw/kdrive/linux/linux.c). If X is not
 * running there is nothing to notify and the write is skipped; X re-reads
 * the file when it starts and after every resume, so a boot or a wake in
 * tablet posture comes up correct without being told.
 *
 * WHY A SEPARATE DAEMON AND NOT PART OF brightd
 * ---------------------------------------------
 * brightd already reads this same evdev node for SW_LID, so folding this
 * in would have saved a process. It is separate because the two have
 * nothing to do with each other and share no state: rotation must keep
 * working while brightd is stopped or inhibited, and brightd's policy
 * (idle timers, suspend-on-lid) is intricate enough without a display
 * concern in it. Two readers of one evdev node is not a conflict --
 * every open gets its own event queue; only EVIOCGRAB is exclusive, and
 * neither of us grabs.
 *
 * CONFIGURATION
 * -------------
 * /etc/piko/rotation.cfg, "key=value" per line, '#' comments, same
 * format brightd uses. Re-read on every switch event, so an edit takes
 * effect on the next swivel with no restart and no signal (this rootfs's
 * BusyBox has no kill; see NO SIGNAL PROTOCOL in brightd.c).
 *
 *   enabled=1        master switch. 0 means never touch the display --
 *                    and never *un*-touch it either, so a flip set by
 *                    hand with /usr/sbin/flip is left alone.
 *   switch_invert=0  which way round SW_TABLET_MODE reads. 0 means
 *                    "switch reported as set == tablet posture ==
 *                    flipped". See POLARITY below.
 *
 * POLARITY
 * --------
 * SW_TABLET_MODE comes from CORGI_GPIO_SWB via gpio-keys-polled (see
 * corgi_gpio_keys[] in modules/mach-pxa/corgi_patched.c), declared with
 * no .active_low, so the reported value follows the raw GPIO level.
 * Whether that level is high in the swivelled posture or in the clamshell
 * one was NOT verified on hardware when this was written -- the board was
 * not reachable. If the screen turns upside down in the clamshell and
 * correct in tablet mode, that is the entire bug: set switch_invert=1.
 * It is a config line rather than a rebuild precisely because it is the
 * one thing here that could not be checked.
 *
 * Started from /etc/init.d/rcS with '&'. Does not fork or daemonize (rcS
 * backgrounds it), and blocks in read() the rest of the time -- the
 * switch is polled by the kernel at gpio-keys-polled's 250ms interval and
 * only reports on change, so an idle machine pays nothing for this.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/input.h>

/* The w100 CRTC's other 180 degrees. See the header comment. */
#define FLIP_SYSFS	"/sys/devices/platform/w100fb/flip"

/* Xfbdev's control FIFO (hw/kdrive/linux/linux.c). Created by the server;
 * absent, or present with nobody reading, when X is not running. */
#define X_CTL_FIFO	"/tmp/.pikalibrate-ctl"

#define CONFIG_PATH	"/etc/piko/rotation.cfg"

#define BITS_PER_LONG_	(sizeof(long) * 8)
#define NBITS(x)	((((x) - 1) / BITS_PER_LONG_) + 1)
#define ISBITSET(a, b)	((a)[(b) / BITS_PER_LONG_] & (1UL << ((b) % BITS_PER_LONG_)))

static int cfg_enabled = 1;
static int cfg_switch_invert;

static int verbose;

static void
say(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void
load_config(void)
{
	FILE *f;
	char line[256];

	f = fopen(CONFIG_PATH, "r");
	if (!f)
		return;		/* no file: compiled-in defaults stand */

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

		if (!strcmp(key, "enabled"))
			cfg_enabled = !strcmp(val, "yes") || atoi(val) != 0;
		else if (!strcmp(key, "switch_invert"))
			cfg_switch_invert = !strcmp(val, "yes") || atoi(val) != 0;
	}
	fclose(f);
}

/*
 * Current CRTC flip state, or -1 if it cannot be read (no w100fb, or a
 * kernel without the sysfs attribute). Read rather than remembered so
 * this daemon and /usr/sbin/flip can both drive the same register
 * without either one holding a stale idea of it.
 */
static int
read_flip(void)
{
	char buf[8];
	int fd, n;

	fd = open(FLIP_SYSFS, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return atoi(buf) ? 1 : 0;
}

/*
 * Writing this file makes w100fb reprogram the graphics controller and
 * reset the scanout base to the front buffer (w100_set_dispregs() ends
 * with a write to mmGRAPHIC_OFFSET). With the fork's double-buffered
 * page flipping running that can cost one stale frame, until X's next
 * paint pans the scanout back where it belongs -- so it matters that we
 * never write a value that is already set, both here and on startup.
 */
static void
write_flip(int want)
{
	char c = want ? '1' : '0';
	int fd;

	if (read_flip() == want)
		return;

	fd = open(FLIP_SYSFS, O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "flipd: cannot open %s: %s\n",
			FLIP_SYSFS, strerror(errno));
		return;
	}
	if (write(fd, &c, 1) != 1)
		fprintf(stderr, "flipd: cannot write %s: %s\n",
			FLIP_SYSFS, strerror(errno));
	close(fd);
	say("flipd: display flip -> %d\n", want);
}

/*
 * Ask a running X server to re-read the flip state and re-aim the
 * touchscreen. O_NONBLOCK is not optional: opening a FIFO for writing
 * blocks until a reader appears, so without it this would hang forever
 * on a machine with no X. No reader means ENXIO, no FIFO means ENOENT,
 * and both are the normal console-only case rather than errors.
 */
static void
notify_x(void)
{
	int fd = open(X_CTL_FIFO, O_WRONLY | O_NONBLOCK);

	if (fd < 0)
		return;
	if (write(fd, "ROTATE\n", 7) < 0)
		say("flipd: ROTATE write failed: %s\n", strerror(errno));
	else
		say("flipd: told X to re-read the flip state\n");
	close(fd);
}

static void
apply(int tablet)
{
	load_config();		/* pick up edits without a restart */

	if (!cfg_enabled) {
		say("flipd: disabled by config, ignoring switch=%d\n", tablet);
		return;
	}

	if (cfg_switch_invert)
		tablet = !tablet;

	if (read_flip() == tablet)
		return;		/* already there; do not disturb X */

	write_flip(tablet);
	notify_x();
}

/*
 * Find the evdev node carrying SW_TABLET_MODE rather than hardcoding
 * event0. It IS event0 today (gpio-keys-polled probes before the SPI
 * touchscreen and the matrix keypad), but that ordering is a property of
 * driver probe order, not of anything we control, and getting it wrong
 * here fails silently -- we would simply sit on a device that never
 * reports the switch. Asking the kernel which node has the bit costs one
 * ioctl per node, once, at startup.
 *
 * The board has three event nodes, so scanning 32 is pure slack; it is
 * that wide because the same scan is what the host-side test harness
 * exercises, and a desktop running this under uinput lands well past
 * event15. Missing nodes just fail to open and cost nothing.
 */
static int
open_switch_device(void)
{
	unsigned long evbits[NBITS(EV_MAX)];
	unsigned long swbits[NBITS(SW_MAX)];
	char path[32];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;

		memset(evbits, 0, sizeof(evbits));
		memset(swbits, 0, sizeof(swbits));
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) >= 0 &&
		    ISBITSET(evbits, EV_SW) &&
		    ioctl(fd, EVIOCGBIT(EV_SW, sizeof(swbits)), swbits) >= 0 &&
		    ISBITSET(swbits, SW_TABLET_MODE)) {
			say("flipd: watching %s\n", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

/*
 * The switch is a state, not an edge: the kernel reports it when it
 * changes, and a machine booted (or a daemon restarted) with the screen
 * already swivelled would otherwise sit wrong-way-up until the user
 * swivelled it twice. EVIOCGSW asks for the current state directly.
 */
static int
read_switch_state(int fd)
{
	unsigned long sw[NBITS(SW_MAX)];

	memset(sw, 0, sizeof(sw));
	if (ioctl(fd, EVIOCGSW(sizeof(sw)), sw) < 0)
		return -1;
	return ISBITSET(sw, SW_TABLET_MODE) ? 1 : 0;
}

int
main(int argc, char **argv)
{
	int fd, state, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v")) {
			verbose = 1;
		} else {
			fprintf(stderr, "usage: flipd [-v]\n");
			return 2;
		}
	}

	/* A FIFO whose reader went away must not kill us. */
	signal(SIGPIPE, SIG_IGN);

	load_config();

	fd = open_switch_device();
	if (fd < 0) {
		fprintf(stderr, "flipd: no input device reports "
			"SW_TABLET_MODE, giving up\n");
		return 1;
	}

	if (read_flip() < 0) {
		fprintf(stderr, "flipd: %s missing -- no w100fb, or a kernel "
			"without the flip attribute; giving up\n", FLIP_SYSFS);
		close(fd);
		return 1;
	}

	state = read_switch_state(fd);
	if (state >= 0)
		apply(state);

	for (;;) {
		struct input_event ev;
		ssize_t n = read(fd, &ev, sizeof(ev));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "flipd: read failed: %s\n",
				strerror(errno));
			break;
		}
		if (n != sizeof(ev))
			continue;	/* short read: not a whole event */

		if (ev.type == EV_SW && ev.code == SW_TABLET_MODE)
			apply(ev.value ? 1 : 0);
	}

	close(fd);
	return 1;
}
