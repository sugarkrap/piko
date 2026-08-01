/*
 * hwclock -- read and write the PXA25x RTC.
 *
 * This rootfs's busybox has no hwclock applet (same hole as kill, tar and
 * md5sum -- see userspace/src/kill.c), so with the kernel driver alone
 * there was still no way to persist a time change or to re-sync the system
 * clock after setting it. The kernel does the boot-time half by itself
 * (CONFIG_RTC_HCTOSYS reads rtc0 into the system clock during boot); this
 * covers everything after that.
 *
 *   hwclock                    show the RTC (same as -r)
 *   hwclock -r                 show the RTC
 *   hwclock -s                 RTC   -> system clock   (hctosys)
 *   hwclock -w                 system -> RTC           (systohc)
 *   hwclock --set --date=STR   set the RTC to STR
 *   hwclock --setfields Y M D h m s
 *                              set the RTC from six plain integers
 *
 *   -u          the RTC keeps UTC (default)
 *   -l          the RTC keeps local time
 *   -f DEV      RTC device, default /dev/rtc0
 *
 * --setfields exists because the device's busybox ash has no printf and no
 * working `echo -n`, so /usr/sbin/settime cannot zero-pad "8" into "08" to
 * build a "YYYY-MM-DD HH:MM:SS" string. Passing six unpadded integers
 * straight through sidesteps shell string surgery entirely.
 *
 * UTC is the default because nothing else on this board reads the RTC --
 * there is no second OS to stay compatible with -- and a UTC RTC keeps
 * `hwclock -w` correct regardless of what /etc/TZ says at the time.
 *
 * Cross-compile (same toolchain as the rest of userspace/src):
 *   $GCC -march=armv5te -O2 -static -o hwclock hwclock.c
 *   arm-buildroot-linux-uclibcgnueabi-strip hwclock
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_RTC "/dev/rtc0"

static const char *rtc_dev = DEFAULT_RTC;
static int rtc_is_utc = 1;

static void usage(void)
{
    fprintf(stderr,
        "usage: hwclock [-r|-s|-w] [-u|-l] [-f DEV]\n"
        "       hwclock --set --date=\"YYYY-MM-DD HH:MM:SS\"\n"
        "       hwclock --setfields YEAR MON DAY HOUR MIN SEC\n"
        "\n"
        "  -r  show the RTC (default)     -u  RTC keeps UTC (default)\n"
        "  -s  RTC -> system clock        -l  RTC keeps local time\n"
        "  -w  system clock -> RTC        -f  RTC device (default %s)\n",
        DEFAULT_RTC);
    exit(1);
}

static int rtc_open(int flags)
{
    int fd = open(rtc_dev, flags);

    if (fd < 0) {
        fprintf(stderr, "hwclock: %s: %s\n", rtc_dev, strerror(errno));
        if (errno == ENOENT)
            fprintf(stderr,
                "hwclock: no RTC device -- is CONFIG_RTC_DRV_SA1100=y in the\n"
                "hwclock: running kernel? check: dmesg | grep rtc\n");
        exit(1);
    }
    return fd;
}

/*
 * struct rtc_time has the same layout as the leading fields of struct tm,
 * but the kernel neither fills nor honours tm_isdst/tm_wday/tm_yday, so
 * convert field by field rather than casting between the two.
 */
static void rtc_to_tm(const struct rtc_time *r, struct tm *t)
{
    memset(t, 0, sizeof(*t));
    t->tm_sec  = r->tm_sec;
    t->tm_min  = r->tm_min;
    t->tm_hour = r->tm_hour;
    t->tm_mday = r->tm_mday;
    t->tm_mon  = r->tm_mon;
    t->tm_year = r->tm_year;
    t->tm_isdst = -1;
}

static void tm_to_rtc(const struct tm *t, struct rtc_time *r)
{
    memset(r, 0, sizeof(*r));
    r->tm_sec  = t->tm_sec;
    r->tm_min  = t->tm_min;
    r->tm_hour = t->tm_hour;
    r->tm_mday = t->tm_mday;
    r->tm_mon  = t->tm_mon;
    r->tm_year = t->tm_year;
}

/* Interpret a broken-down RTC reading as an epoch time, honouring -u/-l. */
static time_t tm_to_epoch(struct tm *t)
{
    if (rtc_is_utc)
        return timegm(t);
    return mktime(t);
}

static time_t read_rtc(void)
{
    struct rtc_time rt;
    struct tm t;
    time_t when;
    int fd = rtc_open(O_RDONLY);

    if (ioctl(fd, RTC_RD_TIME, &rt) < 0) {
        fprintf(stderr, "hwclock: RTC_RD_TIME: %s\n", strerror(errno));
        close(fd);
        exit(1);
    }
    close(fd);

    rtc_to_tm(&rt, &t);
    when = tm_to_epoch(&t);
    if (when == (time_t)-1) {
        fprintf(stderr, "hwclock: RTC holds an unconvertible date\n");
        exit(1);
    }
    return when;
}

static void write_rtc(time_t when)
{
    struct rtc_time rt;
    struct tm t;
    int fd;

    if (rtc_is_utc)
        gmtime_r(&when, &t);
    else
        localtime_r(&when, &t);

    tm_to_rtc(&t, &rt);

    fd = rtc_open(O_WRONLY);
    if (ioctl(fd, RTC_SET_TIME, &rt) < 0) {
        fprintf(stderr, "hwclock: RTC_SET_TIME: %s\n", strerror(errno));
        if (errno == EACCES || errno == EPERM)
            fprintf(stderr, "hwclock: (must be root to set the RTC)\n");
        close(fd);
        exit(1);
    }
    close(fd);
}

static void show(time_t when)
{
    char buf[64];
    struct tm t;

    localtime_r(&when, &t);
    if (strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &t) == 0)
        strcpy(buf, "?");
    printf("%s  (RTC keeps %s)\n", buf, rtc_is_utc ? "UTC" : "local time");
}

static void do_hctosys(void)
{
    struct timeval tv;

    tv.tv_sec = read_rtc();
    tv.tv_usec = 0;
    if (settimeofday(&tv, NULL) < 0) {
        fprintf(stderr, "hwclock: settimeofday: %s\n", strerror(errno));
        exit(1);
    }
}

static void do_systohc(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) < 0) {
        fprintf(stderr, "hwclock: gettimeofday: %s\n", strerror(errno));
        exit(1);
    }
    write_rtc(tv.tv_sec);
}

/*
 * Accepts "YYYY-MM-DD HH:MM:SS", "YYYY-MM-DDTHH:MM:SS" and the compact
 * "YYYYMMDDHHMMSS". Seconds are optional in the punctuated forms.
 */
static int parse_date(const char *s, struct tm *t)
{
    int y, mo, d, h, mi, se = 0;
    size_t i, len = strlen(s);

    memset(t, 0, sizeof(*t));
    t->tm_isdst = -1;

    /*
     * Compact form first. "%d-%d-%d" applied to 14 straight digits would
     * feed the whole run to a single %d and overflow it before failing on
     * the missing '-', so never let the punctuated pattern see this shape.
     */
    if (len == 14) {
        for (i = 0; i < len; i++)
            if (s[i] < '0' || s[i] > '9')
                break;
        if (i == len &&
            sscanf(s, "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &se) == 6)
            goto got_it;
    }

    se = 0;
    if (sscanf(s, "%d-%d-%d%*[ T]%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 5)
        goto got_it;

    return -1;

got_it:
    t->tm_year = y - 1900;
    t->tm_mon  = mo - 1;
    t->tm_mday = d;
    t->tm_hour = h;
    t->tm_min  = mi;
    t->tm_sec  = se;
    return 0;
}

static int field(const char *s, const char *what)
{
    char *end;
    long v = strtol(s, &end, 10);

    if (*s == '\0' || *end != '\0') {
        fprintf(stderr, "hwclock: %s: not a number: '%s'\n", what, s);
        exit(1);
    }
    return (int)v;
}

/*
 * Sanity-check before writing. The RTC will happily accept 2026-02-31 and
 * hand back something else entirely on the next read; catching it here
 * means `settime` reports the typo instead of silently storing garbage.
 */
static void set_from_tm(struct tm *t)
{
    struct tm check;
    time_t when;

    check = *t;
    when = tm_to_epoch(&check);
    if (when == (time_t)-1 ||
        check.tm_mday != t->tm_mday || check.tm_mon != t->tm_mon ||
        check.tm_year != t->tm_year) {
        fprintf(stderr, "hwclock: not a real date/time\n");
        exit(1);
    }

    /*
     * The PXA RTC counts seconds in a 32-bit register, so it cannot
     * represent anything before the epoch at all.
     */
    if (when < 0) {
        fprintf(stderr, "hwclock: dates before 1970 cannot be stored\n");
        exit(1);
    }

    write_rtc(when);
}

int main(int argc, char **argv)
{
    enum { ACT_SHOW, ACT_HCTOSYS, ACT_SYSTOHC, ACT_SET } action = ACT_SHOW;
    const char *datestr = NULL;
    int setfields = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (!strcmp(a, "-r") || !strcmp(a, "--show")) {
            action = ACT_SHOW;
        } else if (!strcmp(a, "-s") || !strcmp(a, "--hctosys")) {
            action = ACT_HCTOSYS;
        } else if (!strcmp(a, "-w") || !strcmp(a, "--systohc")) {
            action = ACT_SYSTOHC;
        } else if (!strcmp(a, "--set")) {
            action = ACT_SET;
        } else if (!strncmp(a, "--date=", 7)) {
            datestr = a + 7;
            action = ACT_SET;
        } else if (!strcmp(a, "--date")) {
            if (++i >= argc)
                usage();
            datestr = argv[i];
            action = ACT_SET;
        } else if (!strcmp(a, "--setfields")) {
            action = ACT_SET;
            setfields = i + 1;
            /* six positional integers follow; stop option parsing here */
            if (argc - setfields < 6)
                usage();
            i = argc;
        } else if (!strcmp(a, "-u") || !strcmp(a, "--utc")) {
            rtc_is_utc = 1;
        } else if (!strcmp(a, "-l") || !strcmp(a, "--localtime")) {
            rtc_is_utc = 0;
        } else if (!strcmp(a, "-f")) {
            if (++i >= argc)
                usage();
            rtc_dev = argv[i];
        } else if (!strncmp(a, "-f", 2) && a[2] != '\0') {
            rtc_dev = a + 2;
        } else {
            usage();
        }
    }

    switch (action) {
    case ACT_SHOW:
        show(read_rtc());
        break;

    case ACT_HCTOSYS:
        do_hctosys();
        break;

    case ACT_SYSTOHC:
        do_systohc();
        break;

    case ACT_SET: {
        struct tm t;

        if (setfields) {
            memset(&t, 0, sizeof(t));
            t.tm_isdst = -1;
            t.tm_year = field(argv[setfields + 0], "year")  - 1900;
            t.tm_mon  = field(argv[setfields + 1], "month") - 1;
            t.tm_mday = field(argv[setfields + 2], "day");
            t.tm_hour = field(argv[setfields + 3], "hour");
            t.tm_min  = field(argv[setfields + 4], "minute");
            t.tm_sec  = field(argv[setfields + 5], "second");
        } else if (datestr) {
            if (parse_date(datestr, &t) < 0) {
                fprintf(stderr,
                    "hwclock: cannot parse '%s' -- want \"YYYY-MM-DD HH:MM:SS\"\n",
                    datestr);
                return 1;
            }
        } else {
            fprintf(stderr, "hwclock: --set needs --date or --setfields\n");
            return 1;
        }
        set_from_tm(&t);
        break;
    }
    }

    return 0;
}
