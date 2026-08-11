
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

#define FB_DEV		"/dev/fb0"

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

		if (!strcmp(key, "enabled"))
			cfg_enabled = !strcmp(val, "yes") || atoi(val) != 0;
		else if (!strcmp(key, "switch_invert"))
			cfg_switch_invert = !strcmp(val, "yes") || atoi(val) != 0;
	}
	fclose(f);
}

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

	load_config();

	if (!cfg_enabled) {
		say("flipd: disabled by config, ignoring switch=%d\n", tablet);
		return;
	}

	if (cfg_switch_invert)
		tablet = !tablet;

	want = tablet ? 1 : 0;
	have = read_portrait();

	if (have == want) {
		say("flipd: already %s\n", want ? "portrait" : "landscape");
		return;
	}

	if (!tell_x(want))
		set_fb_orientation(want);
}

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
			continue;

		if (ev.type == EV_SW && ev.code == SW_TABLET_MODE)
			apply(ev.value ? 1 : 0);
	}

	close(fd);
	return 1;
}
