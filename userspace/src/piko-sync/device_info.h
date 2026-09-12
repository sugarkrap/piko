#ifndef PIKO_SYNC_DEVICE_INFO_H
#define PIKO_SYNC_DEVICE_INFO_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/statvfs.h>

#include <map>
#include <string>
#include <vector>

#include "protocol.h"
#include "crc32.h"

namespace piko_sync {

inline std::string trimmed_line(const std::string &line)
{
    std::string out = line;
    while (!out.empty() && (out[out.size() - 1] == '\n' || out[out.size() - 1] == '\r'))
        out.erase(out.size() - 1);
    return out;
}

inline std::string device_hostname()
{
    char buf[256];
    if (gethostname(buf, sizeof(buf)) != 0)
        return std::string("zaurus");
    buf[sizeof(buf) - 1] = '\0';
    return std::string(buf);
}

inline uint64_t meminfo_kilobytes(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return 0;
    char line[256];
    size_t len = strlen(key);
    uint64_t value = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, len) == 0 && line[len] == ':') {
            value = strtoull(line + len + 1, 0, 10);
            break;
        }
    }
    fclose(f);
    return value;
}

inline bool cpu_sample(uint64_t &busy, uint64_t &total)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return false;
    char line[512];
    bool ok = false;
    if (fgets(line, sizeof(line), f) && strncmp(line, "cpu ", 4) == 0) {
        uint64_t values[8];
        for (int i = 0; i < 8; i++)
            values[i] = 0;
        sscanf(line + 4, "%llu %llu %llu %llu %llu %llu %llu %llu",
               (unsigned long long *)&values[0], (unsigned long long *)&values[1],
               (unsigned long long *)&values[2], (unsigned long long *)&values[3],
               (unsigned long long *)&values[4], (unsigned long long *)&values[5],
               (unsigned long long *)&values[6], (unsigned long long *)&values[7]);
        total = 0;
        for (int i = 0; i < 8; i++)
            total += values[i];
        busy = total - values[3] - values[4];
        ok = true;
    }
    fclose(f);
    return ok;
}

inline void collect_mounts(std::vector<DeviceMount> &out,
                           const std::string &preferred = std::string())
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char device[512];
        char point[512];
        char kind[128];
        if (sscanf(line, "%511s %511s %127s", device, point, kind) != 3)
            continue;
        if (strncmp(device, "/dev/", 5) != 0)
            continue;

        bool seen = false;
        for (size_t i = 0; i < out.size(); i++) {
            if (out[i].device != device)
                continue;
            seen = true;
            if (!preferred.empty() && preferred == point)
                out[i].mount_point = point;
        }
        if (seen)
            continue;

        struct statvfs sv;
        if (statvfs(point, &sv) != 0)
            continue;

        DeviceMount mount;
        mount.mount_point = point;
        mount.device = device;
        mount.total_bytes = static_cast<uint64_t>(sv.f_blocks) * sv.f_frsize;
        mount.free_bytes = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
        out.push_back(mount);
    }
    fclose(f);
}

inline std::string wallpaper_path()
{
    const char *home = getenv("HOME");
    std::string spec_path = std::string(home ? home : "/root") + "/.matchbox/wallpaper";
    static const char *prefixes[] = {
        "img-mosaic:", "img-centered:", "img-stretched:", "img-filled:", 0
    };

    std::string path;
    FILE *f = fopen(spec_path.c_str(), "r");
    if (f) {
        char line[1024];
        if (fgets(line, sizeof(line), f)) {
            std::string spec = trimmed_line(line);
            for (int i = 0; prefixes[i]; i++) {
                size_t len = strlen(prefixes[i]);
                if (spec.compare(0, len, prefixes[i]) == 0) {
                    path = spec.substr(len);
                    break;
                }
            }
        }
        fclose(f);
    }

    if (path.empty())
        path = "/usr/share/backgrounds/piko-default.png";
    return path;
}

inline bool read_whole_file(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}




}

#endif
