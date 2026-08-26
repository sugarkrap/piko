#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pikoemu_ui.h"

#define UI_TTL_MS    10000
#define UI_SLIDE_MS  420
#define UI_MARGIN    8
#define UI_GAP       4
#define UI_PAD       12
#define UI_TEXT_MAX  128

struct ui_piece {
    char name[16];
    int w, h, l, t, r, b;
    int flags;
    const unsigned short *px;
    const unsigned char *alpha;
};

struct ui_glyph {
    unsigned int cp;
    int ax, ay, w, h, bx, by, advance;
};

struct ui_font {
    unsigned char *blob;
    int size, line, ascent;
    int aw, ah;
    const unsigned char *atlas;
    struct ui_glyph *glyph;
    int count;
};

struct ui_note {
    char text[UI_TEXT_MAX];
    unsigned short *px;
    int w, h;
    unsigned int born;
    int slot;
    int at, target;
    int shown;
    int dying;
    int rect_x, rect_y, rect_w, rect_h;
    int had_rect;
};

static SDL_Surface *dst;
static ui_restore_fn restore_cb;
static void *restore_user;

static unsigned char *art_blob;
static struct ui_piece art[8];
static int art_count;
static struct ui_font font_bold;
static struct ui_font font_small;

static struct ui_note notes[PIKOEMU_UI_MAX];

static void push_clipped(int x, int y, int w, int h);
static int note_count;
static int rotated;

static unsigned int rd32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned char *slurp(const char *path, size_t *len)
{
    unsigned char *buf;
    long n;
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (unsigned char *)malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static int load_art(const char *path)
{
    size_t len = 0;
    unsigned char *b = slurp(path, &len);
    unsigned int n;
    int i;

    if (b == NULL)
        return 0;
    if (len < 12 || memcmp(b, "PKUI", 4) != 0 || rd32(b + 4) != 2) {
        free(b);
        return 0;
    }
    n = rd16(b + 8);
    if (n > 8) n = 8;
    if (len < 12 + (size_t)n * 40) {
        free(b);
        return 0;
    }
    for (i = 0; i < (int)n; i++) {
        const unsigned char *e = b + 12 + i * 40;
        const unsigned char *blob = b + 12 + n * 40 + rd32(e + 32);
        snprintf(art[i].name, sizeof(art[i].name), "%s", (const char *)e);
        art[i].w = (int)rd16(e + 16);
        art[i].h = (int)rd16(e + 18);
        art[i].l = (int)rd16(e + 20);
        art[i].t = (int)rd16(e + 22);
        art[i].r = (int)rd16(e + 24);
        art[i].b = (int)rd16(e + 26);
        art[i].flags = (int)rd16(e + 28);
        art[i].px = (const unsigned short *)blob;
        art[i].alpha = (art[i].flags & 1)
                       ? blob + (size_t)art[i].w * art[i].h * 2 : NULL;
    }
    art_blob = b;
    art_count = (int)n;
    return 1;
}

static const struct ui_piece *piece(const char *name)
{
    int i;
    for (i = 0; i < art_count; i++)
        if (strcmp(art[i].name, name) == 0)
            return &art[i];
    return NULL;
}

static int load_font(const char *path, struct ui_font *f)
{
    size_t len = 0;
    unsigned char *b = slurp(path, &len);
    int i;

    if (b == NULL)
        return 0;
    if (len < 22 || memcmp(b, "PKFN", 4) != 0 || rd32(b + 4) != 1) {
        free(b);
        return 0;
    }
    f->size   = (int)rd16(b + 8);
    f->line   = (int)rd16(b + 10);
    f->ascent = (int)rd16(b + 12);
    f->aw     = (int)rd16(b + 16);
    f->ah     = (int)rd16(b + 18);
    f->count  = (int)rd16(b + 20);
    if (len < 22 + (size_t)f->count * 18 + (size_t)f->aw * f->ah) {
        free(b);
        return 0;
    }
    f->glyph = (struct ui_glyph *)malloc(sizeof(*f->glyph) * (size_t)f->count);
    if (f->glyph == NULL) {
        free(b);
        return 0;
    }
    for (i = 0; i < f->count; i++) {
        const unsigned char *e = b + 22 + i * 18;
        f->glyph[i].cp      = rd32(e);
        f->glyph[i].ax      = (int)rd16(e + 4);
        f->glyph[i].ay      = (int)rd16(e + 6);
        f->glyph[i].w       = (int)rd16(e + 8);
        f->glyph[i].h       = (int)rd16(e + 10);
        f->glyph[i].bx      = (short)rd16(e + 12);
        f->glyph[i].by      = (short)rd16(e + 14);
        f->glyph[i].advance = (short)rd16(e + 16);
    }
    f->atlas = b + 22 + (size_t)f->count * 18;
    f->blob = b;
    return 1;
}

static const struct ui_glyph *glyph_for(const struct ui_font *f, unsigned int cp)
{
    int i;
    for (i = 0; i < f->count; i++)
        if (f->glyph[i].cp == cp)
            return &f->glyph[i];
    return NULL;
}

int ui_load(const char *dir)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/notify.pkui", dir);
    if (!load_art(path))
        return 0;
    snprintf(path, sizeof(path), "%s/sans-bold.pkfn", dir);
    if (!load_font(path, &font_bold))
        return 0;
    snprintf(path, sizeof(path), "%s/sans.pkfn", dir);
    return load_font(path, &font_small);
}

void ui_attach(SDL_Surface *surface, ui_restore_fn restore, void *user)
{
    dst = surface;
    restore_cb = restore;
    restore_user = user;
}

void ui_set_rotated(int r)
{
    int i;

    if (r == rotated)
        return;
    for (i = 0; i < note_count; i++) {
        if (notes[i].had_rect && restore_cb != NULL)
            restore_cb(restore_user, notes[i].rect_x, notes[i].rect_y,
                       notes[i].rect_w, notes[i].rect_h);
        free(notes[i].px);
    }
    if (note_count > 0 && dst != NULL)
        SDL_UpdateRect(dst, 0, 0, 0, 0);
    note_count = 0;
    rotated = r;
}

int ui_rotated(void)
{
    return rotated;
}

static void draw_string(struct ui_note *n, const struct ui_font *f,
                        int pen, int base, int limit, const char *text,
                        int cr, int cg, int cb)
{
    for (; *text != '\0'; text++) {
        const struct ui_glyph *g = glyph_for(f, (unsigned char)*text);
        int gx, gy;

        if (g == NULL)
            continue;
        if (pen + g->bx + g->w > limit)
            break;
        for (gy = 0; gy < g->h; gy++) {
            int py = base + g->by + gy;
            if (py < 0 || py >= n->h)
                continue;
            for (gx = 0; gx < g->w; gx++) {
                int px = pen + g->bx + gx;
                unsigned int a = f->atlas[(size_t)(g->ay + gy) * f->aw + g->ax + gx];
                unsigned short d;
                int r, gr, b;

                if (a == 0 || px < 0 || px >= n->w)
                    continue;
                d = n->px[(size_t)py * n->w + px];
                r  = ((d >> 11) & 0x1F) << 3;
                gr = ((d >> 5) & 0x3F) << 2;
                b  = (d & 0x1F) << 3;
                r  += (cr - r) * (int)a / 255;
                gr += (cg - gr) * (int)a / 255;
                b  += (cb - b) * (int)a / 255;
                n->px[(size_t)py * n->w + px] =
                    (unsigned short)(((r & 0xF8) << 8) | ((gr & 0xFC) << 3) | (b >> 3));
            }
        }
        pen += g->advance;
    }
}

static void draw_icon(struct ui_note *n, const struct ui_piece *ic, int ix, int iy)
{
    int x, y;

    for (y = 0; y < ic->h; y++) {
        int py = iy + y;
        if (py < 0 || py >= n->h)
            continue;
        for (x = 0; x < ic->w; x++) {
            int px = ix + x;
            unsigned int a = ic->alpha ? ic->alpha[(size_t)y * ic->w + x] : 255;
            unsigned short sv, dv;
            int r, g, b;

            if (a == 0 || px < 0 || px >= n->w)
                continue;
            sv = ic->px[(size_t)y * ic->w + x];
            if (a == 255) {
                n->px[(size_t)py * n->w + px] = sv;
                continue;
            }
            dv = n->px[(size_t)py * n->w + px];
            r = ((dv >> 11) & 0x1F) << 3;
            g = ((dv >> 5) & 0x3F) << 2;
            b = (dv & 0x1F) << 3;
            r += ((((sv >> 11) & 0x1F) << 3) - r) * (int)a / 255;
            g += ((((sv >> 5) & 0x3F) << 2) - g) * (int)a / 255;
            b += (((sv & 0x1F) << 3) - b) * (int)a / 255;
            n->px[(size_t)py * n->w + px] =
                (unsigned short)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        }
    }
}

static void compose(struct ui_note *n, const char *title, const char *desc,
                    const char *icon)
{
    const struct ui_piece *bar = piece("bar");
    const struct ui_piece *ic = (icon != NULL) ? piece(icon) : NULL;
    int limit, x, y;

    n->w = PIKOEMU_UI_MAX_W;
    n->h = bar->h;
    n->px = (unsigned short *)malloc((size_t)n->w * n->h * 2);
    if (n->px == NULL)
        return;

    for (y = 0; y < n->h; y++) {
        for (x = 0; x < n->w; x++) {
            int sx;
            if (x < bar->l)
                sx = x;
            else if (x >= n->w - bar->r)
                sx = bar->w - (n->w - x);
            else
                sx = bar->l + (x - bar->l) % (bar->w - bar->l - bar->r);
            n->px[(size_t)y * n->w + x] = bar->px[(size_t)y * bar->w + sx];
        }
    }

    limit = n->w - UI_PAD;
    if (ic != NULL) {
        int ix = n->w - UI_PAD - ic->w;
        draw_icon(n, ic, ix, (n->h - ic->h) / 2);
        limit = ix - UI_PAD;
    }

    if (desc != NULL && desc[0] != '\0') {
        draw_string(n, &font_bold, UI_PAD, n->h / 2 - 3, limit, title,
                    0xf2, 0xf5, 0xf8);
        draw_string(n, &font_small, UI_PAD, n->h / 2 + font_small.ascent + 2,
                    limit, desc, 0xa8, 0xb4, 0xc4);
    } else {
        draw_string(n, &font_bold, UI_PAD, (n->h + font_bold.ascent) / 2 - 2,
                    limit, title, 0xf2, 0xf5, 0xf8);
    }
}

static int span(const struct ui_note *n)
{
    return rotated ? n->h : n->h;
}

static void slot_target(int slot, const struct ui_note *n, int *tx, int *ty)
{
    int step = span(n) + UI_GAP;
    if (rotated) {
        *tx = dst->w - UI_MARGIN - n->h - slot * step;
        *ty = UI_MARGIN;
    } else {
        *tx = dst->w - UI_MARGIN - n->w;
        *ty = dst->h - UI_MARGIN - n->h - slot * step;
    }
}

static void note_rect(const struct ui_note *n, int at, int *x, int *y, int *w, int *h)
{
    int tx, ty;
    slot_target(n->slot, n, &tx, &ty);
    if (rotated) {
        *w = n->h;
        *h = n->w;
        *x = at;
        *y = ty;
    } else {
        *w = n->w;
        *h = n->h;
        *x = tx;
        *y = at;
    }
}

static void draw_note(const struct ui_note *n)
{
    unsigned short *out = (unsigned short *)dst->pixels;
    int pitch = dst->pitch / 2;
    int x, y, w, h, sx, sy;

    if (n->px == NULL)
        return;
    note_rect(n, n->at, &x, &y, &w, &h);
    for (sy = 0; sy < h; sy++) {
        int dy = y + sy;
        if (dy < 0 || dy >= dst->h)
            continue;
        for (sx = 0; sx < w; sx++) {
            int dx = x + sx;
            unsigned short v;
            if (dx < 0 || dx >= dst->w)
                continue;
            if (rotated)
                v = n->px[(size_t)sx * n->w + (n->w - 1 - sy)];
            else
                v = n->px[(size_t)sy * n->w + sx];
            out[(size_t)dy * pitch + dx] = v;
        }
    }
}

static void reflow(void)
{
    int i;
    for (i = 0; i < note_count; i++) {
        int tx, ty;
        notes[i].slot = i;
        slot_target(i, &notes[i], &tx, &ty);
        notes[i].target = rotated ? tx : ty;
    }
}

static void drop(int idx)
{
    free(notes[idx].px);
    memmove(&notes[idx], &notes[idx + 1],
            sizeof(notes[0]) * (size_t)(note_count - idx - 1));
    note_count--;
    reflow();
}

void ui_notify(const char *title, const char *desc, const char *icon,
               unsigned int now)
{
    struct ui_note n;

    if (dst == NULL || art_count == 0 || font_bold.blob == NULL)
        return;

    memset(&n, 0, sizeof(n));
    snprintf(n.text, sizeof(n.text), "%s", title);
    compose(&n, title, desc, icon);
    if (n.px == NULL)
        return;

    if (note_count == PIKOEMU_UI_MAX) {
        struct ui_note *old = &notes[note_count - 1];
        if (old->had_rect && restore_cb != NULL) {
            restore_cb(restore_user, old->rect_x, old->rect_y,
                       old->rect_w, old->rect_h);
            push_clipped(old->rect_x, old->rect_y, old->rect_w, old->rect_h);
        }
        drop(note_count - 1);
    }

    memmove(&notes[1], &notes[0], sizeof(notes[0]) * (size_t)note_count);
    notes[0] = n;
    note_count++;
    reflow();

    notes[0].born = now;
    notes[0].at = rotated ? dst->w : dst->h;
    notes[0].shown = 0;
}

int ui_notify_pen(int x, int y)
{
    int i;
    for (i = 0; i < note_count; i++) {
        int nx, ny, nw, nh;
        note_rect(&notes[i], notes[i].at, &nx, &ny, &nw, &nh);
        if (x >= nx && x < nx + nw && y >= ny && y < ny + nh) {
            notes[i].dying = 1;
            return 1;
        }
    }
    return 0;
}

int ui_notify_active(void)
{
    return note_count;
}

static int ease(int from, int to, unsigned int elapsed)
{
    double t = (double)elapsed / UI_SLIDE_MS;
    double e;
    if (t >= 1.0)
        return to;
    e = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    return from + (int)((to - from) * e + (to > from ? 0.5 : -0.5));
}

static int overlaps(int ax, int ay, int aw, int ah,
                    int bx, int by, int bw, int bh)
{
    return !(ax >= bx + bw || ax + aw <= bx || ay >= by + bh || ay + ah <= by);
}

static void push_clipped(int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;

    if (dst == NULL)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    if (x1 <= x || y1 <= y)
        return;
    SDL_UpdateRect(dst, x, y, x1 - x, y1 - y);
}

static void add_rect(SDL_Rect *out, int *n, int max, int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;

    if (out == NULL || *n >= max || dst == NULL)
        return;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    if (x1 <= x || y1 <= y)
        return;
    out[*n].x = (Sint16)x;
    out[*n].y = (Sint16)y;
    out[*n].w = (Uint16)(x1 - x);
    out[*n].h = (Uint16)(y1 - y);
    (*n)++;
}

int ui_notify_repaint_over(int x, int y, int w, int h, SDL_Rect *out, int max)
{
    int i, n = 0;

    if (dst == NULL || note_count == 0)
        return 0;
    if (SDL_MUSTLOCK(dst) && SDL_LockSurface(dst) < 0)
        return 0;
    for (i = note_count - 1; i >= 0; i--) {
        struct ui_note *note = &notes[i];
        int nx, ny, nw, nh;
        note_rect(note, note->at, &nx, &ny, &nw, &nh);
        if (!overlaps(nx, ny, nw, nh, x, y, w, h))
            continue;
        note->rect_x = nx; note->rect_y = ny;
        note->rect_w = nw; note->rect_h = nh;
        note->had_rect = 1;
        draw_note(note);
        add_rect(out, &n, max, nx, ny, nw, nh);
    }
    if (SDL_MUSTLOCK(dst))
        SDL_UnlockSurface(dst);
    return n;
}

int ui_notify_tick(unsigned int now)
{
    SDL_Rect rects[PIKOEMU_UI_MAX * 2];
    int nr = 0, i, locked = 0;

    if (dst == NULL || note_count == 0)
        return 0;

    for (i = note_count - 1; i >= 0; i--) {
        struct ui_note *n = &notes[i];
        int prev = n->at;
        int off = rotated ? dst->w : dst->h;
        int nx, ny, nw, nh;

        if (!n->dying && now - n->born >= UI_TTL_MS) {
            n->dying = 1;
            n->born = now;
        }

        if (n->dying) {
            n->at = ease(n->target, off, now - n->born);
            if (n->at == off) {
                if (n->had_rect) {
                    restore_cb(restore_user, n->rect_x, n->rect_y,
                               n->rect_w, n->rect_h);
                    add_rect(rects, &nr, PIKOEMU_UI_MAX * 2,
                             n->rect_x, n->rect_y, n->rect_w, n->rect_h);
                }
                drop(i);
                continue;
            }
        } else if (!n->shown) {
            n->at = ease(off, n->target, now - n->born);
            if (n->at == n->target)
                n->shown = 1;
        } else if (n->at != n->target) {
            n->at = n->target;
        }

        if (n->at == prev && n->had_rect)
            continue;

        if (!locked) {
            if (SDL_MUSTLOCK(dst) && SDL_LockSurface(dst) < 0)
                return 0;
            locked = 1;
        }
        if (n->had_rect) {
            restore_cb(restore_user, n->rect_x, n->rect_y,
                       n->rect_w, n->rect_h);
            add_rect(rects, &nr, PIKOEMU_UI_MAX * 2,
                     n->rect_x, n->rect_y, n->rect_w, n->rect_h);
        }
        note_rect(n, n->at, &nx, &ny, &nw, &nh);
        n->rect_x = nx; n->rect_y = ny; n->rect_w = nw; n->rect_h = nh;
        n->had_rect = 1;
        draw_note(n);
        add_rect(rects, &nr, PIKOEMU_UI_MAX * 2, nx, ny, nw, nh);
    }

    if (locked && SDL_MUSTLOCK(dst))
        SDL_UnlockSurface(dst);
    if (nr > 0)
        SDL_UpdateRects(dst, nr, rects);
    return nr > 0;
}
