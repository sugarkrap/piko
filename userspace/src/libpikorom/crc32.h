#ifndef PIKO_CRC32_H
#define PIKO_CRC32_H

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include <map>
#include <string>

namespace piko_sync {

class Crc32 {
public:
    Crc32() : crc_(0xffffffffu) {}

    void update(const char *data, size_t len)
    {
        const uint32_t *table = table_();
        const unsigned char *p = reinterpret_cast<const unsigned char *>(data);
        for (size_t i = 0; i < len; i++)
            crc_ = table[(crc_ ^ p[i]) & 0xff] ^ (crc_ >> 8);
    }

    uint32_t final_value() const { return crc_ ^ 0xffffffffu; }

private:
    static const uint32_t *table_()
    {
        static uint32_t t[256];
        static bool ready = false;
        if (!ready) {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++)
                    c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            ready = true;
        }
        return t;
    }

    uint32_t crc_;
};

struct FileCrcCacheEntry {
    off_t size;
    time_t mtime;
    uint32_t crc;
};

inline uint32_t file_crc32(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return 0;
    Crc32 crc;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        crc.update(buf, n);
    fclose(f);
    return crc.final_value();
}

inline uint32_t cached_file_crc32(const std::string &path)
{
    static std::map<std::string, FileCrcCacheEntry> cache;

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        cache.erase(path);
        return 0;
    }

    std::map<std::string, FileCrcCacheEntry>::iterator it = cache.find(path);
    if (it != cache.end() && it->second.size == st.st_size && it->second.mtime == st.st_mtime)
        return it->second.crc;

    FileCrcCacheEntry entry;
    entry.size = st.st_size;
    entry.mtime = st.st_mtime;
    entry.crc = file_crc32(path);
    cache[path] = entry;
    return entry.crc;
}

}

#endif
