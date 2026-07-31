/*
 * kill -- this rootfs's busybox has no kill applet at all (confirmed via
 * `busybox kill` -> "applet not found"), leaving no way to stop a
 * backgrounded process short of a reboot. Minimal replacement covering
 * the two things actually needed here: `kill [-SIGNAL] PID...`.
 *
 * Cross-compile (same toolchain as the rest of userspace/src):
 *   $GCC -march=armv5te -O2 -static -o kill kill.c
 *   arm-buildroot-linux-uclibcgnueabi-strip kill
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-SIGNAL] pid [pid ...]\n", prog);
    exit(1);
}

int main(int argc, char **argv)
{
    int sig = SIGTERM;
    int i, first_pid = 1;
    int failures = 0;

    if (argc < 2)
        usage(argv[0]);

    if (argv[1][0] == '-') {
        char *end;
        long v = strtol(argv[1] + 1, &end, 10);

        if (*end != '\0' || v <= 0) {
            fprintf(stderr, "kill: unrecognized signal '%s'\n", argv[1] + 1);
            return 1;
        }
        sig = (int)v;
        first_pid = 2;
    }

    if (first_pid >= argc)
        usage(argv[0]);

    for (i = first_pid; i < argc; i++) {
        char *end;
        long pid = strtol(argv[i], &end, 10);

        if (*end != '\0') {
            fprintf(stderr, "kill: not a pid: %s\n", argv[i]);
            failures++;
            continue;
        }
        if (kill((pid_t)pid, sig) < 0) {
            fprintf(stderr, "kill: (%ld): %s\n", pid, strerror(errno));
            failures++;
        }
    }

    return failures ? 1 : 0;
}
