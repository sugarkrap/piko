#include "ird.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_ird(void)
{
	struct sockaddr_un addr;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", ird_sock_path());

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
		close(fd);
		return -1;
	}

	return fd;
}

const char *ird_sock_path(void)
{
	const char *v = getenv("IRD_SOCK");

	return v && *v ? v : IRD_SOCK_DEFAULT;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: irctl status\n"
		"       irctl off | irda | blaster\n"
		"       irctl discovery [on|off]\n"
		"       irctl watch\n");
}

int main(int argc, char **argv)
{
	char line[IRD_LINE_MAX];
	char buf[IRD_LINE_MAX];
	int watch = 0;
	int fd;
	int i;

	if (argc < 2) {
		usage();
		return 2;
	}

	if (!strcmp(argv[1], "watch")) {
		watch = 1;
		snprintf(line, sizeof(line), "subscribe\n");
	} else {
		size_t used = 0;

		for (i = 1; i < argc; i++)
			used += (size_t)snprintf(line + used, sizeof(line) - used,
						 i > 1 ? " %s" : "%s", argv[i]);
		snprintf(line + used, sizeof(line) - used, "\n");
	}

	fd = connect_ird();
	if (fd < 0) {
		fprintf(stderr, "irctl: no ird on %s -- is it running?\n",
			ird_sock_path());
		return 1;
	}

	if (write(fd, line, strlen(line)) < 0) {
		fprintf(stderr, "irctl: cannot talk to ird\n");
		close(fd);
		return 1;
	}

	while (1) {
		ssize_t got = read(fd, buf, sizeof(buf) - 1);

		if (got <= 0)
			break;

		buf[got] = '\0';
		fputs(buf, stdout);
		fflush(stdout);

		if (!watch)
			break;
	}

	close(fd);

	return 0;
}
