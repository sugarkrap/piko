
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

#define INHIBIT    "/tmp/brightd.inhibit"

#define CONFIG_PATH "/etc/zaurus/power-management.cfg"

#define FIFO_PATH  "/tmp/brightd.fifo"

#define OSD_FIFO   "/tmp/mb-brightness.fifo"

#define HEARTBEAT_TTL 30

#define DEF_DIM_SECS    60
#define DEF_TOAST_SECS  120
#define DEF_BLANK_SECS  300
#define DEF_DIM_LEVEL   5

#define TICK_SECS       2

#define GRAB_PROBE_SECS 30

static const char *dev_switch = "/dev/input/event0";
static const char *dev_keys   = "/dev/input/event1";
static const char *dev_touch  = "/dev/input/event2";

static int dim_secs   = DEF_DIM_SECS;
static int toast_secs = DEF_TOAST_SECS;
static int blank_secs = DEF_BLANK_SECS;
static int dim_level  = DEF_DIM_LEVEL;
static int verbose    = 0;
static int suspend_on_lid = 0;
static int toast_battery_deadzone = 1;

static int dim_secs_cli = 0, blank_secs_cli = 0, dim_level_cli = 0,
	toast_secs_cli = 0;

static pid_t toaster_pid = -1;

static time_t cfg_mtime = 0;

enum { ST_ACTIVE, ST_DIMMED, ST_BLANKED };
static int state = ST_ACTIVE;

static int saved_level = -1;

static int starved = 0;
static time_t last_probe = 0;

static time_t last_heartbeat = 0;

static time_t last_activity = 0;

static time_t last_resume = 0;

#define RESUME_GUARD_SECS 2

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
	puts("The on/off button suspends; pressing it again wakes the device.");
	puts("Create /tmp/brightd.inhibit to suspend dimming (e.g. video).");
	puts("");
	puts("/etc/zaurus/power-management.cfg overrides dim_secs/blank_secs/");
	puts("dim_level above and adds suspend_on_lid=yes and");
	puts("toast_battery_deadzone=no; re-read live on change.");
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

static void
load_config(void)
{
	struct stat st;
	FILE *f;
	char line[256];

	if (stat(CONFIG_PATH, &st) < 0)
		return;
	if (cfg_mtime && st.st_mtime == cfg_mtime)
		return;

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
		} else if (!strcmp(key, "toast_battery_deadzone")) {
			toast_battery_deadzone =
				!(!strcmp(val, "no") || !strcmp(val, "0"));
		}
	}
	fclose(f);

	cfg_mtime = st.st_mtime;
	say("brightd: loaded " CONFIG_PATH);
}

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
		if (toast_battery_deadzone)
			execl(TOASTERS_BIN, "toasters", (char *)NULL);
		else
			execl(TOASTERS_BIN, "toasters", "-B", (char *)NULL);
		_exit(127);
	}
	say("brightd: toasters started");
}

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

	if (state == ST_ACTIVE) {
		int cur = current_level();
		if (cur >= 0)
			saved_level = cur;
	}

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

static void
do_suspend(void)
{
	int fd;

	fd = open("/sys/power/mem_sleep", O_WRONLY);
	if (fd >= 0) {
		if (write(fd, "deep", 4) != 4)
			fprintf(stderr, "brightd: could not select deep sleep: %s\n",
				strerror(errno));
		close(fd);
	}

	fd = open("/sys/power/state", O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "brightd: cannot open /sys/power/state: %s\n",
			strerror(errno));
		return;
	}

	fprintf(stderr, "brightd: suspending (echo mem > /sys/power/state)\n");
	if (write(fd, "mem", 3) != 3)
		fprintf(stderr, "brightd: suspend failed: %s\n", strerror(errno));
	else
		fprintf(stderr, "brightd: back from suspend\n");
	close(fd);

	say("brightd: resumed");
	last_resume = now_mono();
	go_active();
	last_activity = now_mono();
}

static void
request_suspend(void)
{
	if (last_resume && (now_mono() - last_resume) < RESUME_GUARD_SECS) {
		say("brightd: ignoring on/off, just resumed");
		return;
	}

	do_suspend();
}

static void
notify_osd(void)
{
	int fd = open(OSD_FIFO, O_WRONLY | O_NONBLOCK);

	if (fd < 0)
		return;

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

	notify_osd();
}

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

	if (fd_key < 0) {
		fprintf(stderr, "brightd: no keyboard input node, giving up\n");
		return 1;
	}

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

					if (ev.code == KEY_SUSPEND) {
						if (ev.value == 1)
							request_suspend();
						continue;
					}

					if (ev.code == KEY_F3) {
						if (ev.value == 1)
							fn_held = 1;
						else if (ev.value == 0)
							fn_held = 0;
						activity = 1;
						continue;
					}

					if (fn_held && ev.value != 0 &&
					    (ev.code == KEY_3 || ev.code == KEY_4)) {
						go_active();
						run_bright(ev.code == KEY_4 ? "up" : "down");
						activity = 1;
						continue;
					}

					activity = 1;
				}
			}

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
							if (state != ST_BLANKED &&
							    !inhibited())
								go_blank();
							break;
						case 'z':
							request_suspend();
							break;
						case 'w':
							go_active();
							activity = 1;
							break;
						default:
							break;
						}
					}
				}
			}
		}

		if (activity) {
			last_activity = now_mono();
			if (!lid_closed)
				go_active();
		}

		if (lid_closed || inhibited())
			continue;

		{
			time_t idle = now_mono() - last_activity;

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

				if (starved && !x_alive) {
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

			if (toast_secs > 0 && idle >= toast_secs)
				start_toaster();
		}
	}

	return 0;
}
