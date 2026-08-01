/*
 * flipd -- screen-rotation daemon for the Sharp Zaurus C7x0 swivel hinge.
 *
 * Watches the tablet-mode switch and turns the display around when the
 * screen is swivelled and folded flat over the keyboard, so the desktop
 * stays the right way up in both postures.
 *
 * LANDSCAPE <-> PORTRAIT, AND IT COSTS NOTHING
 * --------------------------------------------
 * The tablet posture is PORTRAIT: a real 480x640 desktop, not a
 * turned-around landscape one. That is what the switch means on this
 * machine, and it is also the cheap direction, which is worth
 * understanding before anyone "optimises" it.
 *
 * The panel is PHYSICALLY 480x640 portrait. The 640x480 landscape desktop
 * this machine normally runs is itself produced by the w100's CRTC
 * rotating during scanout -- graphic_ctrl.portrait_mode = 1, from
 * INIT_MODE_ROTATED in the board file (modules/mach-pxa/corgi_patched.c),
 * programmed by w100_set_dispregs() (modules/w100/w100fb_patched.c). So
 * portrait is not "landscape plus a rotation". It is one transform FEWER:
 *
 *               framebuffer   CRTC portrait_mode   pixel clock divider
 *   landscape    640x480       1  (90 degrees)      6
 *   portrait     480x640       0  (none)            2
 *
 * Switching orientation is therefore just a framebuffer mode change; the
 * CRTC picks up or puts down its rotation for free. No pixel is moved by
 * the CPU either way. Portrait even clocks the panel three times faster,
 * because unrotated scanout reads memory linearly -- which is a good
 * reason to measure the refresh rate in both postures rather than assume
 * portrait is the expensive one.
 *
 * That matters because the obvious implementation is far worse. Xfbdev
 * CAN rotate (`xrandr -o left`): kdrive would set scrpriv->randr and
 * fbdevSetShadow() would swap shadowUpdatePacked for
 * shadowUpdateRotate16_90, turning every damage flush from a per-row
 * memcpy into a reversed per-pixel copy, on a 400MHz PXA255, forever --
 * to reproduce a result the CRTC gives away. Do not "simplify" this
 * daemon into an xrandr call. (There is no xrandr on this rootfs anyway.)
 *
 * WHO ACTUALLY CHANGES THE MODE
 * -----------------------------
 * X does, whenever X is running. The change is a live screen RESIZE, and
 * the server has to tear down its root clip, redo the shadow and
 * page-flip buffers at the new stride, resize the root window and tell
 * every client -- see fbdevSetOrientation() in the xserver fork. Setting
 * the mode behind its back would leave the whole desktop drawing at the
 * wrong size. So this daemon writes PORTRAIT or LANDSCAPE to the server's
 * control FIFO (the channel pikalibrate already uses; see
 * PikalibrateWakeup() in hw/kdrive/linux/linux.c) and lets it do the work.
 *
 * With no X running -- before the graphical session starts, or on a
 * console boot -- there is nothing to coordinate with, so we set the mode
 * ourselves and the console rotates with it.
 *
 * The desktop end of this needs no new code:
 * matchbox-window-manager has handled root-window resizes since 2005 (the
 * ConfigureNotify case in wm.c is commented "screen rotation" and adjusts
 * every client, dialog and docked panel edge by the size delta).
 *
 * THE INPUT HALF
 * --------------
 * The digitiser does not resize. It is bonded to the panel and keeps
 * reporting in the panel's own axes, so when the screen stops being
 * rotated the raw-to-screen mapping has to pick up the 90 degrees the
 * CRTC just put down -- the axes swap. A display that looks right but
 * taps in the wrong place is worse than one that is the wrong way up.
 *
 * The touchscreen is EVIOCGRAB'd by Xfbdev while X runs, so nothing out
 * here can correct it; the transform lives in EvdevPtrAbsolute()
 * (hw/kdrive/linux/evdev.c) and is switched by the same FIFO command that
 * resizes the screen, after the resize, so it is computed against the new
 * dimensions.
 *
 * The 180-degree flip that /usr/sbin/flip drives
 * (/sys/devices/platform/w100fb/flip) is a SEPARATE, orthogonal thing --
 * it turns whichever orientation you are in upside down. This daemon does
 * not touch it.
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
 *   enabled=1        master switch. 0 means never change orientation --
 *                    and never change it back either, so an orientation
 *                    set by hand is left alone.
 *   switch_invert=0  which way round SW_TABLET_MODE reads. 0 means
 *                    "switch reported as set == tablet posture ==
 *                    portrait". See POLARITY below.
 *   portrait_invert=x  read by the X server, not by this daemon: which of
 *                    the two touchscreen axes reverses in portrait. See
 *                    EvdevSetPortrait() in the xserver fork.
 *
 * POLARITY
 * --------
 * SW_TABLET_MODE comes from CORGI_GPIO_SWB via gpio-keys-polled (see
 * corgi_gpio_keys[] in modules/mach-pxa/corgi_patched.c), declared with
 * no .active_low, so the reported value follows the raw GPIO level.
 * Whether that level is high in the swivelled posture or in the clamshell
 * one was NOT verified on hardware when this was written -- the board was
 * not reachable. If the screen goes portrait in the clamshell and
 * landscape in tablet mode, that is the entire bug: set switch_invert=1.
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

#include <linux/fb.h>
#include <linux/input.h>

/* The framebuffer whose geometry IS the orientation. */
#define FB_DEV		"/dev/fb0"

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
 * Current orientation: 1 = portrait, 0 = landscape, -1 = cannot tell.
 *
 * Asked of the framebuffer rather than remembered, so this daemon,
 * /usr/sbin/flip and the X server can never hold three different ideas of
 * which way round the screen is. "Portrait" is simply the taller of the
 * two geometries -- derived, not hardcoded, so it stays true if the mode
 * table ever changes.
 */
static int
read_portrait(void)
{
	struct fb_var_screeninfo var;
	int fd, r;

	fd = open(FB_DEV, O_RDONLY);
	if (fd < 0)
		return -1;
	r = ioctl(fd, FBIOGET_VSCREENINFO, &var);
	close(fd);
	if (r < 0)
		return -1;
	return var.yres > var.xres ? 1 : 0;
}

/*
 * Ask a running X server to change orientation. Returns 1 if the command
 * was delivered, 0 if there is no server listening.
 *
 * X has to be the one to do it whenever it is running: the change is a
 * live screen RESIZE, and the server must tear down its root clip, redo
 * its shadow and page-flip buffers at the new stride, resize the root
 * window and tell every client -- see fbdevSetOrientation() in the
 * xserver fork. Reaching past it to set the mode behind its back would
 * leave the whole desktop drawing at the wrong size.
 *
 * O_NONBLOCK is not optional: opening a FIFO for writing blocks until a
 * reader appears, so without it this would hang forever on a machine with
 * no X. No reader means ENXIO, no FIFO means ENOENT, and both simply mean
 * "X is not running" rather than an error.
 */
static int
tell_x(int portrait)
{
	const char *cmd = portrait ? "PORTRAIT\n" : "LANDSCAPE\n";
	int fd = open(X_CTL_FIFO, O_WRONLY | O_NONBLOCK);

	if (fd < 0)
		return 0;
	if (write(fd, cmd, strlen(cmd)) < 0) {
		say("flipd: %s write failed: %s\n", cmd, strerror(errno));
		close(fd);
		return 0;
	}
	close(fd);
	say("flipd: asked X for %s\n", portrait ? "portrait" : "landscape");
	return 1;
}

/*
 * No X: set the mode ourselves so the console rotates too.
 *
 * Only reached before the graphical session starts, or on a board booted
 * to a console. Deliberately does NOT touch xres_virtual: the fbdev core
 * keeps the existing virtual size if it can, and X re-establishes its own
 * double buffer when it starts.
 */
static int
set_fb_orientation(int portrait)
{
	struct fb_var_screeninfo var;
	unsigned int lo, hi;
	int fd;

	fd = open(FB_DEV, O_RDWR);
	if (fd < 0) {
		say("flipd: %s: %s\n", FB_DEV, strerror(errno));
		return 0;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
		fprintf(stderr, "flipd: FBIOGET_VSCREENINFO: %s\n",
			strerror(errno));
		close(fd);
		return 0;
	}

	lo = var.xres < var.yres ? var.xres : var.yres;
	hi = var.xres < var.yres ? var.yres : var.xres;

	var.xres = portrait ? lo : hi;
	var.yres = portrait ? hi : lo;
	var.xres_virtual = var.xres;
	var.yres_virtual = var.yres;
	var.xoffset = 0;
	var.yoffset = 0;
	var.activate = FB_ACTIVATE_NOW;

	if (ioctl(fd, FBIOPUT_VSCREENINFO, &var) < 0) {
		fprintf(stderr, "flipd: FBIOPUT_VSCREENINFO %ux%u: %s\n",
			var.xres, var.yres, strerror(errno));
		close(fd);
		return 0;
	}
	close(fd);
	say("flipd: console framebuffer -> %ux%u\n", var.xres, var.yres);
	return 1;
}

static void
apply(int tablet)
{
	int want, have;

	load_config();		/* pick up edits without a restart */

	if (!cfg_enabled) {
		say("flipd: disabled by config, ignoring switch=%d\n", tablet);
		return;
	}

	if (cfg_switch_invert)
		tablet = !tablet;

	want = tablet ? 1 : 0;	/* tablet posture == portrait */
	have = read_portrait();

	if (have == want) {
		say("flipd: already %s\n", want ? "portrait" : "landscape");
		return;
	}

	/* X first: if it is up, it owns the framebuffer geometry. */
	if (!tell_x(want))
		set_fb_orientation(want);
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

	if (read_portrait() < 0) {
		fprintf(stderr, "flipd: cannot read %s geometry -- no "
			"framebuffer; giving up\n", FB_DEV);
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
