/*
 * panel-wallpaper.cxx -- "Set Wallpaper" as a reusable panel.
 *
 * The body of what used to be mb-wallpaper-picker.cxx, moved into an
 * Fl_Group subclass so that the standalone binary and piko-settings run
 * the same code rather than two copies of it. See panel-wallpaper.H for
 * what is unchanged, and panels/piko-panel.H for the contract.
 *
 * The file-scope globals the old picker used (g_entries, g_selected_mode,
 * g_current_path, g_preview, g_status) are members now -- a panel can be
 * constructed more than once in a process, since piko-settings builds a
 * fresh one every time its row is tapped.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "panel-wallpaper.H"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Scroll.H>
#include <FL/x.H>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#define THUMB_SIZE        100
#define CELL_PAD          14
#define CELL_W            (THUMB_SIZE + CELL_PAD)
#define CELL_H            (THUMB_SIZE + CELL_PAD + 16)   /* + label line */
#define SCROLLBAR_RESERVE 20

#define CTRL_H            64        /* mode selector + preview swatch */
#define TOOLBAR_H         32        /* status line + Browse */
#define PREVIEW_SIZE      56
#define BROWSE_W          100

struct Mode {
    const char *label;
    const char *prefix;   /* spec prefix understood by mbdesktop_bg_parse_spec() */
};

static const Mode MODES[] = {
    { "Mosaic",   "img-mosaic:"    },
    { "Centered", "img-centered:"  },
    { "Stretch",  "img-stretched:" },
    { "Fill",     "img-filled:"    },
};
#define N_MODES (int)(sizeof(MODES) / sizeof(MODES[0]))

/* ── helpers, unchanged from the standalone picker ──────────────────────── */

/* home directory, matching mb_util_get_homedir()'s exact fallback */
static std::string homedir(void)
{
    const char *h = getenv("HOME");

    if (h == NULL) {
        const char *t = getenv("TMPDIR");
        return t ? t : "/tmp";
    }
    return h;
}

static bool has_image_ext(const char *name)
{
    static const char *exts[] = { ".png", ".jpg", ".jpeg", ".bmp", NULL };
    size_t len = strlen(name);

    for (int i = 0; exts[i]; i++) {
        size_t elen = strlen(exts[i]);
        if (len > elen && !strcasecmp(name + len - elen, exts[i]))
            return true;
    }
    return false;
}

static std::string basename_of(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/* Decode once, keep only an aspect-correct scaled copy. */
static Fl_Image *load_thumb(const std::string &path, int max_dim)
{
    Fl_Shared_Image *orig = Fl_Shared_Image::get(path.c_str());

    if (!orig || orig->w() <= 0 || orig->h() <= 0) {
        if (orig)
            orig->release();
        return NULL;
    }

    int tw, th;
    if (orig->w() >= orig->h()) {
        tw = max_dim;
        th = orig->h() * max_dim / orig->w();
    } else {
        th = max_dim;
        tw = orig->w() * max_dim / orig->h();
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    /* get(path, tw, th) reuses the decode we just did (still cached, since
     * we hold a reference) rather than re-reading the file. */
    Fl_Shared_Image *thumb = Fl_Shared_Image::get(path.c_str(), tw, th);
    orig->release();
    return thumb;
}

/* ── construction ──────────────────────────────────────────────────────── */

WallpaperPanel::WallpaperPanel(int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H),
      selected_mode_(3),            /* default to Fill */
      preview_(NULL), status_(NULL), scroll_(NULL)
{
    /* Exactly once per process, however many panels and hosts there are --
     * FLTK's own is not idempotent. See panels/piko-panel.H. */
    piko_register_images();

    box(FL_FLAT_BOX);

    scan();
    read_current_spec();

    begin();

    /* Mode selector, left of a preview swatch showing what the modes are
     * about to be applied to. */
    int preview_x   = X + W - PREVIEW_SIZE - 8;
    int mode_area_w = preview_x - X - 8;
    int mode_btn_w  = mode_area_w / N_MODES;
    if (mode_btn_w < 8)
        mode_btn_w = 8;

    mode_refs_.resize(N_MODES);
    for (int i = 0; i < N_MODES; i++) {
        mode_refs_[i].panel = this;
        mode_refs_[i].mode  = i;

        Fl_Button *b = new Fl_Button(X + 4 + i * mode_btn_w, Y + 4,
                                      mode_btn_w - 4, CTRL_H - 8,
                                      MODES[i].label);
        b->type(FL_RADIO_BUTTON);
        b->value(i == selected_mode_);
        b->selection_color(fl_rgb_color(0x34, 0x65, 0xa4));
        b->clear_visible_focus();
        b->callback(mode_cb, &mode_refs_[i]);
    }

    preview_ = new Fl_Box(preview_x, Y + 4, PREVIEW_SIZE, PREVIEW_SIZE);
    preview_->box(FL_DOWN_BOX);

    /* Status line + Browse. */
    Fl_Button *browse = new Fl_Button(X + W - BROWSE_W - 8, Y + CTRL_H + 4,
                                       BROWSE_W, TOOLBAR_H - 8, "Browse...");
    browse->clear_visible_focus();
    browse->callback(browse_cb, this);

    status_ = new Fl_Box(X + 8, Y + CTRL_H + 4,
                          W - 8 - BROWSE_W - 16, TOOLBAR_H - 8, "");
    status_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    if (!current_path_.empty())
        status_->copy_label(("Current: " + basename_of(current_path_) +
                              " (" + MODES[selected_mode_].label + ")").c_str());
    else
        status_->copy_label("Tap an image below, or Browse for one");

    int scroll_y = Y + CTRL_H + TOOLBAR_H;
    scroll_ = new Fl_Scroll(X, scroll_y, W, H - CTRL_H - TOOLBAR_H);
    scroll_->box(FL_FLAT_BOX);

    build_grid();

    end();

    resizable(scroll_);
    update_preview();
}

Fl_Group *piko_panel_wallpaper_new(int X, int Y, int W, int H)
{
    return new WallpaperPanel(X, Y, W, H);
}

/* ── scanning ──────────────────────────────────────────────────────────── */

bool WallpaperPanel::entry_less(const Entry &a, const Entry &b)
{
    return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
}

void WallpaperPanel::scan(void)
{
    const std::string dirs[] = {
        std::string("/usr/share/backgrounds"),
        homedir() + "/.matchbox/backgrounds",
    };

    for (size_t d = 0; d < sizeof(dirs) / sizeof(dirs[0]); d++) {
        DIR *dp = opendir(dirs[d].c_str());
        struct dirent *de;

        if (!dp)
            continue;

        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            if (!has_image_ext(de->d_name))
                continue;

            Entry e;
            e.path  = dirs[d] + "/" + de->d_name;
            e.label = de->d_name;
            entries_.push_back(e);
        }

        closedir(dp);
    }

    std::sort(entries_.begin(), entries_.end(), entry_less);
}

/* The currently persisted spec, so the panel opens showing what is already
 * set rather than a blank slate. */
bool WallpaperPanel::read_current_spec(void)
{
    std::string spec_path = homedir() + "/.matchbox/wallpaper";
    FILE *fp = fopen(spec_path.c_str(), "r");
    char buf[1024];

    if (!fp)
        return false;

    bool ok = false;
    if (fgets(buf, sizeof(buf), fp)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';

        std::string spec(buf);
        for (int i = 0; i < N_MODES; i++) {
            size_t plen = strlen(MODES[i].prefix);
            if (spec.compare(0, plen, MODES[i].prefix) == 0) {
                current_path_  = spec.substr(plen);
                selected_mode_ = i;
                ok = true;
                break;
            }
        }
    }

    fclose(fp);
    return ok;
}

/* ── the thumbnail grid ────────────────────────────────────────────────── */

void WallpaperPanel::build_grid(void)
{
    /*
     * Children are placed relative to the scroll's own origin, not (0,0).
     * The standalone picker placed them at absolute (0,0) while its
     * Fl_Scroll started 96px down the window, which only ever worked
     * because Fl_Scroll re-derives its scroll extents from the bounding
     * box of whatever it contains. Embedded, the panel does not start at
     * the top of the window at all, so the sloppy version would put the
     * grid behind piko-settings' header.
     */
    int ox = scroll_->x();
    int oy = scroll_->y();
    int avail_w = scroll_->w();

    scroll_->clear();
    thumb_refs_.clear();
    scroll_->begin();

    if (entries_.empty()) {
        Fl_Box *msg = new Fl_Box(ox + 8, oy + 8, avail_w - 16, 40,
            "No images in /usr/share/backgrounds or ~/.matchbox/backgrounds "
            "-- use Browse to pick one.");
        msg->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        scroll_->end();
        return;
    }

    int columns = (avail_w - SCROLLBAR_RESERVE) / CELL_W;
    if (columns < 1)
        columns = 1;
    int rows = ((int)entries_.size() + columns - 1) / columns;

    thumb_refs_.resize(entries_.size());   /* sized before any pointer is taken */

    Fl_Group *grid = new Fl_Group(ox, oy,
                                   columns * CELL_W + CELL_PAD,
                                   rows * CELL_H + CELL_PAD);
    grid->begin();

    for (size_t i = 0; i < entries_.size(); i++) {
        int col = (int)i % columns;
        int row = (int)i / columns;
        int bx  = ox + CELL_PAD / 2 + col * CELL_W;
        int by  = oy + CELL_PAD / 2 + row * CELL_H;

        thumb_refs_[i].panel = this;
        thumb_refs_[i].index = (int)i;

        Fl_Button *b = new Fl_Button(bx, by, THUMB_SIZE, THUMB_SIZE + 16,
                                      entries_[i].label.c_str());
        b->box(FL_THIN_UP_BOX);
        b->labelsize(11);
        b->align(FL_ALIGN_IMAGE_OVER_TEXT | FL_ALIGN_INSIDE
                  | FL_ALIGN_CLIP | FL_ALIGN_WRAP);
        b->clear_visible_focus();

        Fl_Image *thumb = load_thumb(entries_[i].path, THUMB_SIZE);
        if (thumb)
            b->image(thumb);
        else
            b->label("(failed to load)");

        b->callback(thumb_cb, &thumb_refs_[i]);
    }

    grid->end();
    scroll_->end();
}

void WallpaperPanel::resize(int X, int Y, int W, int H)
{
    int old_w = w();

    Fl_Group::resize(X, Y, W, H);

    /* Column count is a function of the width, so a rotation has to
     * rebuild. The standalone picker built its grid once at startup and
     * did not survive flipd turning the screen; it does now. */
    if (W != old_w) {
        scroll_->scroll_to(0, 0);
        build_grid();
        redraw();
    }
}

/* ── preview and applying ──────────────────────────────────────────────── */

void WallpaperPanel::update_preview(void)
{
    preview_->image(current_path_.empty()
                     ? NULL : load_thumb(current_path_, PREVIEW_SIZE));
    preview_->redraw();
}

void WallpaperPanel::apply(const std::string &path, const std::string &label)
{
    std::string spec = MODES[selected_mode_].prefix + path;
    std::string home = homedir();

    if (!home.empty()) {
        std::string dir  = home + "/.matchbox";
        std::string file = dir + "/wallpaper";
        std::string tmp  = file + ".tmp";

        mkdir(dir.c_str(), 0755);

        FILE *fp = fopen(tmp.c_str(), "w");
        if (fp) {
            fprintf(fp, "%s\n", spec.c_str());
            fclose(fp);
            rename(tmp.c_str(), file.c_str());   /* atomic -- never a torn file */
        }
    }

    /* _MB_WALLPAPER_SPEC: see panel-wallpaper.H for why this exists. */
    static Atom atom_wallpaper = 0, atom_utf8 = 0;
    if (!atom_wallpaper) {
        atom_wallpaper = XInternAtom(fl_display, "_MB_WALLPAPER_SPEC", False);
        atom_utf8      = XInternAtom(fl_display, "UTF8_STRING", False);
    }
    XChangeProperty(fl_display, RootWindow(fl_display, fl_screen),
                     atom_wallpaper, atom_utf8, 8, PropModeReplace,
                     (const unsigned char *)spec.c_str(), (int)spec.size());
    XFlush(fl_display);

    current_path_ = path;
    update_preview();
    status_->copy_label(("Applied: " + label +
                          " (" + MODES[selected_mode_].label + ")").c_str());
}

/* ── callbacks ─────────────────────────────────────────────────────────── */

void WallpaperPanel::mode_cb(Fl_Widget *, void *data)
{
    ModeRef *r = (ModeRef *)data;

    /* The buttons are FL_RADIO_BUTTON, so FLTK has already turned the
     * others off -- this is only bookkeeping for the next apply. */
    r->panel->selected_mode_ = r->mode;
}

void WallpaperPanel::thumb_cb(Fl_Widget *, void *data)
{
    ThumbRef *r = (ThumbRef *)data;
    const Entry &e = r->panel->entries_[r->index];

    r->panel->apply(e.path, e.label);
}

void WallpaperPanel::browse_cb(Fl_Widget *, void *data)
{
    WallpaperPanel *p = (WallpaperPanel *)data;

    std::string start = p->current_path_.empty()
        ? "/usr/share/backgrounds/"
        : p->current_path_.substr(0, p->current_path_.find_last_of('/') + 1);

    const char *pick = fl_file_chooser("Choose a wallpaper image",
                                        "Image Files (*.{png,jpg,jpeg,bmp})",
                                        start.c_str(), 0);
    if (!pick)
        return;

    std::string path(pick);
    p->apply(path, basename_of(path));
}
