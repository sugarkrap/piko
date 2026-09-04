#ifndef PIKO_ASSET_FORMAT_H
#define PIKO_ASSET_FORMAT_H

#include <stddef.h>

#define PKBZ_MAGIC0 'P'
#define PKBZ_MAGIC1 'K'
#define PKBZ_MAGIC2 'B'
#define PKBZ_MAGIC3 'Z'
#define PKBZ_VERSION 2u
#define PKBZ_HDR_FIXED 44u

#define PKBG_MAGIC0 'P'
#define PKBG_MAGIC1 'K'
#define PKBG_MAGIC2 'B'
#define PKBG_MAGIC3 'G'
#define PKBG_VERSION 1u
#define PKBG_HDR_FIXED 20u

struct pkbz_head {
    unsigned int version;
    unsigned int width, height;
    unsigned int screen_x, screen_y, screen_w, screen_h;
    unsigned int offset_x, offset_y;
    unsigned int source_len;
    size_t       pixel_offset;
};

struct pkbg_head {
    unsigned int version;
    unsigned int width, height;
    unsigned int source_len;
    size_t       pixel_offset;
};

static unsigned int piko_asset_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int pkbz_parse_head(const unsigned char *p, size_t len, struct pkbz_head *out)
{
    if (p == 0 || out == 0 || len < PKBZ_HDR_FIXED)
        return 0;
    if (p[0] != PKBZ_MAGIC0 || p[1] != PKBZ_MAGIC1
        || p[2] != PKBZ_MAGIC2 || p[3] != PKBZ_MAGIC3)
        return 0;
    out->version    = piko_asset_u32(p + 4);
    if (out->version != PKBZ_VERSION)
        return 0;
    out->width      = piko_asset_u32(p + 8);
    out->height     = piko_asset_u32(p + 12);
    out->screen_x   = piko_asset_u32(p + 16);
    out->screen_y   = piko_asset_u32(p + 20);
    out->screen_w   = piko_asset_u32(p + 24);
    out->screen_h   = piko_asset_u32(p + 28);
    out->offset_x   = piko_asset_u32(p + 32);
    out->offset_y   = piko_asset_u32(p + 36);
    out->source_len = piko_asset_u32(p + 40);
    out->pixel_offset = (size_t)PKBZ_HDR_FIXED + out->source_len;
    if (out->width == 0 || out->height == 0
        || out->screen_w == 0 || out->screen_h == 0)
        return 0;
    return 1;
}

static size_t pkbz_colour_bytes(const struct pkbz_head *h)
{
    return (size_t)h->width * h->height * 2;
}

static size_t pkbz_alpha_bytes(const struct pkbz_head *h)
{
    return (size_t)h->width * h->height;
}

static int pkbg_parse_head(const unsigned char *p, size_t len, struct pkbg_head *out)
{
    if (p == 0 || out == 0 || len < PKBG_HDR_FIXED)
        return 0;
    if (p[0] != PKBG_MAGIC0 || p[1] != PKBG_MAGIC1
        || p[2] != PKBG_MAGIC2 || p[3] != PKBG_MAGIC3)
        return 0;
    out->version    = piko_asset_u32(p + 4);
    if (out->version != PKBG_VERSION)
        return 0;
    out->width      = piko_asset_u32(p + 8);
    out->height     = piko_asset_u32(p + 12);
    out->source_len = piko_asset_u32(p + 16);
    out->pixel_offset = (size_t)PKBG_HDR_FIXED + out->source_len;
    if (out->width == 0 || out->height == 0)
        return 0;
    return 1;
}

static size_t pkbg_colour_bytes(const struct pkbg_head *h)
{
    return (size_t)h->width * h->height * 2;
}

#endif
