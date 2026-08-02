/*
 * mb-wallpaper-picker -- a touch-friendly wallpaper picker for the classic
 * Matchbox desktop, built with FLTK.
 *
 * This replaces an earlier version written directly against libmb/Xlib
 * (see git history for userspace/src/matchbox-desktop-classic/src/
 * mb-wallpaper-picker.c). Moving it to FLTK trades hand-rolled drawing,
 * hit-testing and scrolling for real widgets -- Fl_Scroll gives the grid
 * a working scrollbar for free, and Fl_Button's built-in FL_RADIO_BUTTON
 * type replaces the manual "which mode is highlighted" bookkeeping. It
 * also adds a Browse button (Fl_File_Chooser) so a wallpaper does not have
 * to already live in one of the two scanned directories, and a small
 * preview swatch next to the mode selector so the mode buttons have
 * something concrete to apply to.
 *
 * Behaviour that is unchanged from the old picker, because matchbox-desktop
 * depends on it (see docs/HOWTO-MATCHBOX-DESKTOP.md, "Wallpaper: modes,
 * formats, and why it's cached raw"):
 *
 *   - scans /usr/share/backgrounds and $HOME/.matchbox/backgrounds for
 *     .png/.jpg/.jpeg/.bmp files and shows them as a tap-to-apply grid;
 *   - a 4-way mode selector (Mosaic/Centered/Stretch/Fill) that is
 *     remembered but only takes effect the next time something is applied,
 *     same as before;
 *   - applying writes $HOME/.matchbox/wallpaper (mode:filename, the file
 *     matchbox-desktop reads at startup) atomically, and sets the
 *     _MB_WALLPAPER_SPEC property on the root window so an already-running
 *     desktop updates immediately. The property exists because this
 *     device's busybox has no kill/killall/pkill at all -- an X property
 *     it already watches via PropertyNotify (the same mechanism
 *     _MB_THEME_NAME uses for live theme switches) is the only way to
 *     signal it.
 *
 * On launch, if $HOME/.matchbox/wallpaper already names a file in one of
 * the known modes, that file and mode are preselected and shown in the
 * preview -- so opening the picker shows what is currently set, not a
 * blank slate.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Shared_Image.H>
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
#include <string>
#include <vector>

#define THUMB_SIZE      100
#define CELL_PAD        14
#define CELL_W          (THUMB_SIZE + CELL_PAD)
#define CELL_H          (THUMB_SIZE + CELL_PAD + 16)   /* + label line */
#define SCROLLBAR_RESERVE 20

#define HEADER_H        64
#define TOOLBAR_H       32
#define PREVIEW_SIZE    56

struct Mode {
    const char *label;
    const char *prefix;        /* spec prefix understood by mbdesktop_bg_parse_spec() */
};

static const Mode MODES[] = {
    { "Mosaic",   "img-mosaic:"    },
    { "Centered", "img-centered:"  },
    { "Stretch",  "img-stretched:" },
    { "Fill",     "img-filled:"    },
};
#define N_MODES (int)(sizeof(MODES) / sizeof(MODES[0]))

struct WPEntry {
    std::string path;
    std::string label;
};

static std::vector<WPEntry> g_entries;
static int                  g_selected_mode = 3;       /* default to Fill */
static std::string          g_current_path;             /* last applied, for the preview */

static Fl_Box *g_preview;
static Fl_Box *g_status;

/* ── home directory, matching mb_util_get_homedir()'s exact fallback ───── */

static std::string homedir(void)
{
    const char *h = getenv("HOME");

    if (h == NULL) {
        const char *t = getenv("TMPDIR");
        return t ? t : "/tmp";
    }
    return h;
}

/* ── scanning ────────────────────────────────────────────────────────── */

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

static void scan_dir(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    struct dirent *de;

    if (!d)
        return;

    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (!has_image_ext(de->d_name))
            continue;

        WPEntry e;
        e.path  = dir + "/" + de->d_name;
        e.label = de->d_name;
        g_entries.push_back(e);
    }

    closedir(d);
}

static bool entry_less(const WPEntry &a, const WPEntry &b)
{
    return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
}

static void scan_wallpapers(void)
{
    scan_dir("/usr/share/backgrounds");
    scan_dir(homedir() + "/.matchbox/backgrounds");
    std::sort(g_entries.begin(), g_entries.end(), entry_less);
}

/* ── the currently persisted spec, so the picker opens showing what is
 * already set rather than a blank slate ───────────────────────────────── */

static bool read_current_spec(std::string &path_out, int &mode_out)
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
                path_out = spec.substr(plen);
                mode_out = i;
                ok = true;
                break;
            }
        }
    }

    fclose(fp);
    return ok;
}

/* ── image loading: decode once, keep only an aspect-correct scaled copy ─── */

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

    /* get(path, tw, th) reuses the original decode we just did (still
     * cached, since we are holding a reference to it) rather than
     * re-reading and re-decoding the file. */
    Fl_Shared_Image *thumb = Fl_Shared_Image::get(path.c_str(), tw, th);
    orig->release();
    return thumb;
}

static std::string basename_of(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/* ── preview swatch next to the mode selector ───────────────────────────── */

static void update_preview(void)
{
    g_preview->image(g_current_path.empty() ? NULL : load_thumb(g_current_path, PREVIEW_SIZE));
    g_preview->redraw();
}

/* ── applying a choice ──────────────────────────────────────────────────── */

static void apply_wallpaper(const std::string &path, const std::string &label)
{
    std::string spec = MODES[g_selected_mode].prefix + path;
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
            rename(tmp.c_str(), file.c_str());     /* atomic -- never leave a torn file */
        }
    }

    /* _MB_WALLPAPER_SPEC: see the file header comment for why this exists. */
    static Atom atom_wallpaper = 0, atom_utf8 = 0;
    if (!atom_wallpaper) {
        atom_wallpaper = XInternAtom(fl_display, "_MB_WALLPAPER_SPEC", False);
        atom_utf8      = XInternAtom(fl_display, "UTF8_STRING", False);
    }
    XChangeProperty(fl_display, RootWindow(fl_display, fl_screen),
                     atom_wallpaper, atom_utf8, 8, PropModeReplace,
                     (const unsigned char *)spec.c_str(), (int)spec.size());
    XFlush(fl_display);

    g_current_path = path;
    update_preview();
    g_status->copy_label(("Applied: " + label + " (" + MODES[g_selected_mode].label + ")").c_str());
}

/* ── callbacks ───────────────────────────────────────────────────────────── */

static void mode_cb(Fl_Widget *, void *data)
{
    /* The buttons are FL_RADIO_BUTTON, so FLTK has already turned the
     * others off -- this is only bookkeeping for the next apply. */
    g_selected_mode = (int)(intptr_t)data;
}

static void thumb_cb(Fl_Widget *, void *data)
{
    const WPEntry &e = g_entries[(size_t)(intptr_t)data];
    apply_wallpaper(e.path, e.label);
}

static void browse_cb(Fl_Widget *, void *)
{
    std::string start = g_current_path.empty()
        ? "/usr/share/backgrounds/"
        : g_current_path.substr(0, g_current_path.find_last_of('/') + 1);

    const char *pick = fl_file_chooser("Choose a wallpaper image",
                                        "Image Files (*.{png,jpg,jpeg,bmp})",
                                        start.c_str(), 0);
    if (!pick)
        return;

    std::string path(pick);
    apply_wallpaper(path, basename_of(path));
}

/* ── the thumbnail grid ─────────────────────────────────────────────────── */

static void build_grid(Fl_Scroll *scroll, int win_w)
{
    scroll->begin();

    if (g_entries.empty()) {
        Fl_Box *msg = new Fl_Box(8, 8, win_w - 16, 40,
            "No images in /usr/share/backgrounds or ~/.matchbox/backgrounds "
            "-- use Browse to pick one.");
        msg->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        scroll->end();
        return;
    }

    int columns = (win_w - SCROLLBAR_RESERVE) / CELL_W;
    if (columns < 1)
        columns = 1;
    int rows = ((int)g_entries.size() + columns - 1) / columns;

    Fl_Group *grid = new Fl_Group(0, 0, columns * CELL_W + CELL_PAD,
                                   rows * CELL_H + CELL_PAD);
    grid->begin();

    for (size_t i = 0; i < g_entries.size(); i++) {
        int col = (int)i % columns;
        int row = (int)i / columns;
        int x = CELL_PAD / 2 + col * CELL_W;
        int y = CELL_PAD / 2 + row * CELL_H;

        Fl_Button *b = new Fl_Button(x, y, THUMB_SIZE, THUMB_SIZE + 16,
                                      g_entries[i].label.c_str());
        b->box(FL_THIN_UP_BOX);
        b->labelsize(11);
        b->align(FL_ALIGN_IMAGE_OVER_TEXT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP | FL_ALIGN_WRAP);
        b->clear_visible_focus();

        Fl_Image *thumb = load_thumb(g_entries[i].path, THUMB_SIZE);
        if (thumb)
            b->image(thumb);
        else
            b->label("(failed to load)");

        b->callback(thumb_cb, (void *)(intptr_t)i);
    }

    grid->end();
    scroll->end();
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    fl_register_images();

    scan_wallpapers();

    std::string current_path;
    int current_mode;
    if (read_current_spec(current_path, current_mode)) {
        g_current_path  = current_path;
        g_selected_mode = current_mode;
    }

    int win_w = Fl::w();
    int win_h = Fl::h();

    Fl_Double_Window win(win_w, win_h, "Set Wallpaper");
    win.begin();

    /* Mode selector, left of a preview swatch that shows what the modes
     * are about to be applied to. */
    int preview_x   = win_w - PREVIEW_SIZE - 8;
    int mode_area_w = preview_x - 8;
    int mode_btn_w  = mode_area_w / N_MODES;

    for (int i = 0; i < N_MODES; i++) {
        Fl_Button *b = new Fl_Button(4 + i * mode_btn_w, 4,
                                      mode_btn_w - 4, HEADER_H - 8,
                                      MODES[i].label);
        b->type(FL_RADIO_BUTTON);
        b->value(i == g_selected_mode);
        b->selection_color(fl_rgb_color(0x34, 0x65, 0xa4));
        b->clear_visible_focus();
        b->callback(mode_cb, (void *)(intptr_t)i);
    }

    g_preview = new Fl_Box(preview_x, 4, PREVIEW_SIZE, PREVIEW_SIZE);
    g_preview->box(FL_DOWN_BOX);

    /* Status line + Browse. */
    Fl_Button *browse_btn = new Fl_Button(win_w - 108, HEADER_H + 4, 100, TOOLBAR_H - 8, "Browse...");
    browse_btn->clear_visible_focus();
    browse_btn->callback(browse_cb);

    g_status = new Fl_Box(8, HEADER_H + 4, win_w - 8 - 108 - 8, TOOLBAR_H - 8, "");
    g_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    if (!g_current_path.empty()) {
        g_status->copy_label(("Current: " + basename_of(g_current_path) +
                               " (" + MODES[g_selected_mode].label + ")").c_str());
    } else {
        g_status->copy_label("Tap an image below, or Browse for one");
    }

    /* Thumbnail grid, in a real scrolling widget instead of hand-rolled
     * up/down buttons. */
    int scroll_y = HEADER_H + TOOLBAR_H;
    Fl_Scroll *scroll = new Fl_Scroll(0, scroll_y, win_w, win_h - scroll_y);
    scroll->box(FL_FLAT_BOX);
    build_grid(scroll, win_w);

    win.end();
    win.resizable(scroll);

    update_preview();

    win.show(argc, argv);

    return Fl::run();
}
