#include <fcntl.h>
#include <linux/lirc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DEV		"/dev/lirc0"
#define MAX_EDGES	1024

static int rx(int fd, unsigned int timeout_ms)
{
	unsigned int mode = LIRC_MODE_MODE2;
	unsigned int val;
	ssize_t n;

	if (ioctl(fd, LIRC_SET_REC_MODE, &mode) < 0) {
		perror("ir: this device cannot receive");
		return 1;
	}

	if (timeout_ms) {
		val = timeout_ms * 1000;
		if (ioctl(fd, LIRC_SET_REC_TIMEOUT, &val) < 0)
			fprintf(stderr, "ir: timeout not settable, keeping the driver default\n");
	}

	while ((n = read(fd, &val, sizeof(val))) == (ssize_t)sizeof(val)) {
		switch (val & LIRC_MODE2_MASK) {
		case LIRC_MODE2_PULSE:
			printf("pulse %u\n", val & LIRC_VALUE_MASK);
			break;
		case LIRC_MODE2_SPACE:
			printf("space %u\n", val & LIRC_VALUE_MASK);
			break;
		case LIRC_MODE2_TIMEOUT:
			printf("timeout %u\n\n", val & LIRC_VALUE_MASK);
			break;
		case LIRC_MODE2_FREQUENCY:
			printf("carrier %u\n", val & LIRC_VALUE_MASK);
			break;
		}
		fflush(stdout);
	}

	if (n < 0) {
		perror("ir: read");
		return 1;
	}

	return 0;
}

static unsigned int parse_edges(FILE *in, unsigned int *buf, unsigned int max)
{
	unsigned int count = 0;
	char line[128];

	while (fgets(line, sizeof(line), in)) {
		char *p = line;
		unsigned long us;

		while (*p == ' ' || *p == '\t')
			p++;

		if (!strncmp(p, "timeout", 7)) {
			if (count)
				break;
			continue;
		}
		if (!strncmp(p, "carrier", 7) || *p == '#' || *p == '\n' || *p == '\0')
			continue;
		if (!strncmp(p, "pulse", 5) || !strncmp(p, "space", 5))
			p += 5;

		us = strtoul(p, NULL, 10);
		if (!us)
			continue;

		if (count == max) {
			fprintf(stderr, "ir: over %u edges, keeping the first %u\n",
				max, max);
			break;
		}
		buf[count++] = (unsigned int)us;
	}

	if (count && !(count % 2))
		count--;

	return count;
}

static int tx(int fd, FILE *in, unsigned int carrier, unsigned int duty)
{
	unsigned int mode = LIRC_MODE_PULSE;
	unsigned int buf[MAX_EDGES];
	unsigned int count;

	if (ioctl(fd, LIRC_SET_SEND_MODE, &mode) < 0) {
		perror("ir: this device cannot transmit");
		return 1;
	}
	if (ioctl(fd, LIRC_SET_SEND_CARRIER, &carrier) < 0) {
		perror("ir: LIRC_SET_SEND_CARRIER");
		return 1;
	}
	if (ioctl(fd, LIRC_SET_SEND_DUTY_CYCLE, &duty) < 0)
		fprintf(stderr, "ir: duty cycle not settable, keeping the driver default\n");

	count = parse_edges(in, buf, MAX_EDGES);
	if (!count) {
		fprintf(stderr, "ir: nothing to send\n");
		return 1;
	}

	if (write(fd, buf, count * sizeof(buf[0])) < 0) {
		perror("ir: write");
		return 1;
	}

	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: ir rx [-t ms] [-D dev]\n"
		"       ir tx [-c hz] [-d pct] [-D dev] [file]\n"
		"\n"
		"  ir rx > play.ir     capture until ^C\n"
		"  ir tx play.ir       replay the first frame in it\n");
}

int main(int argc, char **argv)
{
	const char *dev = DEV;
	const char *path = NULL;
	unsigned int carrier = 38000;
	unsigned int duty = 33;
	unsigned int timeout_ms = 0;
	int recv;
	int fd;
	int i;
	int ret;

	if (argc < 2) {
		usage();
		return 2;
	}

	if (!strcmp(argv[1], "rx"))
		recv = 1;
	else if (!strcmp(argv[1], "tx"))
		recv = 0;
	else {
		usage();
		return 2;
	}

	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "-D") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			timeout_ms = (unsigned int)strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			carrier = (unsigned int)strtoul(argv[++i], NULL, 10);
		else if (!strcmp(argv[i], "-d") && i + 1 < argc)
			duty = (unsigned int)strtoul(argv[++i], NULL, 10);
		else if (argv[i][0] != '-' && !recv && !path)
			path = argv[i];
		else {
			usage();
			return 2;
		}
	}

	fd = open(dev, recv ? O_RDONLY : O_WRONLY);
	if (fd < 0) {
		perror(dev);
		fprintf(stderr, "ir: no lirc device -- run  irmode remote\n");
		return 1;
	}

	if (recv) {
		ret = rx(fd, timeout_ms);
	} else {
		FILE *in = stdin;

		if (path) {
			in = fopen(path, "r");
			if (!in) {
				perror(path);
				close(fd);
				return 1;
			}
		}
		ret = tx(fd, in, carrier, duty);
		if (in != stdin)
			fclose(in);
	}

	close(fd);

	return ret;
}
