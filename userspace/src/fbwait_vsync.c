#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static long delta_us(const struct timespec *a, const struct timespec *b)
{
    long sec = b->tv_sec - a->tv_sec;
    long nsec = b->tv_nsec - a->tv_nsec;
    return sec * 1000000L + nsec / 1000L;
}

int main(int argc, char **argv)
{
    const char *fb = "/dev/fb0";
    int loops = 120;
    int arg = 0;
    int fd;
    int i;
    int ok = 0;
    int fail = 0;
    long min_us = 0;
    long max_us = 0;
    long sum_us = 0;
    struct timespec t0, t1;

    if (argc > 1)
        loops = atoi(argv[1]);
    if (loops < 2)
        loops = 2;

    fd = open(fb, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", fb, strerror(errno));
        return 1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &t0) != 0) {
        fprintf(stderr, "clock_gettime start failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    for (i = 0; i < loops; i++) {
        struct timespec a, b;
        long us;

        if (clock_gettime(CLOCK_MONOTONIC, &a) != 0) {
            fprintf(stderr, "clock_gettime loop failed: %s\n", strerror(errno));
            close(fd);
            return 1;
        }

        if (ioctl(fd, FBIO_WAITFORVSYNC, &arg) != 0) {
            fail++;
            continue;
        }

        if (clock_gettime(CLOCK_MONOTONIC, &b) != 0) {
            fprintf(stderr, "clock_gettime loop end failed: %s\n", strerror(errno));
            close(fd);
            return 1;
        }

        us = delta_us(&a, &b);
        if (ok == 0 || us < min_us)
            min_us = us;
        if (ok == 0 || us > max_us)
            max_us = us;
        sum_us += us;
        ok++;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0) {
        fprintf(stderr, "clock_gettime end failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);

    printf("fb=%s loops=%d ok=%d fail=%d elapsed_ms=%ld\n",
           fb, loops, ok, fail, delta_us(&t0, &t1) / 1000L);

    if (ok > 0) {
        double avg = (double)sum_us / (double)ok;
        printf("wait_us min=%ld avg=%.2f max=%ld\n", min_us, avg, max_us);
    }

    if (fail > 0)
        return 2;

    return 0;
}
