#ifndef PIKOEMU_CONFIG_H
#define PIKOEMU_CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pikorom.h"

#define PIKOEMU_MAXLINE 4096
#define PIKOEMU_MAXVAL  512

struct pikoemu_cfg {
    char  rom[PIKOEMU_MAXVAL];
    char  machine[64];
    char  backend[64];
    char  title[PIKOEMU_MAXVAL];
    char  video_key[32];
    int   canvas_w, canvas_h;
    int   screen_w, screen_h;
    int   rotate;
    int   has_bezel;
    unsigned char bezel_r, bezel_g, bezel_b;
    char  bezel_image[PIKOEMU_MAXVAL];
    int   found;
};

static int pikoemu_hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

struct pikoemu_named_color { const char *name; unsigned char r, g, b; };

static const struct pikoemu_named_color pikoemu_colors[] = {
    { "black",   0x00, 0x00, 0x00 },
    { "white",   0xFF, 0xFF, 0xFF },
    { "red",     0xFF, 0x00, 0x00 },
    { "green",   0x00, 0xFF, 0x00 },
    { "blue",    0x00, 0x00, 0xFF },
    { "cyan",    0x00, 0xFF, 0xFF },
    { "magenta", 0xFF, 0x00, 0xFF },
    { "yellow",  0xFF, 0xFF, 0x00 },
    { "orange",  0xFF, 0x80, 0x00 },
    { "grey",    0x80, 0x80, 0x80 },
    { "gray",    0x80, 0x80, 0x80 },
    { NULL, 0, 0, 0 }
};

static int pikoemu_parse_color(const char *v, unsigned char *r, unsigned char *g, unsigned char *b)
{
    int i;
    if (v[0] == '#') {
        int c[6], n;
        for (n = 0; n < 6; n++) {
            c[n] = pikoemu_hexval(v[1 + n]);
            if (c[n] < 0) return 0;
        }
        if (v[7] != '\0') return 0;
        *r = (unsigned char)((c[0] << 4) | c[1]);
        *g = (unsigned char)((c[2] << 4) | c[3]);
        *b = (unsigned char)((c[4] << 4) | c[5]);
        return 1;
    }
    for (i = 0; pikoemu_colors[i].name != NULL; i++) {
        if (strcmp(v, pikoemu_colors[i].name) == 0) {
            *r = pikoemu_colors[i].r;
            *g = pikoemu_colors[i].g;
            *b = pikoemu_colors[i].b;
            return 1;
        }
    }
    return 0;
}

static void pikoemu_apply_option(struct pikoemu_cfg *c, const char *key, const char *raw)
{
    char val[PIKOEMU_MAXVAL];
    pikorom_option_unescape(raw, val, sizeof(val));

    if (strcmp(key, "title") == 0) {
        snprintf(c->title, sizeof(c->title), "%s", val);
    } else if (strcmp(key, "rotate") == 0) {
        c->rotate = (strcmp(val, "1") == 0);
    } else if (strcmp(key, "canvas") == 0) {
        int w = 0, h = 0;
        if (sscanf(val, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            c->canvas_w = w;
            c->canvas_h = h;
        }
    } else if (strcmp(key, "video") == 0) {
        snprintf(c->video_key, sizeof(c->video_key), "%s", val);
    } else if (strcmp(key, "bezel") == 0) {
        if (val[0] == '/') {
            snprintf(c->bezel_image, sizeof(c->bezel_image), "%s", val);
            c->has_bezel = 1;
        } else if (strcmp(val, "none") == 0) {
            c->has_bezel = 0;
        } else if (pikoemu_parse_color(val, &c->bezel_r, &c->bezel_g, &c->bezel_b)) {
            c->bezel_image[0] = '\0';
            c->has_bezel = 1;
        } else if (val[0] != '\0') {
            snprintf(c->bezel_image, sizeof(c->bezel_image), "%s", val);
            c->has_bezel = 1;
        }
    }
}

static const char *const pikoemu_option_keys[] = {
    "title", "rotate", "canvas", "video", "bezel", NULL
};

static void pikoemu_parse_options(struct pikoemu_cfg *c, const char *opts)
{
    int i;
    for (i = 0; pikoemu_option_keys[i] != NULL; i++) {
        char raw[PIKOEMU_MAXVAL];
        if (pikorom_option_get(opts, pikoemu_option_keys[i], raw, sizeof(raw)))
            pikoemu_apply_option(c, pikoemu_option_keys[i], raw);
    }
}

static void pikoemu_resolve_bezel(struct pikoemu_cfg *c)
{
    const char *root = getenv("PIKOEMU_ROOT_CFG");
    char path[PIKOEMU_MAXVAL];
    char opts[PIKOEMU_MAXLINE];
    char want[PIKOEMU_MAXVAL];

    if (c->has_bezel)
        return;

    snprintf(path, sizeof(path), "%s",
             (root && root[0]) ? root : "/usr/local/.zaurus/emulation.cfg");

    if (c->backend[0] != '\0') {
        snprintf(want, sizeof(want), "@backend:%s", c->backend);
        if (pikorom_entry_lookup(path, want, NULL, 0, NULL, 0, opts, sizeof(opts))) {
            pikoemu_parse_options(c, opts);
            if (c->has_bezel)
                return;
        }
    }
    if (pikorom_entry_lookup(path, "@global", NULL, 0, NULL, 0, opts, sizeof(opts)))
        pikoemu_parse_options(c, opts);
}

static int pikoemu_load(const char *rom, struct pikoemu_cfg *c)
{
    char path[PIKOEMU_MAXVAL];
    char opts[PIKOEMU_MAXLINE];
    const char *env = getenv("EMULATION_CFG");

    memset(c, 0, sizeof(*c));
    snprintf(c->rom, sizeof(c->rom), "%s", rom);

    if (env != NULL && env[0] != '\0')
        snprintf(path, sizeof(path), "%s", env);
    else if (!pikorom_cfg_path_for(rom, path, sizeof(path)))
        return 0;

    if (pikorom_entry_lookup(path, rom, c->machine, sizeof(c->machine),
                             c->backend, sizeof(c->backend), opts, sizeof(opts))) {
        pikoemu_parse_options(c, opts);
        c->found = 1;
    }

    pikoemu_resolve_bezel(c);
    return c->found;
}

static void pikoemu_resolve(struct pikoemu_cfg *c, int screen_w, int screen_h)
{
    c->screen_w = screen_w;
    c->screen_h = screen_h;
    if (c->canvas_w <= 0 || c->canvas_h <= 0) {
        c->canvas_w = 320;
        c->canvas_h = 240;
    }
    if (c->canvas_h > c->canvas_w) {
        int t = c->canvas_w;
        c->canvas_w = c->canvas_h;
        c->canvas_h = t;
    }
    if (c->canvas_w > c->screen_w) c->canvas_w = c->screen_w;
    if (c->canvas_h > c->screen_h) c->canvas_h = c->screen_h;
}

#endif
