
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FIFO_PATH   "/tmp/mb-volume.fifo"
#define CONFIG_PATH "/etc/zaurus/volumed"

static void usage(void)
{
	puts("vol -- volume control (talks to the mb-volume panel applet)");
	puts("");
	puts("  vol            show the current level");
	puts("  vol up         one step louder");
	puts("  vol down       one step quieter");
	puts("  vol mute       toggle mute");
	puts("  vol show       pop up the on-screen display only");
	puts("");
	puts("Needs the desktop running: mb-volume owns the mixer.");
}

static int show_level(void)
{
	FILE *f;
	char line[128];
	int volume = -1;
	int muted = 0;

	f = fopen(CONFIG_PATH, "r");
	if (!f) {
		puts("volume: not set yet");
		return 0;
	}

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

		if (!strcmp(key, "volume"))
			volume = atoi(val);
		else if (!strcmp(key, "muted"))
			muted = (!strcmp(val, "yes") || !strcmp(val, "1"));
	}
	fclose(f);

	if (volume < 0) {
		puts("volume: not set yet");
		return 0;
	}

	printf("volume %d%s\n", volume, muted ? " (muted)" : "");
	return 0;
}

static int send_opcode(char op)
{
	int fd;

	fd = open(FIFO_PATH, O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		if (errno == ENOENT || errno == ENXIO)
			fprintf(stderr, "vol: mb-volume is not running\n");
		else
			fprintf(stderr, "vol: %s: %s\n",
				FIFO_PATH, strerror(errno));
		return 1;
	}

	if (write(fd, &op, 1) != 1) {
		fprintf(stderr, "vol: could not send to mb-volume: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}

int main(int argc, char *argv[])
{
	const char *cmd;

	if (argc < 2)
		return show_level();

	cmd = argv[1];

	if (!strcmp(cmd, "up"))
		return send_opcode('u');
	if (!strcmp(cmd, "down"))
		return send_opcode('d');
	if (!strcmp(cmd, "mute"))
		return send_opcode('m');
	if (!strcmp(cmd, "show"))
		return send_opcode('s');
	if (!strcmp(cmd, "help") || !strcmp(cmd, "-h") || !strcmp(cmd, "--help")) {
		usage();
		return 0;
	}

	fprintf(stderr, "vol: unknown command '%s'\n", cmd);
	usage();
	return 1;
}
