#ifndef PIKO_BEZEL_BAKE_H
#define PIKO_BEZEL_BAKE_H

#include <FL/Fl_Image.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_JPEG_Image.H>

#include <cstring>
#include <string>
#include <vector>

#include "bezel_db.h"
#include "bezel_format.h"

#define BEZEL_HOLE_ALPHA 128
#define BEZEL_MASTER_W 960
#define BEZEL_MASTER_H 720

struct BakedBezel {
    int width, height;
    int screen_x, screen_y, screen_w, screen_h;
    std::vector<unsigned char> rgb;
};

inline Fl_Image *bezel_load_image(const std::string &path)
{
    if (path.empty())
        return 0;
    std::string lower = path;
    for (size_t i = 0; i < lower.size(); i++)
        if (lower[i] >= 'A' && lower[i] <= 'Z') lower[i] = (char)(lower[i] + 32);

    Fl_Image *im = 0;
    if (lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".jpg") == 0))
        im = new Fl_JPEG_Image(path.c_str());
    else if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, ".jpeg") == 0)
        im = new Fl_JPEG_Image(path.c_str());
    else
        im = new Fl_PNG_Image(path.c_str());

    if (im && im->w() > 0 && im->d() >= 3)
        return im;
    delete im;
    return 0;
}

inline void bezel_sample(const Fl_Image *im, int x0, int y0, int x1, int y1,
                         int out[4])
{
    int w = im->w(), h = im->h(), d = im->d();
    long acc[4] = { 0, 0, 0, 0 };
    long n = 0;

    if (x0 < 0) x0 = 0; if (x1 > w) x1 = w;
    if (y0 < 0) y0 = 0; if (y1 > h) y1 = h;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    if (x0 >= w) x0 = w - 1, x1 = w;
    if (y0 >= h) y0 = h - 1, y1 = h;

    const unsigned char *base = (const unsigned char *)im->data()[0];
    for (int y = y0; y < y1; y++) {
        const unsigned char *px = base + ((size_t)y * w + x0) * d;
        for (int x = x0; x < x1; x++, px += d) {
            acc[0] += px[0];
            acc[1] += px[1];
            acc[2] += px[2];
            acc[3] += (d == 4) ? px[3] : 255;
            n++;
        }
    }
    out[0] = (int)(acc[0] / n);
    out[1] = (int)(acc[1] / n);
    out[2] = (int)(acc[2] / n);
    out[3] = (int)(acc[3] / n);
}

inline bool bezel_bake(const BezelPreset &p, int W, int H,
                       BakedBezel &out, std::string &err)
{
    Fl_Image *im = bezel_load_image(p.image);
    if (!im) {
        err = "cannot load overlay image: " + p.image;
        return false;
    }

    double src_ar = (double)im->w() / im->h();
    double dst_ar = (double)W / H;
    double vis_w = 1.0, vis_h = 1.0;
    if (src_ar > dst_ar)
        vis_w = dst_ar / src_ar;
    else
        vis_h = src_ar / dst_ar;

    double step_x = vis_w * im->w() / W;
    double step_y = vis_h * im->h() / H;
    double base_x = (1.0 - vis_w) * 0.5 * im->w();
    double base_y = (1.0 - vis_h) * 0.5 * im->h();

    out.width = W;
    out.height = H;
    out.rgb.assign((size_t)W * H * 3, 0);

    int min_x = W, min_y = H, max_x = -1, max_y = -1;
    int shift_x = 0, shift_y = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < H; y++) {
            double sy = base_y + (double)(y - shift_y) * step_y;
            for (int x = 0; x < W; x++) {
                double sx = base_x + (double)(x - shift_x) * step_x;
                int s[4];

                unsigned char *o = &out.rgb[((size_t)y * W + x) * 3];
                if (pass == 1
                    && (sx + step_x <= 0 || sx >= im->w()
                        || sy + step_y <= 0 || sy >= im->h())) {
                    o[0] = o[1] = o[2] = 0;
                    continue;
                }
                bezel_sample(im, (int)sx, (int)sy,
                             (int)(sx + step_x + 0.5), (int)(sy + step_y + 0.5), s);
                if (s[3] < BEZEL_HOLE_ALPHA) {
                    if (x < min_x) min_x = x;
                    if (y < min_y) min_y = y;
                    if (x > max_x) max_x = x;
                    if (y > max_y) max_y = y;
                    if (pass == 1)
                        o[0] = o[1] = o[2] = 0;
                    continue;
                }
                if (pass == 0)
                    continue;
                o[0] = (unsigned char)(s[0] * s[3] / 255);
                o[1] = (unsigned char)(s[1] * s[3] / 255);
                o[2] = (unsigned char)(s[2] * s[3] / 255);
            }
        }

        if (max_x < min_x || max_y < min_y) {
            err = "no transparent screen area in " + bezel_basename(p.image);
            delete im;
            return false;
        }
        if (pass == 0) {
            shift_x = (W - (max_x - min_x + 1)) / 2 - min_x;
            shift_y = (H - (max_y - min_y + 1)) / 2 - min_y;
            min_x = W; min_y = H; max_x = -1; max_y = -1;
        }
    }

    delete im;

    out.screen_x = min_x;
    out.screen_y = min_y;
    out.screen_w = max_x - min_x + 1;
    out.screen_h = max_y - min_y + 1;

    for (int y = out.screen_y; y < out.screen_y + out.screen_h; y++) {
        unsigned char *row = &out.rgb[((size_t)y * W + out.screen_x) * 3];
        memset(row, 0, (size_t)out.screen_w * 3);
    }

    err.clear();
    return true;
}

inline std::string bezel_to_pkbz(const BakedBezel &b, const std::string &source)
{
    std::vector<unsigned short> px((size_t)b.width * b.height);
    for (size_t i = 0; i < px.size(); i++) {
        const unsigned char *p = &b.rgb[i * 3];
        px[i] = pkbz_rgb565(p[0], p[1], p[2]);
    }
    PkbzHeader h;
    h.version  = PKBZ_VERSION;
    h.width    = (unsigned)b.width;
    h.height   = (unsigned)b.height;
    h.screen_x = (unsigned)b.screen_x;
    h.screen_y = (unsigned)b.screen_y;
    h.screen_w = (unsigned)b.screen_w;
    h.screen_h = (unsigned)b.screen_h;
    h.source   = source;
    return pkbz_encode(h, &px[0]);
}

#endif
