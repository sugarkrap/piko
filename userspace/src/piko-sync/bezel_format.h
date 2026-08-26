#ifndef PIKO_BEZEL_FORMAT_H
#define PIKO_BEZEL_FORMAT_H

#include <stdio.h>
#include <string.h>
#include <string>

#define PKBZ_MAGIC0 'P'
#define PKBZ_MAGIC1 'K'
#define PKBZ_MAGIC2 'B'
#define PKBZ_MAGIC3 'Z'
#define PKBZ_VERSION 1u
#define PKBZ_HDR_FIXED 36u

struct PkbzHeader {
    unsigned int version;
    unsigned int width, height;
    unsigned int screen_x, screen_y, screen_w, screen_h;
    std::string  source;
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
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

inline std::string pkbz_encode(const PkbzHeader &h, const unsigned short *pixels)
{
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
    pkbz_put_u32(out, (unsigned int)h.source.size());
    out.append(h.source);
    out.append((const char *)pixels, (size_t)h.width * h.height * 2);
    return out;
}

inline bool pkbz_decode_header(const std::string &blob, PkbzHeader &h,
                               size_t &pixel_offset)
{
    if (blob.size() < PKBZ_HDR_FIXED)
        return false;
    const unsigned char *p = (const unsigned char *)blob.data();
    if (p[0] != PKBZ_MAGIC0 || p[1] != PKBZ_MAGIC1
        || p[2] != PKBZ_MAGIC2 || p[3] != PKBZ_MAGIC3)
        return false;
    h.version  = pkbz_get_u32(p + 4);
    if (h.version != PKBZ_VERSION)
        return false;
    h.width    = pkbz_get_u32(p + 8);
    h.height   = pkbz_get_u32(p + 12);
    h.screen_x = pkbz_get_u32(p + 16);
    h.screen_y = pkbz_get_u32(p + 20);
    h.screen_w = pkbz_get_u32(p + 24);
    h.screen_h = pkbz_get_u32(p + 28);
    unsigned int slen = pkbz_get_u32(p + 32);
    if (blob.size() < PKBZ_HDR_FIXED + slen)
        return false;
    h.source.assign(blob, PKBZ_HDR_FIXED, slen);
    pixel_offset = PKBZ_HDR_FIXED + slen;
    if (h.width == 0 || h.height == 0)
        return false;
    if (blob.size() - pixel_offset < (size_t)h.width * h.height * 2)
        return false;
    return true;
}

inline unsigned short pkbz_rgb565(int r, int g, int b)
{
    return (unsigned short)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#endif
