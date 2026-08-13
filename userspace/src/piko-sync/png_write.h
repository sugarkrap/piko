
#ifndef PIKO_SYNC_PNG_WRITE_H
#define PIKO_SYNC_PNG_WRITE_H

#include <stdio.h>
#include <stdint.h>
#include <zlib.h>

#include <string>
#include <vector>

#include "protocol.h"

namespace piko_sync {

inline void png_chunk(std::string &out, const char *type, const std::string &data)
{
    put_u32(out, static_cast<uint32_t>(data.size()));
    std::string body(type, 4);
    body.append(data);
    out.append(body);
    Crc32 c;
    c.update(body.data(), body.size());
    put_u32(out, c.final_value());
}

inline bool png_encode_rgb(const std::string &rgb, uint32_t width,
                           uint32_t height, std::string &out, std::string &err)
{
    if (rgb.size() != static_cast<size_t>(width) * height * 3) {
        err = "internal: RGB buffer size does not match the given geometry";
        return false;
    }

    std::string raw;
    raw.reserve(static_cast<size_t>(height) * (1 + width * 3));
    for (uint32_t y = 0; y < height; y++) {
        raw.append(1, '\0');
        raw.append(rgb, static_cast<size_t>(y) * width * 3, static_cast<size_t>(width) * 3);
    }

    uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> z(zlen);
    if (compress2(&z[0], &zlen,
                  reinterpret_cast<const Bytef *>(raw.data()),
                  static_cast<uLong>(raw.size()), 6) != Z_OK) {
        err = "zlib compress2 failed";
        return false;
    }

    out.clear();
    const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    out.append(reinterpret_cast<const char *>(sig), 8);

    std::string ihdr;
    put_u32(ihdr, width);
    put_u32(ihdr, height);
    ihdr.append(1, 8);
    ihdr.append(1, 2);
    ihdr.append(1, '\0');
    ihdr.append(1, '\0');
    ihdr.append(1, '\0');
    png_chunk(out, "IHDR", ihdr);

    png_chunk(out, "IDAT", std::string(reinterpret_cast<const char *>(&z[0]), zlen));
    png_chunk(out, "IEND", std::string());
    return true;
}

inline bool png_write_rgb(const char *path, const std::string &rgb,
                          uint32_t width, uint32_t height, std::string &err)
{
    std::string png;
    if (!png_encode_rgb(rgb, width, height, png, err))
        return false;

    FILE *f = fopen(path, "wb");
    if (!f) {
        err = std::string("cannot write ") + path;
        return false;
    }
    size_t wrote = fwrite(png.data(), 1, png.size(), f);
    bool ok = (wrote == png.size());
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        err = std::string("short write to ") + path;
    return ok;
}

inline std::string rgb565_to_rgb888(const std::string &raw, uint32_t width, uint32_t height)
{
    std::string out;
    out.reserve(static_cast<size_t>(width) * height * 3);
    const unsigned char *p = reinterpret_cast<const unsigned char *>(raw.data());
    size_t px = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < px; i++) {
        unsigned v = p[i * 2] | (static_cast<unsigned>(p[i * 2 + 1]) << 8);
        unsigned r = (v >> 11) & 0x1f;
        unsigned g = (v >> 5) & 0x3f;
        unsigned b = v & 0x1f;
        out.append(1, static_cast<char>((r << 3) | (r >> 2)));
        out.append(1, static_cast<char>((g << 2) | (g >> 4)));
        out.append(1, static_cast<char>((b << 3) | (b >> 2)));
    }
    return out;
}

}

#endif
