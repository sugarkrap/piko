#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>

#include "piko_art.h"

#define PRIMARY   "\033[38;2;196;196;196m"
#define MID       "\033[38;2;143;143;143m"
#define LOW       "\033[38;2;105;105;105m"
#define RESET     "\033[0m"

static void human_kib(unsigned long kib, char *out, size_t outlen)
{
    if (kib >= 1024UL * 1024UL)
        snprintf(out, outlen, "%.1f GiB", kib / (1024.0 * 1024.0));
    else if (kib >= 1024UL)
        snprintf(out, outlen, "%.1f MiB", kib / 1024.0);
    else
        snprintf(out, outlen, "%lu KiB", kib);
}

static void print_meminfo(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    unsigned long total = 0, avail = 0, free_ = 0;
    char line[256];
    char tbuf[32], abuf[32];

    if (!f) {
        printf(MID "Memory:    " RESET "unknown\n");
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        unsigned long v;
        if (sscanf(line, "MemTotal: %lu kB", &v) == 1) total = v;
        else if (sscanf(line, "MemAvailable: %lu kB", &v) == 1) avail = v;
        else if (sscanf(line, "MemFree: %lu kB", &v) == 1) free_ = v;
    }
    fclose(f);
    if (!avail)
        avail = free_;

    human_kib(total, tbuf, sizeof(tbuf));
    human_kib(total - avail, abuf, sizeof(abuf));
    printf(MID "Memory:    " RESET "%s / %s\n", abuf, tbuf);
}

static void print_uptime(void)
{
    FILE *f = fopen("/proc/uptime", "r");
    double up = 0;
    long days, hours, mins;

    if (!f || fscanf(f, "%lf", &up) != 1) {
        printf(MID "Uptime:    " RESET "unknown\n");
        if (f) fclose(f);
        return;
    }
    fclose(f);

    days  = (long)up / 86400;
    hours = ((long)up % 86400) / 3600;
    mins  = ((long)up % 3600) / 60;

    printf(MID "Uptime:    " RESET);
    if (days)
        printf("%ldd %ldh %ldm\n", days, hours, mins);
    else if (hours)
        printf("%ldh %ldm\n", hours, mins);
    else
        printf("%ldm\n", mins);
}

static void print_cpu(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[256];
    char model[128] = "unknown";
    char hw[128] = "";

    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *colon = strchr(line, ':');
            if (!colon)
                continue;
            char *val = colon + 1;
            while (*val == ' ' || *val == '\t') val++;
            val[strcspn(val, "\n")] = 0;
            if (!strncmp(line, "Hardware", 8))
                snprintf(hw, sizeof(hw), "%s", val);
            else if (!strncmp(line, "Processor", 9) || !strncmp(line, "model name", 10))
                snprintf(model, sizeof(model), "%s", val);
        }
        fclose(f);
    }
    printf(MID "CPU:       " RESET "%s%s%s\n", model, hw[0] ? " / " : "", hw);
}

static int is_mounted(const char *path)
{
    FILE *f = fopen("/proc/mounts", "r");
    char line[512];
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char dev[256], mnt[256];
        if (sscanf(line, "%255s %255s", dev, mnt) != 2)
            continue;
        if (!strcmp(mnt, path)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void print_disk(const char *label, const char *path)
{
    struct statvfs sv;
    char ubuf[32], tbuf[32];
    unsigned long total_kib, used_kib;

    if (!is_mounted(path)) {
        printf(MID "%s" RESET "not mounted\n", label);
        return;
    }

    if (statvfs(path, &sv) != 0)
        return;

    total_kib = (unsigned long)((unsigned long long)sv.f_blocks * sv.f_frsize / 1024ULL);
    used_kib  = (unsigned long)(((unsigned long long)sv.f_blocks - sv.f_bfree) * sv.f_frsize / 1024ULL);

    human_kib(used_kib, ubuf, sizeof(ubuf));
    human_kib(total_kib, tbuf, sizeof(tbuf));
    printf(MID "%s" RESET "%s / %s\n", label, ubuf, tbuf);
}

int main(void)
{
    int i;
    struct utsname uts;
    char host[64] = "zaurus";
    const char *user = getenv("USER");
    const char *shell = getenv("SHELL");

    gethostname(host, sizeof(host));
    uname(&uts);

    for (i = 0; i < PIKO_ART_LINES; i++)
        printf("%s\n", piko_art[i]);

    printf("\n" PRIMARY "%s" RESET LOW "@" RESET PRIMARY "%s" RESET "\n",
           user ? user : "piko", host);
    printf(LOW "-----------------------------\n" RESET);
    printf(MID "OS:        " RESET "zaurus-refresh stage 2 (otQuake)\n");
    printf(MID "Kernel:    " RESET "%s %s\n", uts.sysname, uts.release);
    print_uptime();
    print_cpu();
    print_meminfo();
    printf(MID "Shell:     " RESET "%s\n", shell ? shell : "unknown");
    print_disk("Disk (/):     ", "/");
    print_disk("Disk (/mnt/card): ", "/mnt/card");
    printf("\n");

    return 0;
}
