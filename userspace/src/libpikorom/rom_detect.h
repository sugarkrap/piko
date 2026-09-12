
#ifndef PIKO_SYNC_ROM_DETECT_H
#define PIKO_SYNC_ROM_DETECT_H

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <string>

namespace piko_sync {

const size_t SMC_COPIER_HEADER = 512;
const size_t SNES_LOROM_HEADER = 0x7fc0;
const size_t SNES_HIROM_HEADER = 0xffc0;
const size_t SNES_HEADER_SIZE = 32;
const size_t SNES_CHECKSUM_OFF = 0x1e;
const size_t SNES_COMPLEMENT_OFF = 0x1c;

inline bool snes_header_is_valid(const unsigned char *h)
{
    unsigned complement = h[SNES_COMPLEMENT_OFF] | (h[SNES_COMPLEMENT_OFF + 1] << 8);
    unsigned checksum = h[SNES_CHECKSUM_OFF] | (h[SNES_CHECKSUM_OFF + 1] << 8);
    if (checksum == 0 && complement == 0)
        return false;
    return (checksum ^ complement) == 0xffff;
}

inline bool read_at(FILE *f, long off, unsigned char *buf, size_t len)
{
    if (fseek(f, off, SEEK_SET) != 0)
        return false;
    return fread(buf, 1, len, f) == len;
}

inline bool has_suffix_nocase(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i < n; i++) {
        char a = s[s.size() - n + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

inline bool looks_like_midlet_jar(FILE *f)
{
    unsigned char magic[4];
    if (!read_at(f, 0, magic, sizeof(magic)))
        return false;
    return magic[0] == 'P' && magic[1] == 'K' && magic[2] == 0x03 && magic[3] == 0x04;
}

inline bool looks_like_midlet_jad(FILE *f)
{
    char buf[2048];
    size_t got;
    if (fseek(f, 0, SEEK_SET) != 0)
        return false;
    got = fread(buf, 1, sizeof(buf) - 1, f);
    if (got == 0)
        return false;
    buf[got] = '\0';
    return strstr(buf, "MIDlet-1") != NULL || strstr(buf, "MIDlet-Name") != NULL;
}

inline std::string detect_machine(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return std::string();

    if (has_suffix_nocase(path, ".jad") && looks_like_midlet_jad(f)) {
        fclose(f);
        return std::string("J2ME");
    }
    if (has_suffix_nocase(path, ".jar") && looks_like_midlet_jar(f)) {
        fclose(f);
        return std::string("J2ME");
    }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return std::string(); }
    long size = ftell(f);
    if (size <= 0) { fclose(f); return std::string(); }

    long base = ((size % 1024) == (long)SMC_COPIER_HEADER) ? (long)SMC_COPIER_HEADER : 0;

    unsigned char h[SNES_HEADER_SIZE];
    bool snes = false;
    if (read_at(f, base + (long)SNES_LOROM_HEADER, h, sizeof(h)))
        snes = snes_header_is_valid(h);
    if (!snes && read_at(f, base + (long)SNES_HIROM_HEADER, h, sizeof(h)))
        snes = snes_header_is_valid(h);

    fclose(f);
    return snes ? std::string("SNES") : std::string();
}

struct BackendInfo {
    const char *name;
    const char *display;
    const char *machines;
    const char *extensions;
};

inline const BackendInfo *backend_table(size_t &count)
{
    static const BackendInfo table[] = {
        { "phoneme", "phoneME", "J2ME", "jar,jad" },
    };
    count = sizeof(table) / sizeof(table[0]);
    return table;
}

inline bool backend_supports(const BackendInfo &backend, const std::string &machine)
{
    std::string list(backend.machines);
    size_t start = 0;
    bool supported = false;
    while (start <= list.size() && !supported) {
        size_t stop = list.find(',', start);
        if (stop == std::string::npos)
            stop = list.size();
        if (list.substr(start, stop - start) == machine)
            supported = true;
        start = stop + 1;
    }
    return supported;
}

inline std::string machine_backend(const std::string &machine)
{
    size_t count = 0;
    const BackendInfo *table = backend_table(count);
    for (size_t i = 0; i < count; i++)
        if (backend_supports(table[i], machine))
            return std::string(table[i].name);
    return std::string();
}

}

#endif
