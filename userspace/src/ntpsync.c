/*
 * ntpsync -- minimal SNTP client: set the system clock from an NTP server,
 * then persist it to the RTC.
 *
 * This rootfs's busybox has neither ntpd nor rdate (nor, before
 * userspace/src/hwclock.c, hwclock), so there was no way at all to get an
 * accurate time onto the device short of typing it in. One UDP round trip
 * is all this needs, which suits a machine whose only link is a 2003-era
 * Prism2 card.
 *
 *   ntpsync                 sync from the default pool, then write the RTC
 *   ntpsync -n              sync the system clock only, leave the RTC alone
 *   ntpsync -q              quiet: no output on success
 *   ntpsync SERVER...       try these servers instead of the default list
 *
 * On success it runs `hwclock -w` (unless -n) so the new time survives a
 * reboot -- doing it here rather than leaving it to the caller means the
 * one-word device command does the whole job, which is the same reasoning
 * behind bright/wifiup/audioon.
 *
 * No adjtime, no drift file, no gradual slew: this steps the clock. On a
 * board that boots at the epoch, stepping is the point.
 *
 * Cross-compile (same toolchain as the rest of userspace/src):
 *   $GCC -march=armv5te -O2 -static -o ntpsync ntpsync.c
 *   arm-buildroot-linux-uclibcgnueabi-strip ntpsync
 */

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Seconds between the NTP epoch (1900-01-01) and the Unix epoch. */
#define NTP_UNIX_DELTA 2208988800UL

#define NTP_PACKET_LEN 48
#define RECV_TIMEOUT_SEC 5
#define ATTEMPTS_PER_SERVER 2

#define HWCLOCK_PATH "/usr/sbin/hwclock"

static const char *default_servers[] = {
    "pool.ntp.org",
    "time.cloudflare.com",
    NULL
};

static int quiet = 0;

/*
 * Query one server. Returns 0 and fills *out on success, -1 otherwise.
 * Every failure path here is non-fatal -- the caller moves to the next
 * server -- so nothing in here exits.
 */
static int query(const char *host, time_t *out)
{
    struct addrinfo hints, *res = NULL, *ai;
    unsigned char pkt[NTP_PACKET_LEN];
    struct timeval tv;
    int rc, attempt;
    int ok = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* no IPv6 anywhere on this board */
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(host, "123", &hints, &res);
    if (rc != 0) {
        if (!quiet)
            fprintf(stderr, "ntpsync: %s: %s\n", host, gai_strerror(rc));
        return -1;
    }

    for (ai = res; ai != NULL && ok != 0; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

        if (fd < 0)
            continue;

        tv.tv_sec = RECV_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            close(fd);
            continue;
        }

        for (attempt = 0; attempt < ATTEMPTS_PER_SERVER; attempt++) {
            ssize_t n;
            unsigned long secs;

            memset(pkt, 0, sizeof(pkt));
            pkt[0] = 0x23;   /* LI = 0, VN = 4, Mode = 3 (client) */

            if (send(fd, pkt, sizeof(pkt), 0) < 0)
                break;

            n = recv(fd, pkt, sizeof(pkt), 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;        /* timed out -- one more try */
                break;
            }
            if (n != NTP_PACKET_LEN)
                continue;

            /* Mode 4 = server. Stratum 0 means "kiss of death", not a time. */
            if ((pkt[0] & 0x07) != 4 || pkt[1] == 0)
                break;

            /* Transmit timestamp: seconds since 1900, big-endian, at [40..43]. */
            secs = ((unsigned long)pkt[40] << 24) |
                   ((unsigned long)pkt[41] << 16) |
                   ((unsigned long)pkt[42] << 8)  |
                   ((unsigned long)pkt[43]);

            if (secs <= NTP_UNIX_DELTA)
                break;

            *out = (time_t)(secs - NTP_UNIX_DELTA);
            ok = 0;
            break;
        }

        close(fd);
    }

    freeaddrinfo(res);
    return ok;
}

/*
 * Hand the RTC write to hwclock rather than repeating the ioctl dance --
 * one place decides how the RTC is interpreted (UTC by default), so the
 * two tools can never disagree about it.
 */
static int persist_to_rtc(void)
{
    pid_t pid = fork();
    int status;

    if (pid < 0) {
        fprintf(stderr, "ntpsync: fork: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl(HWCLOCK_PATH, "hwclock", "-w", (char *)NULL);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "ntpsync: %s -w failed -- time set, but not persisted\n",
                HWCLOCK_PATH);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char **servers = default_servers;
    const char *argv_servers[16];
    int write_rtc = 1;
    int i, n_args = 0;
    time_t now = 0;
    struct timeval tv;
    char buf[64];
    struct tm t;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n")) {
            write_rtc = 0;
        } else if (!strcmp(argv[i], "-q")) {
            quiet = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr,
                "usage: ntpsync [-n] [-q] [server ...]\n"
                "  -n  set the system clock only, do not write the RTC\n"
                "  -q  no output on success\n");
            return 1;
        } else if (n_args < (int)(sizeof(argv_servers) / sizeof(argv_servers[0])) - 1) {
            argv_servers[n_args++] = argv[i];
        }
    }

    if (n_args > 0) {
        argv_servers[n_args] = NULL;
        servers = argv_servers;
    }

    for (i = 0; servers[i] != NULL; i++) {
        if (query(servers[i], &now) == 0)
            break;
    }

    if (now == 0) {
        fprintf(stderr, "ntpsync: no server answered -- is wlan0 up and routed?\n");
        return 1;
    }

    /*
     * A server that hands back something absurd is worse than no sync at
     * all: it would be written straight to the RTC. Anything earlier than
     * this source file was written is not a real answer.
     */
    if (now < 1750000000L) {
        fprintf(stderr, "ntpsync: server returned an implausible time, ignoring\n");
        return 1;
    }

    tv.tv_sec = now;
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) < 0) {
        fprintf(stderr, "ntpsync: settimeofday: %s\n", strerror(errno));
        if (errno == EPERM)
            fprintf(stderr, "ntpsync: (must be root)\n");
        return 1;
    }

    localtime_r(&now, &t);
    if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &t) == 0)
        strcpy(buf, "?");
    if (!quiet)
        printf("ntpsync: system clock set to %s\n", buf);

    if (write_rtc && persist_to_rtc() == 0 && !quiet)
        printf("ntpsync: written to the RTC\n");

    return 0;
}
