/*
 * vol -- one-word volume control for the Sharp Zaurus C7x0.
 *
 *   vol            show the current level
 *   vol up         one step louder
 *   vol down       one step quieter
 *   vol mute       toggle mute
 *   vol show       just pop the OSD up, change nothing
 *
 * Typable on the device keyboard: no '/' or ':' anywhere in the commands
 * you have to enter, same reason bright/wifiup/audioon/netinfo exist (see
 * AGENTS.md, "The device keyboard cannot type many characters").
 *
 * WHAT THIS ACTUALLY DOES
 * -----------------------
 * Nothing, by itself. It is a one-byte poke at mb-volume's control FIFO;
 * mb-volume owns the ALSA mixer, the /etc/zaurus/volumed config file and
 * the on-screen display, and this just asks it to do something. That
 * split is deliberate and is the same one bright/brightd already use:
 * exactly one process may own a piece of hardware state, or two of them
 * race and the persisted config ends up describing neither.
 *
 * So this is a no-op when the desktop is not running. That is the honest
 * behaviour -- there is no "adjust ALSA behind mb-volume's back" mode,
 * because that would leave the applet's slider, the OSD and the config
 * file all disagreeing with the mixer until the next restart.
 *
 * WHY C AND NOT A SHELL SCRIPT
 * ----------------------------
 * Because "echo u > /tmp/mb-volume.fifo" BLOCKS FOREVER when mb-volume is
 * not running: opening a FIFO for writing waits for a reader, and this
 * device's /tmp is on the root jffs2 rather than a tmpfs, so a FIFO left
 * behind by a previous session is still sitting there after a reboot
 * looking exactly like a live one. A shell that hits that is stuck with
 * no ^C on the framebuffer console and no kill/killall/pkill applet in
 * this busybox to rescue it with.
 *
 * O_NONBLOCK is the fix: on a FIFO it makes an O_WRONLY open with no
 * reader fail immediately with ENXIO instead of waiting, which turns the
 * hang into the "is mb-volume running?" message below. There is no way to
 * ask for that flag from busybox ash, and this busybox has no timeout
 * applet to bound the wait from outside either -- hence a real program.
 * The X server's evdev media-key path (hw/kdrive/linux/evdev.c in the
 * xserver fork) opens the same FIFO the same way, for the same reason.
 *
 * Copyright 2026 the piko project. GPL v2 or later.
 */

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

/*
 * Reports what mb-volume last persisted, not what ALSA currently says.
 * Reading the mixer directly would mean linking libasound into a tool
 * whose whole job is to avoid touching the mixer, and the config file is
 * written on every committed change anyway (see PERSISTENCE in
 * mb-volume.c), so it is the same number.
 */
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

	/* No printf applet on this device's shell is why the callers are
	 * scripts, but we are a real program -- still, keep the output to
	 * one short line that reads fine in a photo of the screen. */
	printf("volume %d%s\n", volume, muted ? " (muted)" : "");
	return 0;
}

static int send_opcode(char op)
{
	int fd;

	/* O_NONBLOCK is load bearing -- see the header comment. */
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
