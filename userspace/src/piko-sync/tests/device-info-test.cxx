#include "../device_info.h"
#include "../protocol.h"

#include <stdio.h>
#include <sys/time.h>

#include <string>
#include <vector>

using namespace piko_sync;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static double now_milliseconds()
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main()
{
    printf("device info\n");

    double started = now_milliseconds();
    DeviceInfoAckMsg info;
    info.hostname = device_hostname();
    info.model = "SL-C860";
    info.memory_total = meminfo_kilobytes("MemTotal") * 1024;
    collect_mounts(info.mounts);
    double elapsed = now_milliseconds() - started;

    printf("  hostname      %s\n", info.hostname.c_str());
    printf("  memory total  %llu bytes\n", (unsigned long long)info.memory_total);
    printf("  mounts        %u\n", (unsigned)info.mounts.size());
    for (size_t i = 0; i < info.mounts.size(); i++)
        printf("    %-20s %-16s %llu free of %llu\n",
               info.mounts[i].mount_point.c_str(), info.mounts[i].device.c_str(),
               (unsigned long long)info.mounts[i].free_bytes,
               (unsigned long long)info.mounts[i].total_bytes);
    printf("  gathered in   %.1f ms\n", elapsed);

    check(!info.hostname.empty(), "hostname is not empty");
    check(info.memory_total > 0, "memory total is known");
    check(!info.mounts.empty(), "at least one mount was found");

    bool duplicated = false;
    for (size_t i = 0; i < info.mounts.size(); i++)
        for (size_t j = i + 1; j < info.mounts.size(); j++)
            if (info.mounts[i].device == info.mounts[j].device)
                duplicated = true;
    check(!duplicated, "no device is listed twice");

    uint64_t busy = 0;
    uint64_t total = 0;
    check(cpu_sample(busy, total), "cpu sample reads");
    check(total > 0 && busy <= total, "cpu sample is coherent");

    std::string encoded = encode(info);
    DeviceInfoAckMsg decoded;
    check(decode_device_info_ack(encoded, decoded), "device info round trips");
    check(decoded.hostname == info.hostname, "hostname survives the round trip");
    check(decoded.mounts.size() == info.mounts.size(), "mount count survives");
    if (decoded.mounts.size() == info.mounts.size() && !info.mounts.empty()) {
        check(decoded.mounts[0].mount_point == info.mounts[0].mount_point, "mount point survives");
        check(decoded.mounts[0].total_bytes == info.mounts[0].total_bytes, "mount size survives");
    }

    DeviceStatsAckMsg stats;
    stats.cpu_percent = 42;
    stats.memory_total = 64 * 1024 * 1024;
    stats.memory_available = 12 * 1024 * 1024;
    DeviceStatsAckMsg stats_back;
    check(decode_device_stats_ack(encode(stats), stats_back), "stats round trip");
    check(stats_back.cpu_percent == 42, "cpu percent survives");
    check(stats_back.memory_available == 12 * 1024 * 1024, "available memory survives");

    WallpaperInfoMsg wallpaper;
    wallpaper.ok = true;
    wallpaper.unchanged = false;
    wallpaper.checksum = 0xdeadbeefu;
    wallpaper.byte_count = 4096;
    wallpaper.path = "/usr/share/backgrounds/piko-default.png";
    WallpaperInfoMsg wallpaper_back;
    check(decode_wallpaper_info(encode(wallpaper), wallpaper_back), "wallpaper info round trip");
    check(wallpaper_back.checksum == 0xdeadbeefu, "checksum survives");
    check(wallpaper_back.path == wallpaper.path, "wallpaper path survives");

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
