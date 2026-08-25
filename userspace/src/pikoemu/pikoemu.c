#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "SDL.h"

#include "config.h"
#include "pikoemu_proto.h"
#include "pikovideo.h"
#include "pikoemu_ui.h"

#define PIKOEMU_PATHMAX 1024
#define PIKOEMU_BEZEL_CACHE_REV "2"
#define PIKOEMU_SCAN_STEP     4
#define PIKOEMU_WATCH_MS      8000
#define PIKOEMU_CHECK_MS      400

struct bezel_image {
    int w, h;
    int sx, sy, sw, sh;
    unsigned short *px;
};

static SDL_Surface *screen;
static int bezel_canvas_w, bezel_canvas_h;
static unsigned char *shared;
static size_t shared_bytes;
static size_t frame_bytes;
static pid_t child = -1;
static const unsigned short *ui_bezel;

static void trace_out(const char *msg)
{
    fprintf(stderr, "pikoemu: %s\n", msg);
}

static void usage(void)
{
    fprintf(stderr, "usage: pikoemu [--dry-run] <rom> -- <command> [args...]\n");
}

static void print_plan(const struct pikoemu_cfg *c, enum pikovideo_mode m, char **cmd)
{
    int i;
    printf("rom:      %s\n", c->rom);
    printf("entry:    %s\n", c->found ? "found in emulation.cfg" : "NOT FOUND (defaults)");
    printf("title:    %s\n", c->title[0] ? c->title : "(none)");
    printf("backend:  %s\n", c->backend[0] ? c->backend : "(none)");
    printf("video:    %s (%dx%d)\n", pikovideo_mode_key(m), c->screen_w, c->screen_h);
    printf("canvas:   %dx%d\n", c->canvas_w, c->canvas_h);
    printf("offset:   %d,%d\n", (c->screen_w - c->canvas_w) / 2,
                                (c->screen_h - c->canvas_h) / 2);
    printf("rotate:   %d\n", c->rotate);
    if (c->canvas_w == c->screen_w && c->canvas_h == c->screen_h)
        printf("bezel:    not needed, canvas fills the screen\n");
    else if (!c->has_bezel)
        printf("bezel:    none\n");
    else if (c->bezel_image[0])
        printf("bezel:    %s\n", c->bezel_image);
    else
        printf("bezel:    #%02X%02X%02X\n", c->bezel_r, c->bezel_g, c->bezel_b);
    printf("command: ");
    for (i = 0; cmd != NULL && cmd[i] != NULL; i++)
        printf(" %s", cmd[i]);
    printf("\n");
}

static void cleanup(void)
{
    if (screen != NULL) {
        SDL_Quit();
        screen = NULL;
    }
    pikovideo_restore();
}

static void on_signal(int sig)
{
    if (child > 0)
        kill(child, SIGTERM);
    cleanup();
    _exit(128 + sig);
}

static unsigned int pkbz_u32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int pkbz_read(const char *path, struct bezel_image *b, char *stamp,
                     size_t stamp_cap)
{
    unsigned char hdr[36];
    unsigned int slen;
    size_t pixels;
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return 0;
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)
        || memcmp(hdr, "PKBZ", 4) != 0 || pkbz_u32(hdr + 4) != 1) {
        fclose(f);
        return 0;
    }
    b->w  = (int)pkbz_u32(hdr + 8);
    b->h  = (int)pkbz_u32(hdr + 12);
    b->sx = (int)pkbz_u32(hdr + 16);
    b->sy = (int)pkbz_u32(hdr + 20);
    b->sw = (int)pkbz_u32(hdr + 24);
    b->sh = (int)pkbz_u32(hdr + 28);
    slen  = pkbz_u32(hdr + 32);
    if (b->w <= 0 || b->h <= 0 || b->sw <= 0 || b->sh <= 0) {
        fclose(f);
        return 0;
    }
    if (stamp != NULL) {
        size_t n = (slen < stamp_cap - 1) ? slen : stamp_cap - 1;
        if (fread(stamp, 1, n, f) != n) { fclose(f); return 0; }
        stamp[n] = '\0';
        if (slen > n && fseek(f, (long)(slen - n), SEEK_CUR) != 0) {
            fclose(f);
            return 0;
        }
    } else if (fseek(f, (long)slen, SEEK_CUR) != 0) {
        fclose(f);
        return 0;
    }
    pixels = (size_t)b->w * b->h;
    b->px = (unsigned short *)malloc(pixels * 2);
    if (b->px == NULL || fread(b->px, 2, pixels, f) != pixels) {
        free(b->px);
        b->px = NULL;
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static void pkbz_put_u32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static int pkbz_write(const char *path, const struct bezel_image *b,
                      const char *stamp)
{
    unsigned char hdr[36];
    size_t pixels = (size_t)b->w * b->h;
    size_t slen = strlen(stamp);
    char tmp[PIKOEMU_PATHMAX + 8];
    FILE *f;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (f == NULL)
        return 0;
    memcpy(hdr, "PKBZ", 4);
    pkbz_put_u32(hdr + 4, 1);
    pkbz_put_u32(hdr + 8, (unsigned int)b->w);
    pkbz_put_u32(hdr + 12, (unsigned int)b->h);
    pkbz_put_u32(hdr + 16, (unsigned int)b->sx);
    pkbz_put_u32(hdr + 20, (unsigned int)b->sy);
    pkbz_put_u32(hdr + 24, (unsigned int)b->sw);
    pkbz_put_u32(hdr + 28, (unsigned int)b->sh);
    pkbz_put_u32(hdr + 32, (unsigned int)slen);
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)
        || fwrite(stamp, 1, slen, f) != slen
        || fwrite(b->px, 2, pixels, f) != pixels) {
        fclose(f);
        unlink(tmp);
        return 0;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return 0;
    }
    return 1;
}

static int find_master(const struct pikoemu_cfg *c, char *out, size_t cap)
{
    char zroot[64];
    const char *roots[4];
    struct stat st;

    size_t i, n = 0;

    if (!c->has_bezel || c->bezel_image[0] == '\0')
        return 0;
    if (c->bezel_image[0] == '/') {
        snprintf(out, cap, "%s", c->bezel_image);
        return stat(out, &st) == 0;
    }

    pikoemu_zroot(c->rom, zroot, sizeof(zroot));
    roots[n++] = zroot;
    if (strcmp(zroot, "/usr/local/.zaurus") != 0)
        roots[n++] = "/usr/local/.zaurus";
    if (strcmp(zroot, "/mnt/card/.zaurus") != 0)
        roots[n++] = "/mnt/card/.zaurus";
    if (strcmp(zroot, "/mnt/cf/.zaurus") != 0)
        roots[n++] = "/mnt/cf/.zaurus";

    for (i = 0; i < n; i++) {
        snprintf(out, cap, "%s/bezels/%s.pkbz", roots[i], c->bezel_image);
        if (stat(out, &st) == 0)
            return 1;
    }
    return 0;
}

static int rotate_master(struct bezel_image *b)
{
    int w = b->w, h = b->h;
    int sx = b->sx, sy = b->sy, sw = b->sw, sh = b->sh;
    unsigned short *dst = (unsigned short *)malloc((size_t)w * h * 2);
    int X, Y;

    if (dst == NULL)
        return 0;
    for (Y = 0; Y < w; Y++) {
        unsigned short *row = dst + (size_t)Y * h;
        for (X = 0; X < h; X++)
            row[X] = b->px[(size_t)X * w + (w - 1 - Y)];
    }
    free(b->px);
    b->px = dst;
    b->w = h;
    b->h = w;
    b->sx = sy;
    b->sy = w - sx - sw;
    b->sw = sh;
    b->sh = sw;
    return 1;
}

static int resize_bezel(const struct pikoemu_cfg *c,
                        const struct bezel_image *src, struct bezel_image *dst)
{
    double f = (double)bezel_canvas_w / src->sw;
    double t = (double)bezel_canvas_h / src->sh;
    double ox, oy, step;
    int *x0, *x1;
    int x, y;

    if (t > f) f = t;
    t = (double)c->screen_w / src->w; if (t > f) f = t;
    t = (double)c->screen_h / src->h; if (t > f) f = t;

    step = 1.0 / f;
    ox = c->screen_w / 2.0 - (src->sx + src->sw / 2.0) * f;
    oy = c->screen_h / 2.0 - (src->sy + src->sh / 2.0) * f;

    dst->w = c->screen_w;
    dst->h = c->screen_h;
    dst->sw = (int)(src->sw * f + 0.5);
    dst->sh = (int)(src->sh * f + 0.5);
    dst->sx = (c->screen_w - dst->sw) / 2;
    dst->sy = (c->screen_h - dst->sh) / 2;
    dst->px = (unsigned short *)malloc((size_t)dst->w * dst->h * 2);
    if (dst->px == NULL)
        return 0;

    x0 = (int *)malloc((size_t)dst->w * sizeof(int) * 2);
    if (x0 == NULL) {
        free(dst->px);
        dst->px = NULL;
        return 0;
    }
    x1 = x0 + dst->w;
    for (x = 0; x < dst->w; x++) {
        double s0 = ((double)x - ox) * step;
        int a = (int)s0, e = (int)(s0 + step + 0.5);
        if (e <= a) e = a + 1;
        if (a < 0) a = 0;
        if (e > src->w) e = src->w;
        x0[x] = a;
        x1[x] = (e > a) ? e : a;
    }

    for (y = 0; y < dst->h; y++) {
        double s0 = ((double)y - oy) * step;
        int ya = (int)s0, yb = (int)(s0 + step + 0.5);
        unsigned short *row = dst->px + (size_t)y * dst->w;

        if (yb <= ya) yb = ya + 1;
        if (ya < 0) ya = 0;
        if (yb > src->h) yb = src->h;
        if (yb <= ya) {
            memset(row, 0, (size_t)dst->w * 2);
            continue;
        }
        for (x = 0; x < dst->w; x++) {
            unsigned int r = 0, g = 0, bl = 0, n = 0;
            int sx, sy;

            if (x1[x] <= x0[x]) {
                row[x] = 0;
                continue;
            }
            for (sy = ya; sy < yb; sy++) {
                const unsigned short *s = src->px + (size_t)sy * src->w;
                for (sx = x0[x]; sx < x1[x]; sx++) {
                    unsigned short p = s[sx];
                    r  += (p >> 11) & 0x1F;
                    g  += (p >> 5) & 0x3F;
                    bl += p & 0x1F;
                    n++;
                }
            }
            row[x] = (unsigned short)(((r / n) << 11) | ((g / n) << 5) | (bl / n));
        }
    }
    free(x0);
    return 1;
}

static int load_bezel(const struct pikoemu_cfg *c, struct bezel_image *b)
{
    char master[PIKOEMU_PATHMAX];
    char cache[PIKOEMU_PATHMAX];
    char dir[PIKOEMU_PATHMAX];
    char want[128], got[160];
    struct bezel_image raw;
    struct stat st;
    char *slash;

    if (!find_master(c, master, sizeof(master)))
        return 0;
    if (stat(master, &st) != 0)
        return 0;

    snprintf(want, sizeof(want), "%s:%lu:%lu:%dx%d:%dx%d", PIKOEMU_BEZEL_CACHE_REV,
             (unsigned long)st.st_size, (unsigned long)st.st_mtime,
             c->screen_w, c->screen_h, bezel_canvas_w, bezel_canvas_h);

    snprintf(dir, sizeof(dir), "%s", master);
    slash = strrchr(dir, '/');
    if (slash != NULL)
        *slash = '\0';
    snprintf(cache, sizeof(cache), "%s/cache/%s-%dx%d-%dx%d.pkbz", dir,
             c->bezel_image[0] == '/' ? "bezel" : c->bezel_image,
             c->screen_w, c->screen_h, bezel_canvas_w, bezel_canvas_h);

    got[0] = '\0';
    if (pkbz_read(cache, b, got, sizeof(got))
        && b->w == c->screen_w && b->h == c->screen_h
        && strcmp(got, want) == 0) {
        trace_out("bezel cache hit");
        return 1;
    }
    free(b->px);
    b->px = NULL;

    memset(&raw, 0, sizeof(raw));
    if (!pkbz_read(master, &raw, NULL, 0))
        return 0;
    if (bezel_canvas_w > bezel_canvas_h && raw.sh > raw.sw) {
        if (!rotate_master(&raw)) {
            free(raw.px);
            return 0;
        }
        trace_out("bezel rotated 90 CCW for a landscape canvas");
    }
    if (!resize_bezel(c, &raw, b)) {
        free(raw.px);
        return 0;
    }
    free(raw.px);

    snprintf(dir, sizeof(dir), "%s", cache);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
        mkdir(dir, 0755);
    }
    if (pkbz_write(cache, b, want))
        trace_out("bezel resized and cached");
    else
        trace_out("bezel resized, cache not written");
    return 1;
}

static void paint_bezel_image(const struct pikoemu_cfg *c,
                              const struct bezel_image *b)
{
    unsigned short *dst;
    int y, n = (c->screen_w < b->w) ? c->screen_w : b->w;

    if (SDL_MUSTLOCK(screen) && SDL_LockSurface(screen) < 0)
        return;
    dst = (unsigned short *)screen->pixels;
    for (y = 0; y < c->screen_h; y++) {
        unsigned short *row = dst + (size_t)y * (screen->pitch / 2);
        if (y >= b->h) {
            memset(row, 0, (size_t)c->screen_w * 2);
            continue;
        }
        memcpy(row, b->px + (size_t)y * b->w, (size_t)n * 2);
        if (n < c->screen_w)
            memset(row + n, 0, (size_t)(c->screen_w - n) * 2);
    }
    if (SDL_MUSTLOCK(screen))
        SDL_UnlockSurface(screen);
    SDL_UpdateRect(screen, 0, 0, 0, 0);
}

static void paint_bezel(const struct pikoemu_cfg *c, const struct bezel_image *b)
{
    Uint32 col;

    if (b != NULL && b->px != NULL) {
        paint_bezel_image(c, b);
        return;
    }
    col = c->has_bezel ? SDL_MapRGB(screen->format, c->bezel_r, c->bezel_g, c->bezel_b)
                       : SDL_MapRGB(screen->format, 0, 0, 0);
    SDL_FillRect(screen, NULL, col);
    SDL_UpdateRect(screen, 0, 0, 0, 0);
}

static void refit_bezel(struct pikoemu_cfg *c, struct bezel_image *b)
{
    ui_bezel = NULL;
    free(b->px);
    memset(b, 0, sizeof(*b));
    load_bezel(c, b);
    paint_bezel(c, b);
    if (b->px != NULL && b->w == c->screen_w)
        ui_bezel = b->px;
}

static int canvas_resized(struct pikoemu_cfg *c, int w, int h)
{
    char msg[96];

    if (w <= 0 || h <= 0 || (w == c->canvas_w && h == c->canvas_h))
        return 0;
    if ((size_t)w * h * 2 > frame_bytes) {
        snprintf(msg, sizeof(msg), "backend asked for %dx%d, too big for the buffer",
                 w, h);
        trace_out(msg);
        return 0;
    }
    snprintf(msg, sizeof(msg), "canvas %dx%d -> %dx%d",
             c->canvas_w, c->canvas_h, w, h);
    trace_out(msg);
    c->canvas_w = w;
    c->canvas_h = h;
    return 1;
}

static void restore_background(void *user, int x, int y, int w, int h)
{
    const struct pikoemu_cfg *c = (const struct pikoemu_cfg *)user;
    unsigned short *out = (unsigned short *)screen->pixels;
    int pitch = screen->pitch / 2;
    int dx = (c->screen_w - c->canvas_w) / 2;
    int dy = (c->screen_h - c->canvas_h) / 2;
    volatile unsigned int *front = (volatile unsigned int *)shared;
    const unsigned short *frame = (const unsigned short *)
        (shared + PIKOEMU_HDR_BYTES + (size_t)(*front % PIKOEMU_BUFFERS) * frame_bytes);
    int row, col;

    for (row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= screen->h)
            continue;
        for (col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= screen->w)
                continue;
            if (sx >= dx && sx < dx + c->canvas_w
                && sy >= dy && sy < dy + c->canvas_h) {
                out[(size_t)sy * pitch + sx] =
                    frame[(size_t)(sy - dy) * c->canvas_w + (sx - dx)];
            } else if (ui_bezel != NULL) {
                out[(size_t)sy * pitch + sx] =
                    ui_bezel[(size_t)sy * c->screen_w + sx];
            } else {
                out[(size_t)sy * pitch + sx] = 0;
            }
        }
    }
}

static int uniform_col(const unsigned short *f, int w, int h, int x)
{
    unsigned short v = f[x];
    int y;
    for (y = PIKOEMU_SCAN_STEP; y < h; y += PIKOEMU_SCAN_STEP)
        if (f[(size_t)y * w + x] != v)
            return 0;
    return 1;
}

static int uniform_row(const unsigned short *f, int w, int y)
{
    const unsigned short *row = f + (size_t)y * w;
    unsigned short v = row[0];
    int x;
    for (x = PIKOEMU_SCAN_STEP; x < w; x += PIKOEMU_SCAN_STEP)
        if (row[x] != v)
            return 0;
    return 1;
}

static int canvas_fill(const struct pikoemu_cfg *c, int *uw, int *uh)
{
    volatile unsigned int *front = (volatile unsigned int *)shared;
    const unsigned short *frame = (const unsigned short *)
        (shared + PIKOEMU_HDR_BYTES + (size_t)(*front % PIKOEMU_BUFFERS) * frame_bytes);
    int w = c->canvas_w, h = c->canvas_h;
    int left = 0, right = 0, top = 0, bottom = 0;

    while (left < w && uniform_col(frame, w, h, left))
        left += PIKOEMU_SCAN_STEP;
    if (left >= w)
        return 0;
    while (right < w - left && uniform_col(frame, w, h, w - 1 - right))
        right += PIKOEMU_SCAN_STEP;
    while (top < h && uniform_row(frame, w, top))
        top += PIKOEMU_SCAN_STEP;
    if (top >= h)
        return 0;
    while (bottom < h - top && uniform_row(frame, w, h - 1 - bottom))
        bottom += PIKOEMU_SCAN_STEP;

    *uw = w - left - right;
    *uh = h - top - bottom;
    return (*uw > 0 && *uh > 0);
}

static void blit_canvas(const struct pikoemu_cfg *c, int defer)
{
    int dx = (c->screen_w - c->canvas_w) / 2;
    int dy = (c->screen_h - c->canvas_h) / 2;
    int y;
    unsigned short *dst;
    const unsigned short *src;
    volatile unsigned int *front = (volatile unsigned int *)shared;
    unsigned int idx = *front % PIKOEMU_BUFFERS;

    src = (const unsigned short *)(shared + PIKOEMU_HDR_BYTES
                                   + (size_t)idx * frame_bytes);

    if (SDL_MUSTLOCK(screen) && SDL_LockSurface(screen) < 0)
        return;
    dst = (unsigned short *)screen->pixels;
    for (y = 0; y < c->canvas_h; y++)
        memcpy(dst + (size_t)(dy + y) * (screen->pitch / 2) + dx,
               src + (size_t)y * c->canvas_w,
               (size_t)c->canvas_w * 2);
    if (SDL_MUSTLOCK(screen))
        SDL_UnlockSurface(screen);
    if (!defer)
        SDL_UpdateRect(screen, dx, dy, c->canvas_w, c->canvas_h);
}

int main(int argc, char **argv)
{
    struct pikoemu_cfg cfg;
    struct bezel_image bezel;
    enum pikovideo_mode mode;
    const char *rom = NULL;
    char **cmd = NULL;
    int dry_run = 0;
    int bezel_needed;
    int sv[2];
    int fbfd;
    int screen_w, screen_h;
    int i, status = 0;
    char env_fb[32], env_sock[32], env_w[32], env_h[32];
    unsigned char inbuf[64];
    size_t inlen = 0;
    int ui_ready = 0;
    unsigned int last_check = 0;
    int warned = 0, c_landscape = 0, ready = 0;
    unsigned int ready_at = 0;

    memset(&bezel, 0, sizeof(bezel));

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--") == 0) {
            if (i + 1 < argc) cmd = &argv[i + 1];
            break;
        } else if (argv[i][0] == '-') {
            usage();
            return 2;
        } else if (rom == NULL) {
            rom = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if (rom == NULL) {
        usage();
        return 2;
    }

    pikovideo_set_trace(trace_out);
    pikoemu_load(rom, &cfg);

    mode = pikovideo_load_config();
    if (cfg.video_key[0] != '\0' && !pikovideo_mode_from_key(cfg.video_key, &mode))
        fprintf(stderr, "pikoemu: unknown video mode '%s', using %s\n",
                cfg.video_key, pikovideo_mode_key(mode));
    pikovideo_mode_size(mode, &screen_w, &screen_h);
    pikoemu_resolve(&cfg, screen_w, screen_h);
    bezel_canvas_w = cfg.canvas_w;
    bezel_canvas_h = cfg.canvas_h;

    if (dry_run) {
        print_plan(&cfg, mode, cmd);
        return 0;
    }
    if (cmd == NULL) {
        usage();
        return 2;
    }

    frame_bytes = (size_t)cfg.canvas_w * cfg.canvas_h * 2;
    shared_bytes = PIKOEMU_HDR_BYTES + frame_bytes * PIKOEMU_BUFFERS;
    {
        char tmpl[] = "/tmp/pikoemu-fb.XXXXXX";
        fbfd = mkstemp(tmpl);
        if (fbfd >= 0)
            unlink(tmpl);
    }
    if (fbfd < 0 || ftruncate(fbfd, (off_t)shared_bytes) < 0) {
        fprintf(stderr, "pikoemu: cannot create shared buffer: %s\n", strerror(errno));
        return 1;
    }
    shared = (unsigned char *)mmap(NULL, shared_bytes, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, fbfd, 0);
    if (shared == (unsigned char *)MAP_FAILED) {
        fprintf(stderr, "pikoemu: mmap failed: %s\n", strerror(errno));
        return 1;
    }
    memset(shared, 0, shared_bytes);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        fprintf(stderr, "pikoemu: socketpair failed: %s\n", strerror(errno));
        return 1;
    }

    if (!pikovideo_apply(mode, PIKOVIDEO_DRIVER_X11))
        fprintf(stderr, "pikoemu: video mode %s not applied, continuing\n",
                pikovideo_mode_key(mode));

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "pikoemu: SDL_Init: %s\n", SDL_GetError());
        pikovideo_restore();
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    screen = SDL_SetVideoMode(cfg.screen_w, cfg.screen_h, 16,
                              getenv("PIKOEMU_WINDOWED") ? 0 : SDL_FULLSCREEN);
    if (screen == NULL) {
        fprintf(stderr, "pikoemu: SDL_SetVideoMode: %s\n", SDL_GetError());
        cleanup();
        return 1;
    }
    bezel_needed = cfg.canvas_w != cfg.screen_w || cfg.canvas_h != cfg.screen_h;
    if (!bezel_needed) {
        trace_out("canvas fills the screen, no bezel needed");
    } else {
        if (load_bezel(&cfg, &bezel))
            printf("pikoemu: bezel %s %dx%d, screen %dx%d+%d+%d\n",
                   cfg.bezel_image, bezel.w, bezel.h,
                   bezel.sw, bezel.sh, bezel.sx, bezel.sy);
        else if (cfg.bezel_image[0] != '\0')
            fprintf(stderr, "pikoemu: no bezel named %s, painting black\n",
                    cfg.bezel_image);
        paint_bezel(&cfg, &bezel);
        if (bezel.px != NULL && bezel.w == cfg.screen_w)
            ui_bezel = bezel.px;
    }

    c_landscape = cfg.canvas_w > cfg.canvas_h;
    ui_ready = ui_load(PIKOEMU_UI_DIR);
    if (ui_ready)
        ui_attach(screen, restore_background, &cfg);
    else
        trace_out("no ui assets under " PIKOEMU_UI_DIR ", notifications disabled");

    child = fork();
    if (child < 0) {
        fprintf(stderr, "pikoemu: fork failed: %s\n", strerror(errno));
        cleanup();
        return 1;
    }
    if (child == 0) {
        int hi_fb = 30, hi_sock = 31;

        close(sv[0]);
        if (fbfd != hi_fb && dup2(fbfd, hi_fb) >= 0) {
            close(fbfd);
            fbfd = hi_fb;
        }
        if (sv[1] != hi_sock && dup2(sv[1], hi_sock) >= 0) {
            close(sv[1]);
            sv[1] = hi_sock;
        }
        snprintf(env_fb,   sizeof(env_fb),   "%d", fbfd);
        snprintf(env_sock, sizeof(env_sock), "%d", sv[1]);
        snprintf(env_w,    sizeof(env_w),    "%d", cfg.canvas_w);
        snprintf(env_h,    sizeof(env_h),    "%d", cfg.canvas_h);
        setenv("PIKOEMU_FB_FD", env_fb, 1);
        setenv("PIKOEMU_SOCK_FD", env_sock, 1);
        setenv("PIKOEMU_W", env_w, 1);
        setenv("PIKOEMU_H", env_h, 1);
        if (cfg.rotate)
            setenv("J2ME_GP2X_REVERSE", "1", 1);
        execvp(cmd[0], cmd);
        fprintf(stderr, "pikoemu: cannot run %s: %s\n", cmd[0], strerror(errno));
        _exit(127);
    }

    close(sv[1]);
    for (;;) {
        SDL_Event ev;
        struct pollfd pfd;
        int done = 0;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                unsigned char rec[PIKOEMU_EVENT_BYTES];
                unsigned sym = (unsigned)ev.key.keysym.sym;
                unsigned mod = (unsigned)ev.key.keysym.mod;

                rec[0] = (ev.type == SDL_KEYDOWN) ? PIKOEMU_MSG_KEYDOWN
                                                  : PIKOEMU_MSG_KEYUP;
                rec[1] = (unsigned char)(sym & 0xFF);
                rec[2] = (unsigned char)((sym >> 8) & 0xFF);
                rec[3] = (unsigned char)((sym >> 16) & 0xFF);
                rec[4] = (unsigned char)((sym >> 24) & 0xFF);
                rec[5] = (unsigned char)(mod & 0xFF);
                rec[6] = (unsigned char)((mod >> 8) & 0xFF);
                rec[7] = (unsigned char)((mod >> 16) & 0xFF);
                rec[8] = (unsigned char)((mod >> 24) & 0xFF);
                if (write(sv[0], rec, sizeof(rec)) < 0 && errno == EPIPE)
                    done = 1;
            } else if (ev.type == SDL_MOUSEBUTTONDOWN
                       || ev.type == SDL_MOUSEBUTTONUP
                       || ev.type == SDL_MOUSEMOTION) {
                unsigned char rec[PIKOEMU_EVENT_BYTES];
                int dx = (cfg.screen_w - cfg.canvas_w) / 2;
                int dy = (cfg.screen_h - cfg.canvas_h) / 2;
                int px, py;
                unsigned ux, uy;

                if (ev.type == SDL_MOUSEMOTION) {
                    rec[0] = PIKOEMU_MSG_PENMOVE;
                    px = ev.motion.x;
                    py = ev.motion.y;
                } else {
                    rec[0] = (ev.type == SDL_MOUSEBUTTONDOWN) ? PIKOEMU_MSG_PENDOWN
                                                              : PIKOEMU_MSG_PENUP;
                    px = ev.button.x;
                    py = ev.button.y;
                }
                if (ui_ready && ui_notify_pen(px, py))
                    continue;
                px -= dx;
                py -= dy;
                if (px < 0) px = 0;
                if (py < 0) py = 0;
                if (px > cfg.canvas_w - 1) px = cfg.canvas_w - 1;
                if (py > cfg.canvas_h - 1) py = cfg.canvas_h - 1;
                ux = (unsigned)px;
                uy = (unsigned)py;
                rec[1] = (unsigned char)(ux & 0xFF);
                rec[2] = (unsigned char)((ux >> 8) & 0xFF);
                rec[3] = (unsigned char)((ux >> 16) & 0xFF);
                rec[4] = (unsigned char)((ux >> 24) & 0xFF);
                rec[5] = (unsigned char)(uy & 0xFF);
                rec[6] = (unsigned char)((uy >> 8) & 0xFF);
                rec[7] = (unsigned char)((uy >> 16) & 0xFF);
                rec[8] = (unsigned char)((uy >> 24) & 0xFF);
                if (write(sv[0], rec, sizeof(rec)) < 0 && errno == EPIPE)
                    done = 1;
            } else if (ev.type == SDL_QUIT) {
                kill(child, SIGTERM);
                done = 1;
            }
        }
        if (done)
            break;

        pfd.fd = sv[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 10) > 0) {
            ssize_t n = read(sv[0], inbuf + inlen, sizeof(inbuf) - inlen);

            if (n > 0) {
                size_t i = 0;
                int want_blit = 0;

                inlen += (size_t)n;
                while (i < inlen) {
                    if (inbuf[i] == PIKOEMU_MSG_READY) {
                        if (!ready) {
                            ready = 1;
                            ready_at = SDL_GetTicks();
                            trace_out("applet drew its first frame");
                        }
                        i++;
                    } else if (inbuf[i] == PIKOEMU_MSG_ROTATE) {
                        if (inlen - i < PIKOEMU_ROT_BYTES)
                            break;
                        if (ui_ready)
                            ui_set_rotated(inbuf[i + 1] != 0);
                        i += PIKOEMU_ROT_BYTES;
                    } else if (inbuf[i] == PIKOEMU_MSG_SIZE) {
                        int nw, nh;
                        if (inlen - i < PIKOEMU_SIZE_BYTES)
                            break;
                        nw = inbuf[i + 1] | (inbuf[i + 2] << 8);
                        nh = inbuf[i + 3] | (inbuf[i + 4] << 8);
                        i += PIKOEMU_SIZE_BYTES;
                        if (canvas_resized(&cfg, nw, nh) && bezel_needed)
                            refit_bezel(&cfg, &bezel);
                    } else {
                        i++;
                    }
                    want_blit = 1;
                }
                if (i == 0 && inlen == sizeof(inbuf))
                    inlen = 0;
                else {
                    memmove(inbuf, inbuf + i, inlen - i);
                    inlen -= i;
                }
                if (want_blit) {
                    SDL_Rect rects[PIKOEMU_UI_MAX + 1];
                    int cx = (cfg.screen_w - cfg.canvas_w) / 2;
                    int cy = (cfg.screen_h - cfg.canvas_h) / 2;
                    int nr = 1;

                    blit_canvas(&cfg, 1);
                    rects[0].x = (Sint16)cx;
                    rects[0].y = (Sint16)cy;
                    rects[0].w = (Uint16)cfg.canvas_w;
                    rects[0].h = (Uint16)cfg.canvas_h;
                    if (ui_ready)
                        nr += ui_notify_repaint_over(cx, cy, cfg.canvas_w,
                                                     cfg.canvas_h, &rects[1],
                                                     PIKOEMU_UI_MAX);
                    SDL_UpdateRects(screen, nr, rects);
                }
            } else if (n == 0) {
                break;
            } else if (errno != EINTR && errno != EAGAIN) {
                break;
            }
        }
        if (ui_ready) {
            unsigned int now = SDL_GetTicks();
            int uw, uh;

            if (ready && !warned && now - ready_at < PIKOEMU_WATCH_MS
                && now - last_check >= PIKOEMU_CHECK_MS) {
                last_check = now;
                if (c_landscape && canvas_fill(&cfg, &uw, &uh)
                    && uw < cfg.canvas_w * 9 / 10 && uw <= uh) {
                    ui_notify("Applet may be rotated",
                              "Click here for more info", "calendar", now);
                    warned = 1;
                }
            }
            ui_notify_tick(now);
        }

        if (waitpid(child, &status, WNOHANG) == child) {
            child = -1;
            break;
        }
    }

    if (child > 0) {
        waitpid(child, &status, 0);
        child = -1;
    }
    cleanup();
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
