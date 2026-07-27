/*
 * zplay - minimal WAV file player for the Zaurus's OSS /dev/dsp.
 *
 * Deliberately tiny (no libmad/mpg123/etc dependency chain) since this
 * device is 64MB RAM / 400MHz XScale with no hardware audio decode --
 * playing raw PCM straight to /dev/dsp is the lightest possible path.
 *
 * Usage: zplay file.wav
 *
 * Parses just enough of the RIFF/WAVE header (fmt + data chunks) to set
 * the OSS format/channels/rate via ioctl, then streams the PCM payload
 * straight to /dev/dsp in fixed-size chunks.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

struct wav_fmt {
	uint16_t audio_format;
	uint16_t channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits_per_sample;
};

static int read_exact(int fd, void *buf, size_t len)
{
	char *p = buf;
	while (len) {
		ssize_t n = read(fd, p, len);
		if (n <= 0)
			return -1;
		p += n;
		len -= n;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s file.wav\n", argv[0]);
		return 1;
	}

	int wf = open(argv[1], O_RDONLY);
	if (wf < 0) {
		perror("open input");
		return 1;
	}

	char riff[12];
	if (read_exact(wf, riff, 12) || memcmp(riff, "RIFF", 4) ||
	    memcmp(riff + 8, "WAVE", 4)) {
		fprintf(stderr, "not a RIFF/WAVE file\n");
		return 1;
	}

	struct wav_fmt fmt;
	int have_fmt = 0;
	uint32_t data_len = 0;

	for (;;) {
		char id[4];
		uint32_t len;
		if (read_exact(wf, id, 4) || read_exact(wf, &len, 4))
			break;

		if (!memcmp(id, "fmt ", 4)) {
			char buf[64];
			if (len > sizeof(buf) || read_exact(wf, buf, len))
				break;
			memcpy(&fmt, buf, sizeof(fmt));
			have_fmt = 1;
			if (len & 1)
				lseek(wf, 1, SEEK_CUR);
		} else if (!memcmp(id, "data", 4)) {
			data_len = len;
			break; /* PCM payload starts right here */
		} else {
			lseek(wf, len + (len & 1), SEEK_CUR);
		}
	}

	if (!have_fmt || !data_len) {
		fprintf(stderr, "no fmt/data chunk found\n");
		return 1;
	}

	int dsp = open("/dev/dsp", O_WRONLY);
	if (dsp < 0) {
		perror("open /dev/dsp");
		return 1;
	}

	int afmt = (fmt.bits_per_sample == 8) ? AFMT_U8 : AFMT_S16_LE;
	int channels = fmt.channels;
	int rate = fmt.sample_rate;

	if (ioctl(dsp, SNDCTL_DSP_SETFMT, &afmt) < 0)
		perror("SNDCTL_DSP_SETFMT");
	if (ioctl(dsp, SNDCTL_DSP_CHANNELS, &channels) < 0)
		perror("SNDCTL_DSP_CHANNELS");
	if (ioctl(dsp, SNDCTL_DSP_SPEED, &rate) < 0)
		perror("SNDCTL_DSP_SPEED");

	printf("playing %s: %u Hz, %u ch, %u bit, %u bytes\n",
	       argv[1], fmt.sample_rate, fmt.channels, fmt.bits_per_sample,
	       data_len);

	char buf[4096];
	uint32_t remaining = data_len;
	while (remaining) {
		size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
		ssize_t n = read(wf, buf, want);
		if (n <= 0)
			break;
		if (write(dsp, buf, n) != n) {
			perror("write /dev/dsp");
			break;
		}
		remaining -= n;
	}

	close(dsp);
	close(wf);
	return 0;
}
