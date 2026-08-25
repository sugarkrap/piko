#ifndef PIKOEMU_CONFIG_H
#define PIKOEMU_CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void pikoemu_unescape(const char *in, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (in[i] != '\0' && o + 1 < cap) {
        if (in[i] == '%' && in[i + 1] && in[i + 2]) {
            int hi = pikoemu_hexval(in[i + 1]);
            int lo = pikoemu_hexval(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[o++] = (char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        out[o++] = in[i++];
    }
    out[o] = '\0';
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

static void pikoemu_zroot(const char *rom, char *out, size_t cap)
{
    if (strncmp(rom, "/mnt/card/", 10) == 0)
        snprintf(out, cap, "/mnt/card/.zaurus");
    else if (strncmp(rom, "/mnt/cf/", 8) == 0)
        snprintf(out, cap, "/mnt/cf/.zaurus");
    else
        snprintf(out, cap, "/usr/local/.zaurus");
}

static void pikoemu_apply_option(struct pikoemu_cfg *c, const char *key, const char *raw)
{
    char val[PIKOEMU_MAXVAL];
    pikoemu_unescape(raw, val, sizeof(val));

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

static void pikoemu_parse_options(struct pikoemu_cfg *c, char *opts)
{
    char *p = opts;
    while (p != NULL && *p != '\0') {
        char *comma = strchr(p, ',');
        char *eq;
        if (comma != NULL) *comma = '\0';
        eq = strchr(p, '=');
        if (eq != NULL) {
            *eq = '\0';
            pikoemu_apply_option(c, p, eq + 1);
        }
        p = (comma != NULL) ? comma + 1 : NULL;
    }
}

static int pikoemu_find_line(const char *path, const char *want,
                             char *opts, size_t cap)
{
    char line[PIKOEMU_MAXLINE];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (f == NULL)
        return 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *fields[6];
        int n = 0;
        char *p = line;
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        while (n < 6) {
            char *bar = strchr(p, '|');
            fields[n++] = p;
            if (bar == NULL) break;
            *bar = '\0';
            p = bar + 1;
        }
        if (n < 1 || strcmp(fields[0], want) != 0)
            continue;
        opts[0] = '\0';
        if (n > 5)
            snprintf(opts, cap, "%s", fields[5]);
        found = 1;
        break;
    }
    fclose(f);
    return found;
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
        if (pikoemu_find_line(path, want, opts, sizeof(opts))) {
            pikoemu_parse_options(c, opts);
            if (c->has_bezel)
                return;
        }
    }
    if (pikoemu_find_line(path, "@global", opts, sizeof(opts)))
        pikoemu_parse_options(c, opts);
}

static int pikoemu_load(const char *rom, struct pikoemu_cfg *c)
{
    char zroot[64];
    char path[PIKOEMU_MAXVAL];
    char line[PIKOEMU_MAXLINE];
    const char *env = getenv("EMULATION_CFG");
    FILE *f;

    memset(c, 0, sizeof(*c));
    snprintf(c->rom, sizeof(c->rom), "%s", rom);

    if (env != NULL && env[0] != '\0') {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        pikoemu_zroot(rom, zroot, sizeof(zroot));
        snprintf(path, sizeof(path), "%s/emulation.cfg", zroot);
    }

    f = fopen(path, "r");
    if (f == NULL)
        return 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *fields[6];
        int n = 0;
        char *p = line;
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        while (n < 6) {
            char *bar = strchr(p, '|');
            fields[n++] = p;
            if (bar == NULL) break;
            *bar = '\0';
            p = bar + 1;
        }
        if (n < 1 || strcmp(fields[0], rom) != 0)
            continue;

        if (n > 1) snprintf(c->machine, sizeof(c->machine), "%s", fields[1]);
        if (n > 2) snprintf(c->backend, sizeof(c->backend), "%s", fields[2]);
        if (n > 5) pikoemu_parse_options(c, fields[5]);
        c->found = 1;
        break;
    }

    fclose(f);
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
