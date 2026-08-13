
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

inline std::string detect_machine(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return std::string();

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

inline std::string machine_backend(const std::string &machine)
{
    if (machine == "SNES")
        return std::string("pocketsnes");
    return std::string();
}

}

#endif
