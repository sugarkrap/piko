/*
 * piko-settings -- the ROM's settings window: every configuration app in
 * one categorised, touch-friendly list, in the shape XFCE's settings
 * manager uses (icon, name, one line of description, grouped under a
 * category heading).
 *
 * WHY THIS EXISTS
 *
 * The desktop is a flat launcher -- see matchbox-desktop-classic's
 * modules/dotdesktop.c, which deliberately stopped bucketing applications
 * into folders. That is right for applications and wrong for settings: a
 * handful of one-purpose configuration apps scattered alphabetically among
 * the games and the file browser is how you fail to find the one you want.
 * They get one door instead, and this is it.
 *
 * NOTHING HERE IS HARDCODED. The list is built by scanning .desktop files,
 * exactly like the desktop and the panel menu do, so adding a settings app
 * later means shipping its .desktop file and nothing else -- no edit to
 * this file, no rebuild of it. An entry qualifies when it is
 *
 *     Type=Application
 *     Categories=...Settings...        (the same word the panel menu's
 *                                       Settings vfolder matches on)
 *     Name= and Exec= both present
 *
 * and it is placed under the heading named by
 *
 *     X-Piko-Settings-Group=Display    (missing => "Other")
 *
 * The three things a settings entry normally also wants, none of which are
 * this program's business:
 *
 *   - X-Piko-NoDesktop=true keeps it off the desktop icon view, since it
 *     is reachable from here. Read by dotdesktop.c, not by us.
 *   - Categories=Settings puts it in the panel menu's "Desktop
 *     Preferences" folder. Read by mb-applet-menu-launcher, not by us.
 *   - X-Piko-Heavy runs it with the graphical session stopped. Read by
 *     dotdesktop.c -- and, so that launching from here behaves the same
 *     way launching from the desktop does, by us as well. See exec_entry().
 *
 * NO libmb. Same call as mb-wallpaper-picker.cxx made: this is an FLTK app
 * and links FLTK, and the ~60 lines of .desktop parsing below are cheaper
 * than a dependency on the desktop's own library. The parser is a subset on
 * purpose -- see parse_desktop_file().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/fl_draw.H>

#include "panels/piko-panel.H"
#include "panels/panel-wallpaper.H"

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

/* Layout. Sized for the two orientations this device actually has --
 * 640x480 open, 480x640 swivelled (docs/HOWTO-SCREEN-ROTATION.md) -- so
 * every width below is derived from the window, never assumed. */
#define HEADER_H        52
#define CAT_H           24
#define ROW_H           56
#define ICON_SIZE       32
#define ICON_PAD        12
#define TEXT_X          (ICON_PAD + ICON_SIZE + ICON_PAD)
#define SCROLLBAR_RESERVE 18

/* "Back", shown in the header only while a panel is open. Sized as a touch
 * target first -- it is the only way out of a panel. */
#define BACK_W          76
#define BACK_H          32

/* Our own .desktop file, skipped when scanning so the settings window does
 * not list itself. Matched on Exec's basename rather than the filename:
 * the filename is a packaging detail, the binary it runs is the identity. */
#define SELF_BINARY     "piko-settings"

/* Heading order. Anything not named here sorts alphabetically after these,
 * and the catch-all "Other" is always last -- so a settings app shipped
 * with an unrecognised (or absent) group still appears, just at the end. */
static const char *GROUP_ORDER[] = {
    "Display",
    "Input",
    "Sound",
    "Power",
    "Network",
    "System",
    NULL
};
#define GROUP_FALLBACK  "Other"

struct Entry {
    std::string name;
    std::string comment;
    std::string icon;
    std::string exec;         /* already field-code-stripped and, if the
                               * entry is X-Piko-Heavy, already wrapped */
    std::string group;
    std::string panel;        /* X-Piko-Settings-Panel; empty => launch it */
};

static std::vector<Entry> g_entries;

/*
 * The panels this build can show inside its own window.
 *
 * An entry whose .desktop carries X-Piko-Settings-Panel=<name> matching one
 * of these is opened here, XFCE-style, instead of being launched as a
 * separate program. Everything else -- and anything naming a panel this
 * build does not have, e.g. a .desktop from a newer package -- is launched.
 * That fallback is deliberate: an unknown name must never produce a row
 * that does nothing when tapped.
 *
 * Adding a panel is one line here plus the key in the .desktop file. See
 * panels/piko-panel.H.
 *
 * Not everything can be a panel, and that is not a gap to be closed:
 * pikalibrate is SDL rather than FLTK, needs the whole 640x480 to put its
 * crosses in the physical corners, and SUSPENDS the X server while it runs
 * (writes SUSPEND to /tmp/.pikalibrate-ctl; Xfbdev does KdSuspend()). A
 * panel cannot be hosted inside a window drawn by the server it switches
 * off. It stays a launch, and always will.
 */
struct PanelReg {
    const char       *name;
    PikoPanelFactory  make;
};

static const PanelReg PANELS[] = {
    { "wallpaper", piko_panel_wallpaper_new },
    { NULL,        NULL                     }
};

static PikoPanelFactory find_panel(const std::string &name)
{
    if (name.empty())
        return NULL;

    for (int i = 0; PANELS[i].name; i++)
        if (name == PANELS[i].name)
            return PANELS[i].make;

    return NULL;
}

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

/* ── .desktop parsing ──────────────────────────────────────────────────── */

/*
 * A deliberate subset of the Desktop Entry spec: the [Desktop Entry] group
 * only, plain Key=Value, no escape sequences, and localised keys
 * (Name[de]=) skipped rather than matched against a locale. That is the
 * whole of what the files in userspace/desktop/ use, and matchbox's own
 * parser (libmb/mbdotdesktop.c) is barely more than this either.
 *
 * Keys outside [Desktop Entry] are ignored rather than merged: a file with
 * a trailing [Desktop Action Foo] group must not have that group's Name=
 * or Exec= silently override the real ones.
 */
static bool parse_desktop_file(const std::string &path,
                                std::map<std::string, std::string> &out)
{
    FILE *fp = fopen(path.c_str(), "r");
    char buf[1024];
    bool in_entry = false;

    if (!fp)
        return false;

    while (fgets(buf, sizeof(buf), fp)) {
        char *line = buf;
        size_t len;

        while (*line == ' ' || *line == '\t')
            line++;

        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'
                            || line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#')
            continue;

        if (line[0] == '[') {
            in_entry = (strcmp(line, "[Desktop Entry]") == 0);
            continue;
        }

        if (!in_entry)
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;

        std::string key(line, eq - line);
        std::string value(eq + 1);

        /* Name[de]= and friends: not this program's problem. */
        if (key.find('[') != std::string::npos)
            continue;

        /* First occurrence wins, as in libmb's hash-insert. */
        if (out.find(key) == out.end())
            out[key] = value;
    }

    fclose(fp);
    return true;
}

static std::string get(const std::map<std::string, std::string> &m,
                        const char *key)
{
    std::map<std::string, std::string>::const_iterator it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

static bool is_true(const std::string &v)
{
    return !strcasecmp(v.c_str(), "true") || v == "1";
}

/*
 * Exec= field codes. None of the settings apps take an argument, so the
 * codes are simply removed rather than substituted -- which is also what
 * libmb's mb_dotdesktop_get_exec() does. %% is the one that must survive
 * as a literal.
 */
static std::string strip_field_codes(const std::string &exec)
{
    std::string out;

    for (size_t i = 0; i < exec.size(); i++) {
        if (exec[i] != '%') {
            out += exec[i];
            continue;
        }
        if (i + 1 < exec.size() && exec[i + 1] == '%') {
            out += '%';
            i++;
        } else {
            i++;              /* drop the code letter with the % */
        }
    }

    /* Trailing whitespace left behind by a dropped "%U" and the like. */
    size_t end = out.find_last_not_of(" \t");
    return end == std::string::npos ? std::string() : out.substr(0, end + 1);
}

static std::string basename_of(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/* The command's own basename, i.e. Exec= minus any arguments and path. */
static std::string exec_binary(const std::string &exec)
{
    size_t space = exec.find_first_of(" \t");
    return basename_of(space == std::string::npos ? exec : exec.substr(0, space));
}

/* ── scanning ──────────────────────────────────────────────────────────── */

/*
 * The same directories matchbox-desktop's dotdesktop_init() scans, in the
 * same order and for the same reasons -- including the SD card, since an
 * application (and therefore a settings app) can be installed onto the card
 * rather than into the ~68 MiB NAND root. A directory that is not there is
 * the ordinary case for two of these and is skipped silently.
 */
static void scan_dir(const std::string &dir)
{
    DIR *d = opendir(dir.c_str());
    struct dirent *de;

    if (!d)
        return;

    while ((de = readdir(d)) != NULL) {
        std::string fname(de->d_name);
        struct stat st;

        if (fname.empty() || fname[0] == '.')
            continue;
        if (fname.size() <= 8 || fname.compare(fname.size() - 8, 8, ".desktop") != 0)
            continue;

        std::string path = dir + "/" + fname;
        if (lstat(path.c_str(), &st) || S_ISDIR(st.st_mode))
            continue;

        std::map<std::string, std::string> kv;
        if (!parse_desktop_file(path, kv))
            continue;

        if (get(kv, "Type") != "Application")
            continue;

        std::string categories = get(kv, "Categories");
        if (categories.find("Settings") == std::string::npos)
            continue;

        std::string name = get(kv, "Name");
        std::string exec = strip_field_codes(get(kv, "Exec"));
        if (name.empty() || exec.empty())
            continue;

        if (exec_binary(exec) == SELF_BINARY)
            continue;

        /* Same rewrite dotdesktop.c applies, so an entry that needs the
         * framebuffer to itself is launched through matchbox-fbrun from
         * here too rather than straight under X, where it would render
         * nothing and receive no input. */
        if (is_true(get(kv, "X-Piko-Heavy"))) {
            std::string reason = get(kv, "X-Piko-Heavy-Reason");
            std::string wrapped = "matchbox-fbrun -n '" + name + "'";
            if (!reason.empty())
                wrapped += " -r '" + reason + "'";
            exec = wrapped + " -- " + exec;
        }

        Entry e;
        e.name    = name;
        e.comment = get(kv, "Comment");
        e.icon    = get(kv, "Icon");
        e.exec    = exec;
        e.panel   = get(kv, "X-Piko-Settings-Panel");
        e.group   = get(kv, "X-Piko-Settings-Group");
        if (e.group.empty())
            e.group = GROUP_FALLBACK;

        g_entries.push_back(e);
    }

    closedir(d);
}

static void scan_settings(void)
{
    std::vector<std::string> dirs;

    dirs.push_back("/usr/share/applications");
    dirs.push_back("/usr/local/share/applications");
    dirs.push_back(homedir() + "/.applications");
    dirs.push_back("/mnt/card/.zaurus/usr/share/applications");

    for (size_t i = 0; i < dirs.size(); i++) {
        bool dup = false;
        for (size_t j = 0; j < i; j++)
            if (dirs[i] == dirs[j])
                dup = true;
        if (!dup)
            scan_dir(dirs[i]);
    }
}

static int group_rank(const std::string &g)
{
    for (int i = 0; GROUP_ORDER[i]; i++)
        if (g == GROUP_ORDER[i])
            return i;
    return g == GROUP_FALLBACK ? 10000 : 5000;
}

static bool entry_less(const Entry &a, const Entry &b)
{
    int ra = group_rank(a.group), rb = group_rank(b.group);

    if (ra != rb)
        return ra < rb;
    if (a.group != b.group)                       /* both unranked */
        return strcasecmp(a.group.c_str(), b.group.c_str()) < 0;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
}

/* ── icons ─────────────────────────────────────────────────────────────── */

static bool file_exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

/*
 * Icon= resolution, matching libmb's _find_icon_with_ext() rather than the
 * full icon-theme spec: an absolute path is taken as-is, anything else is
 * probed against the pixmap directories with each loadable extension
 * appended. Icon= in these files is normally bare ("pikalibrate"), which
 * is why the extension probe is not optional.
 */
static std::string resolve_icon(const std::string &icon)
{
    static const char *exts[] = { "", ".png", ".xpm", ".jpg", ".jpeg", ".bmp", NULL };

    if (icon.empty())
        return std::string();

    if (icon[0] == '/')
        return file_exists(icon) ? icon : std::string();

    std::vector<std::string> dirs;
    dirs.push_back(homedir() + "/.icons");
    dirs.push_back("/usr/share/pixmaps");
    dirs.push_back("/usr/local/share/pixmaps");
    dirs.push_back("/mnt/card/.zaurus/usr/share/pixmaps");

    for (size_t i = 0; i < dirs.size(); i++)
        for (int e = 0; exts[e]; e++) {
            std::string p = dirs[i] + "/" + icon + exts[e];
            if (file_exists(p))
                return p;
        }

    return std::string();
}

/* Decoded once at startup and kept: there are only ever a handful, and
 * re-decoding on every expose on a 400MHz PXA255 is not free. */
static Fl_Image *load_icon(const std::string &icon, int box)
{
    std::string path = resolve_icon(icon);

    if (path.empty())
        return NULL;

    Fl_Shared_Image *orig = Fl_Shared_Image::get(path.c_str());
    if (!orig || orig->w() <= 0 || orig->h() <= 0) {
        if (orig)
            orig->release();
        return NULL;
    }

    if (orig->w() == box && orig->h() == box)
        return orig;

    int tw, th;
    if (orig->w() >= orig->h()) {
        tw = box;
        th = orig->h() * box / orig->w();
    } else {
        th = box;
        tw = orig->w() * box / orig->h();
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    /* Reuses the decode above -- still cached, since we hold a reference. */
    Fl_Shared_Image *scaled = Fl_Shared_Image::get(path.c_str(), tw, th);
    orig->release();
    return scaled;
}

/* ── launching ─────────────────────────────────────────────────────────── */

/*
 * fork + "sh -c", not execvp of a hand-split argv: Exec= here can carry
 * quoted arguments (the matchbox-fbrun wrapper above builds exactly that),
 * and a shell is the thing that already knows how to split them. The
 * desktop reaches the same place through libmb's mb_exec().
 *
 * SIGCHLD is set to SIG_IGN in main() so these never become zombies -- this
 * window stays open after launching, so nothing here ever wait()s.
 */
static void exec_entry(const Entry &e)
{
    pid_t pid = fork();

    if (pid != 0)
        return;                       /* parent (or a failed fork: nothing
                                       * useful to do about it from a GUI) */

    setsid();                         /* don't die with the settings window */
    execl("/bin/sh", "sh", "-c", e.exec.c_str(), (char *)NULL);
    _exit(127);
}

/* ── widgets ───────────────────────────────────────────────────────────── */

/* A category heading: the grey strip above each group of rows. */
class CategoryHeader : public Fl_Box {
public:
    CategoryHeader(int X, int Y, int W, int H, const char *L)
        : Fl_Box(X, Y, W, H)
    {
        copy_label(L);
        box(FL_FLAT_BOX);
        color(fl_rgb_color(0xD8, 0xD8, 0xD4));
        labelfont(FL_HELVETICA_BOLD);
        labelsize(12);
        labelcolor(fl_rgb_color(0x40, 0x40, 0x40));
        align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }

    void draw(void)
    {
        Fl_Box::draw();
        fl_color(fl_rgb_color(0xB0, 0xB0, 0xAC));
        fl_line(x(), y() + h() - 1, x() + w() - 1, y() + h() - 1);
    }
};

/*
 * One settings entry: icon, name, and the Comment= line underneath it.
 *
 * A custom draw() rather than an Fl_Button label because the two lines of
 * text need different fonts, sizes and colours, and FL_ALIGN_IMAGE_NEXT_TO_TEXT
 * gives one label with one of each.
 */
class SettingsRow : public Fl_Button {
    Fl_Image   *icon_;
    std::string name_;
    std::string comment_;

public:
    SettingsRow(int X, int Y, int W, int H, const Entry &e)
        : Fl_Button(X, Y, W, H), icon_(NULL), name_(e.name), comment_(e.comment)
    {
        box(FL_FLAT_BOX);
        color(FL_BACKGROUND_COLOR);
        icon_ = load_icon(e.icon, ICON_SIZE);

        /* Focus is deliberately LEFT ON here, unlike the touch-first
         * widgets in mb-wallpaper-picker, so the list is navigable with the
         * arrow keys and OK as well as by tapping.
         *
         * That is not symmetry for its own sake: "Calibrate touchscreen" is
         * in this list, and it is exactly what you reach for when the
         * touchscreen is mis-calibrated enough that tapping the row you
         * want is the thing you cannot do. A touch-only settings list puts
         * the fix behind the fault. */
    }

    void draw(void)
    {
        bool pressed = (value() != 0);

        fl_color(pressed ? fl_rgb_color(0x34, 0x65, 0xA4) : color());
        fl_rectf(x(), y(), w(), h());

        if (icon_) {
            icon_->draw(x() + ICON_PAD + (ICON_SIZE - icon_->w()) / 2,
                        y() + (h() - icon_->h()) / 2);
        } else {
            /* An empty outline, deliberately visible. An entry whose Icon=
             * did not resolve is still perfectly usable, so this must not
             * shout -- but it is nearly always a packaging slip (the
             * .desktop file shipped and its .png did not, see LAUNCHERS in
             * tools/build-matchbox-payload.sh), and a silently blank
             * gutter is how that reaches the device unnoticed.
             *
             * Not a fill: at background colour it is invisible, and any
             * other fill reads as a real icon from arm's length. */
            fl_color(pressed ? fl_rgb_color(0x7C, 0xA0, 0xD0)
                             : fl_rgb_color(0x90, 0x90, 0x8C));
            fl_rect(x() + ICON_PAD, y() + (h() - ICON_SIZE) / 2,
                    ICON_SIZE, ICON_SIZE);
        }

        int tx = x() + TEXT_X;
        int tw = w() - TEXT_X - ICON_PAD;
        if (tw < 1)
            tw = 1;

        fl_push_clip(tx, y(), tw, h());

        int name_y = comment_.empty() ? y() + h() / 2 + 5 : y() + h() / 2 - 2;

        fl_font(FL_HELVETICA_BOLD, 13);
        fl_color(pressed ? FL_WHITE : fl_rgb_color(0x20, 0x20, 0x20));
        fl_draw(name_.c_str(), tx, name_y);

        if (!comment_.empty()) {
            fl_font(FL_HELVETICA, 11);
            fl_color(pressed ? fl_rgb_color(0xD0, 0xDC, 0xEC)
                             : fl_rgb_color(0x60, 0x60, 0x60));
            fl_draw(comment_.c_str(), tx, y() + h() / 2 + 14);
        }

        fl_pop_clip();

        fl_color(fl_rgb_color(0xC8, 0xC8, 0xC4));
        fl_line(x() + TEXT_X, y() + h() - 1, x() + w() - 1, y() + h() - 1);

        /* draw() is fully overridden, so Fl_Button's own focus rectangle
         * never gets drawn -- without this the keyboard-navigable list
         * above would give no indication of which row is selected. */
        if (Fl::focus() == this)
            draw_focus(FL_FLAT_BOX, x() + 2, y() + 2, w() - 4, h() - 4);
    }
};

static void show_panel(size_t index);       /* defined below the window */

/*
 * Tapping a row either opens the panel here or launches the program.
 *
 * Which one is a property of the .desktop file (X-Piko-Settings-Panel),
 * not of this code, so a settings app decides for itself whether it is
 * embeddable -- and one that is not, or that names a panel this build has
 * never heard of, is still perfectly reachable.
 */
static void row_cb(Fl_Widget *, void *data)
{
    size_t index = (size_t)(intptr_t)data;

    if (find_panel(g_entries[index].panel))
        show_panel(index);
    else
        exec_entry(g_entries[index]);
}

/* ── the list ──────────────────────────────────────────────────────────── */

static Fl_Scroll *g_scroll;

/* Header widgets and the embedded panel. File-scope because showing a
 * panel retitles the header and swaps what fills the window below it. */
static Fl_Group  *g_panel;          /* NULL when the list is showing */
static Fl_Button *g_back;
static Fl_Box    *g_title;
static Fl_Box    *g_subtitle;
static Fl_Window *g_win;

/*
 * Rebuilt rather than resized on a window size change. This device rotates
 * live -- flipd swivels the desktop between 640x480 and 480x640 when the
 * lid is folded (docs/HOWTO-SCREEN-ROTATION.md) -- and FLTK's proportional
 * child resizing would scale row heights and icon gutters along with the
 * width. Rebuilding is a few dozen widgets and keeps every vertical metric
 * fixed, which is what a list wants.
 */
static void build_list(int win_w)
{
    int y = g_scroll->y();
    std::string current_group;

    /* Reserve the scrollbar gutter only when there is going to be a
     * scrollbar. The list usually fits, and a permanent gutter is visible
     * as a category strip that stops short of the right edge. Counting the
     * headings first is exact -- every row is the same height. */
    int groups = 0;
    for (size_t i = 0; i < g_entries.size(); i++)
        if (i == 0 || g_entries[i].group != g_entries[i - 1].group)
            groups++;

    int needed_h  = groups * CAT_H + (int)g_entries.size() * ROW_H;
    int content_w = needed_h > g_scroll->h() ? win_w - SCROLLBAR_RESERVE : win_w;

    g_scroll->clear();
    g_scroll->begin();

    if (g_entries.empty()) {
        Fl_Box *msg = new Fl_Box(ICON_PAD, y + ICON_PAD,
                                  content_w - 2 * ICON_PAD, 60,
            "No settings found.\n"
            "A settings app appears here once its .desktop file is installed "
            "with Categories=Settings.");
        msg->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
        msg->labelsize(12);
        g_scroll->end();
        return;
    }

    for (size_t i = 0; i < g_entries.size(); i++) {
        if (g_entries[i].group != current_group) {
            current_group = g_entries[i].group;
            new CategoryHeader(g_scroll->x(), y, content_w, CAT_H,
                               current_group.c_str());
            y += CAT_H;
        }

        SettingsRow *row = new SettingsRow(g_scroll->x(), y, content_w, ROW_H,
                                            g_entries[i]);
        row->callback(row_cb, (void *)(intptr_t)i);
        y += ROW_H;
    }

    g_scroll->end();
}

/* Window subclass only so the list can be rebuilt on rotation. */
class SettingsWindow : public Fl_Double_Window {
public:
    SettingsWindow(int W, int H, const char *L) : Fl_Double_Window(W, H, L) {}

    void resize(int X, int Y, int W, int H)
    {
        int old_w = w();

        Fl_Double_Window::resize(X, Y, W, H);

        if (W == old_w)
            return;

        /* The list is rebuilt even while a panel covers it: it is what
         * comes back when Back is tapped, and rebuilding it now is
         * cheaper than remembering that it is stale. */
        if (g_scroll) {
            g_scroll->scroll_to(0, 0);
            build_list(W);
        }

        /* The panel lays itself out from the geometry it is given, so it
         * only needs telling. Fl_Group::resize would otherwise scale it
         * proportionally along with everything else. */
        if (g_panel)
            g_panel->resize(0, HEADER_H, W, H - HEADER_H);

        layout_header(W);
        redraw();
    }

    /* Header geometry depends on whether Back is showing, and on the
     * window width, so both the initial build and every rotation go
     * through here. */
    static void layout_header(int W)
    {
        int right_edge = W - ICON_PAD;

        if (g_back) {
            g_back->resize(W - ICON_PAD - BACK_W,
                            (HEADER_H - BACK_H) / 2, BACK_W, BACK_H);
            if (g_back->visible())
                right_edge = g_back->x() - ICON_PAD;
        }

        /* The text starts wherever main() put it -- TEXT_X with the window
         * icon present, ICON_PAD without it -- so read it back rather than
         * assuming which. */
        if (g_title) {
            int text_w = right_edge - g_title->x();
            if (text_w < 1)
                text_w = 1;
            g_title->resize(g_title->x(), g_title->y(), text_w, g_title->h());
            if (g_subtitle)
                g_subtitle->resize(g_subtitle->x(), g_subtitle->y(),
                                    text_w, g_subtitle->h());
        }
    }
};

/* ── showing a panel, and coming back ──────────────────────────────────── */

static void show_list(void)
{
    if (!g_panel)
        return;

    /* Fl::delete_widget rather than delete: this runs from the Back
     * button's own callback, with the panel still on FLTK's push/focus
     * bookkeeping. Deferring to the top of the next event loop is exactly
     * what it is for. */
    g_panel->hide();
    Fl::delete_widget(g_panel);
    g_panel = NULL;

    g_back->hide();
    g_title->copy_label("Settings");
    g_subtitle->copy_label("Customize your device");
    g_win->copy_label("Settings");

    SettingsWindow::layout_header(g_win->w());

    g_scroll->show();
    g_win->redraw();
}

static void back_cb(Fl_Widget *, void *)
{
    show_list();
}

static void show_panel(size_t index)
{
    const Entry &e = g_entries[index];
    PikoPanelFactory make = find_panel(e.panel);

    if (!make || g_panel)
        return;

    /* The list is hidden, not destroyed -- coming back is a show(), and
     * the scroll position is preserved for free. */
    g_scroll->hide();

    g_win->begin();
    g_panel = make(0, HEADER_H, g_win->w(), g_win->h() - HEADER_H);
    g_win->end();

    /* The header becomes the panel's, the way XFCE's settings manager
     * retitles itself around an embedded dialog. */
    g_title->copy_label(e.name.c_str());
    g_subtitle->copy_label(e.comment.empty() ? "" : e.comment.c_str());
    g_win->copy_label(e.name.c_str());

    g_back->show();
    SettingsWindow::layout_header(g_win->w());

    g_win->redraw();
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    /* Guarded, because an embedded panel needs the image handlers too and
     * FLTK's own registration is not idempotent -- see panels/piko-panel.H. */
    piko_register_images();

    /* See exec_entry(): nothing here ever wait()s for a launched app. */
    signal(SIGCHLD, SIG_IGN);

    scan_settings();
    std::sort(g_entries.begin(), g_entries.end(), entry_less);

    int win_w = Fl::w();
    int win_h = Fl::h();

    SettingsWindow win(win_w, win_h, "Settings");
    win.color(FL_BACKGROUND_COLOR);
    g_win = &win;
    win.begin();

    /* Header: the window's own icon, title, and what this window is for --
     * the same three things XFCE's settings manager puts there. */
    Fl_Box *header = new Fl_Box(0, 0, win_w, HEADER_H);
    header->box(FL_FLAT_BOX);
    header->color(fl_rgb_color(0xF0, 0xF0, 0xEC));

    int text_x = ICON_PAD;
    Fl_Image *self_icon = load_icon(SELF_BINARY, ICON_SIZE);
    if (self_icon) {
        Fl_Box *ib = new Fl_Box(ICON_PAD, (HEADER_H - ICON_SIZE) / 2,
                                 ICON_SIZE, ICON_SIZE);
        ib->image(self_icon);
        text_x = TEXT_X;
    }

    g_title = new Fl_Box(text_x, 6, win_w - text_x - ICON_PAD, 22, "Settings");
    g_title->labelfont(FL_HELVETICA_BOLD);
    g_title->labelsize(16);
    g_title->labelcolor(fl_rgb_color(0x20, 0x20, 0x20));
    g_title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    g_subtitle = new Fl_Box(text_x, 28, win_w - text_x - ICON_PAD, 16,
                             "Customize your device");
    g_subtitle->labelsize(11);
    g_subtitle->labelcolor(fl_rgb_color(0x60, 0x60, 0x60));
    g_subtitle->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    /* Created up front and hidden, rather than built and destroyed with
     * each panel: it belongs to the header, which outlives them. */
    g_back = new Fl_Button(win_w - ICON_PAD - BACK_W, (HEADER_H - BACK_H) / 2,
                            BACK_W, BACK_H, "@<-  Back");
    g_back->callback(back_cb);
    g_back->hide();

    g_scroll = new Fl_Scroll(0, HEADER_H, win_w, win_h - HEADER_H);
    g_scroll->box(FL_FLAT_BOX);
    g_scroll->color(FL_BACKGROUND_COLOR);
    g_scroll->type(Fl_Scroll::VERTICAL);
    build_list(win_w);

    win.end();
    win.resizable(g_scroll);
    win.show(argc, argv);

    return Fl::run();
}

