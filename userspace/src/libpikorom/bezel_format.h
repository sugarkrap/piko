#ifndef PIKO_BEZEL_FORMAT_H
#define PIKO_BEZEL_FORMAT_H

#include <stdio.h>
#include <string.h>
#include <string>

#include "piko_asset_format.h"

struct PkbzHeader {
    unsigned int version;
    unsigned int width, height;
    unsigned int screen_x, screen_y, screen_w, screen_h;
    unsigned int offset_x, offset_y;
    std::string  source;

    PkbzHeader()
        : version(PKBZ_VERSION), width(0), height(0),
          screen_x(0), screen_y(0), screen_w(0), screen_h(0),
          offset_x(0), offset_y(0) {}
};

inline void pkbz_put_u32(std::string &out, unsigned int v)
{
    out += (char)(v & 0xFF);
    out += (char)((v >> 8) & 0xFF);
    out += (char)((v >> 16) & 0xFF);
    out += (char)((v >> 24) & 0xFF);
}

inline unsigned int pkbz_get_u32(const unsigned char *p)
{
    return piko_asset_u32(p);
}

inline std::string pkbz_encode(const PkbzHeader &h, const unsigned short *pixels,
                               const unsigned char *alpha = 0)
{
    size_t count = (size_t)h.width * h.height;
    std::string out;
    out += (char)PKBZ_MAGIC0; out += (char)PKBZ_MAGIC1;
    out += (char)PKBZ_MAGIC2; out += (char)PKBZ_MAGIC3;
    pkbz_put_u32(out, PKBZ_VERSION);
    pkbz_put_u32(out, h.width);
    pkbz_put_u32(out, h.height);
    pkbz_put_u32(out, h.screen_x);
    pkbz_put_u32(out, h.screen_y);
    pkbz_put_u32(out, h.screen_w);
    pkbz_put_u32(out, h.screen_h);
    pkbz_put_u32(out, h.offset_x);
    pkbz_put_u32(out, h.offset_y);
    pkbz_put_u32(out, (unsigned int)h.source.size());
    out.append(h.source);
    out.append((const char *)pixels, count * 2);
    if (alpha != 0)
        out.append((const char *)alpha, count);
    else
        out.append(count, (char)0xFF);
    return out;
}

inline bool pkbz_decode_header(const std::string &blob, PkbzHeader &h,
                               size_t &pixel_offset)
{
    struct pkbz_head raw;
    if (!pkbz_parse_head((const unsigned char *)blob.data(), blob.size(), &raw))
        return false;
    if (blob.size() < raw.pixel_offset)
        return false;
    h.version  = raw.version;
    h.width    = raw.width;
    h.height   = raw.height;
    h.screen_x = raw.screen_x;
    h.screen_y = raw.screen_y;
    h.screen_w = raw.screen_w;
    h.screen_h = raw.screen_h;
    h.offset_x = raw.offset_x;
    h.offset_y = raw.offset_y;
    h.source.assign(blob, PKBZ_HDR_FIXED, raw.source_len);
    pixel_offset = raw.pixel_offset;
    return blob.size() - pixel_offset >= pkbz_colour_bytes(&raw) + pkbz_alpha_bytes(&raw);
}

inline size_t pkbz_alpha_offset(const PkbzHeader &h, size_t pixel_offset)
{
    return pixel_offset + (size_t)h.width * h.height * 2;
}

inline unsigned short pkbz_rgb565(int r, int g, int b)
{
    return (unsigned short)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#endif
