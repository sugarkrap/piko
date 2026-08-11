
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CPUFREQ_DIR	"/sys/devices/system/cpu/cpu0/cpufreq"
#define PARAM_MAXFREQ	"/sys/module/pxa2xx_cpufreq/parameters/pxa255_maxfreq"
#define MODNAME		"pxa2xx-cpufreq"

#define STOCK_MHZ	398

#define FORCE_ABOVE_MHZ	471

static const struct {
	unsigned int khz;
	unsigned int core_mhz;
	unsigned int mem_mhz;
} steps[] = {
	{  99533,  99,  99 },
	{ 199066, 199,  99 },
	{ 298599, 298,  99 },
	{ 398132, 398,  99 },
	{ 471860, 471, 118 },
	{ 530842, 530, 133 },
	{ 589824, 589, 147 },
	{ 663552, 663, 166 },
};

static const char *prog = "mhz";

static void die(const char *what)
{
	fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(errno));
	exit(1);
}

static int read_file(const char *path, char *buf, size_t len)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		n--;
	buf[n] = '\0';
	return 0;
}

static int write_file(const char *path, const char *val)
{
	ssize_t n;
	int fd;

	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	n = write(fd, val, strlen(val));
	close(fd);
	return n < 0 ? -1 : 0;
}

static int read_uint(const char *path, unsigned int *out)
{
	char buf[64];

	if (read_file(path, buf, sizeof(buf)) < 0)
		return -1;
	*out = (unsigned int)strtoul(buf, NULL, 10);
	return 0;
}

static const char *cf(const char *leaf)
{
	static char path[256];

	snprintf(path, sizeof(path), CPUFREQ_DIR "/%s", leaf);
	return path;
}

static int run(const char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int module_loaded(void)
{
	return access(CPUFREQ_DIR, F_OK) == 0;
}

static unsigned int current_ceiling(void)
{
	unsigned int v;

	if (read_uint(PARAM_MAXFREQ, &v) < 0)
		return 0;
	return v ? v : STOCK_MHZ;
}

static int ensure_ceiling(unsigned int want)
{
	static const char *rmmod_argv[] = { "rmmod", MODNAME, NULL };
	const char *modprobe_argv[4];
	char param[64];

	if (module_loaded() && current_ceiling() == want)
		return 0;

	if (module_loaded() && run(rmmod_argv) != 0) {
		fprintf(stderr,
			"%s: could not unload " MODNAME " -- is a governor still holding it?\n",
			prog);
		return -1;
	}

	snprintf(param, sizeof(param), "pxa255_maxfreq=%u", want);
	modprobe_argv[0] = "modprobe";
	modprobe_argv[1] = MODNAME;
	modprobe_argv[2] = param;
	modprobe_argv[3] = NULL;

	if (run(modprobe_argv) != 0 || !module_loaded()) {
		fprintf(stderr,
			"%s: could not load " MODNAME " (%s)\n", prog, param);
		return -1;
	}
	return 0;
}

static unsigned int mem_mhz_for(unsigned int khz)
{
	size_t i;

	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
		if (steps[i].core_mhz == khz / 1000)
			return steps[i].mem_mhz;
	return 0;
}

static void show(void)
{
	char avail[512], gov[64];
	unsigned int cur = 0, ceiling;
	char *tok, *save;

	if (!module_loaded()) {
		printf("cpufreq is not loaded -- the core is at whatever the\n"
		       "bootloader left it at (398 MHz on a stock boot).\n"
		       "Run  mhz auto  or  mhz 398  to take control of it.\n");
		return;
	}

	ceiling = current_ceiling();

	if (read_uint(cf("scaling_cur_freq"), &cur) < 0)
		read_uint(cf("cpuinfo_cur_freq"), &cur);
	if (read_file(cf("scaling_governor"), gov, sizeof(gov)) < 0)
		strcpy(gov, "?");

	printf("core      %u MHz", cur / 1000);
	if (mem_mhz_for(cur))
		printf("   (memory %u MHz)", mem_mhz_for(cur));
	printf("\n");
	printf("governor  %s\n", gov);
	if (ceiling)
		printf("ceiling   %u MHz%s\n", ceiling,
		       ceiling > STOCK_MHZ ? "   OVERCLOCKED" : "");
	else
		printf("ceiling   unknown (" MODNAME " is not the driver in "
		       "charge)\n");

	if (read_file(cf("scaling_available_frequencies"), avail,
		      sizeof(avail)) == 0) {
		printf("steps    ");
		for (tok = strtok_r(avail, " ", &save); tok;
		     tok = strtok_r(NULL, " ", &save)) {
			unsigned int khz = (unsigned int)strtoul(tok, NULL, 10);

			if (khz)
				printf(" %u", khz / 1000);
		}
		printf("\n");
	}
}

static void usage(void)
{
	printf("usage:\n"
	       "  mhz              show speed, governor and available steps\n"
	       "  mhz auto         scale on demand over the permitted range\n"
	       "  mhz <n>          pin the core to step <n> MHz\n"
	       "  mhz <n> force    required above %u MHz\n"
	       "  mhz stock        back to the rated %u MHz, on demand\n"
	       "\n"
	       "overclock steps run the SDRAM and the static memory bus at the\n"
	       "same ratio as the core -- 471 runs memory at 118 MHz, and the\n"
	       "steps above that are well past its rating. nothing persists: a\n"
	       "reboot always comes back at %u MHz.\n",
	       FORCE_ABOVE_MHZ, STOCK_MHZ, STOCK_MHZ);
}

static int set_fixed(unsigned int mhz, int forced)
{
	unsigned int got = 0;
	char khz[32];
	size_t i;

	for (i = 0; i < sizeof(steps) / sizeof(steps[0]); i++)
		if (steps[i].core_mhz == mhz)
			break;
	if (i == sizeof(steps) / sizeof(steps[0])) {
		fprintf(stderr,
			"%s: %u MHz is not a step this part can produce.\n"
			"    try one of: 99 199 298 398 471 530 589 663\n",
			prog, mhz);
		return 1;
	}

	if (mhz > FORCE_ABOVE_MHZ && !forced) {
		fprintf(stderr,
			"%s: %u MHz runs the SDRAM at %u MHz, well over its\n"
			"    100 MHz rating. If you mean it:  mhz %u force\n",
			prog, mhz, steps[i].mem_mhz, mhz);
		return 1;
	}

	if (ensure_ceiling(mhz > STOCK_MHZ ? mhz : STOCK_MHZ) < 0)
		return 1;

	if (write_file(cf("scaling_governor"), "userspace") < 0)
		die("setting the userspace governor");

	snprintf(khz, sizeof(khz), "%u", steps[i].khz);
	if (write_file(cf("scaling_setspeed"), khz) < 0)
		die("setting the speed");

	if (read_uint(cf("scaling_cur_freq"), &got) == 0)
		printf("core %u MHz (memory %u MHz)\n", got / 1000,
		       steps[i].mem_mhz);
	return 0;
}

static int set_auto(unsigned int ceiling)
{
	if (ensure_ceiling(ceiling) < 0)
		return 1;
	if (write_file(cf("scaling_governor"), "ondemand") < 0)
		die("setting the ondemand governor");
	printf("scaling on demand, up to %u MHz\n", ceiling);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned int mhz;
	int forced;
	char *end;

	if (argc == 1) {
		show();
		return 0;
	}

	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "help")) {
		usage();
		return 0;
	}

	if (geteuid() != 0) {
		fprintf(stderr, "%s: must be root to change the clock\n", prog);
		return 1;
	}

	if (!strcmp(argv[1], "stock"))
		return set_auto(STOCK_MHZ);

	if (!strcmp(argv[1], "auto")) {
		return set_auto(module_loaded() ? current_ceiling()
					        : STOCK_MHZ);
	}

	if (!isdigit((unsigned char)argv[1][0])) {
		usage();
		return 1;
	}

	mhz = (unsigned int)strtoul(argv[1], &end, 10);
	if (*end) {
		usage();
		return 1;
	}

	forced = argc > 2 && !strcmp(argv[2], "force");

	return set_fixed(mhz, forced);
}
