
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
#define CELL_H            (THUMB_SIZE + CELL_PAD + 16)
#define SCROLLBAR_RESERVE 20

#define CTRL_H            64
#define TOOLBAR_H         32
#define PREVIEW_SIZE      56
#define BROWSE_W          100

struct Mode {
    const char *label;
    const char *prefix;
};

static const Mode MODES[] = {
    { "Mosaic",   "img-mosaic:"    },
    { "Centered", "img-centered:"  },
    { "Stretch",  "img-stretched:" },
    { "Fill",     "img-filled:"    },
};
#define N_MODES (int)(sizeof(MODES) / sizeof(MODES[0]))

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

    Fl_Shared_Image *thumb = Fl_Shared_Image::get(path.c_str(), tw, th);
    orig->release();
    return thumb;
}

WallpaperPanel::WallpaperPanel(int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H),
      selected_mode_(3),
      preview_(NULL), status_(NULL), scroll_(NULL)
{
    piko_register_images();

    box(FL_FLAT_BOX);

    scan();
    read_current_spec();

    begin();

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

void WallpaperPanel::build_grid(void)
{
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

    thumb_refs_.resize(entries_.size());

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

    if (W != old_w) {
        scroll_->scroll_to(0, 0);
        build_grid();
        redraw();
    }
}

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
            rename(tmp.c_str(), file.c_str());
        }
    }

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

void WallpaperPanel::mode_cb(Fl_Widget *, void *data)
{
    ModeRef *r = (ModeRef *)data;

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
