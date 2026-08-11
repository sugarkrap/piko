
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

#define HEADER_H        52
#define CAT_H           24
#define ROW_H           56
#define ICON_SIZE       32
#define ICON_PAD        12
#define TEXT_X          (ICON_PAD + ICON_SIZE + ICON_PAD)
#define SCROLLBAR_RESERVE 18

#define BACK_W          76
#define BACK_H          32

#define SELF_BINARY     "piko-settings"

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
    std::string exec;
    std::string group;
    std::string panel;
};

static std::vector<Entry> g_entries;

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

static std::string homedir(void)
{
    const char *h = getenv("HOME");

    if (h == NULL) {
        const char *t = getenv("TMPDIR");
        return t ? t : "/tmp";
    }
    return h;
}

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

        if (key.find('[') != std::string::npos)
            continue;

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
            i++;
        }
    }

    size_t end = out.find_last_not_of(" \t");
    return end == std::string::npos ? std::string() : out.substr(0, end + 1);
}

static std::string basename_of(const std::string &path)
{
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string exec_binary(const std::string &exec)
{
    size_t space = exec.find_first_of(" \t");
    return basename_of(space == std::string::npos ? exec : exec.substr(0, space));
}

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
    if (a.group != b.group)
        return strcasecmp(a.group.c_str(), b.group.c_str()) < 0;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
}

static bool file_exists(const std::string &p)
{
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

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

    Fl_Shared_Image *scaled = Fl_Shared_Image::get(path.c_str(), tw, th);
    orig->release();
    return scaled;
}

static void exec_entry(const Entry &e)
{
    pid_t pid = fork();

    if (pid != 0)
        return;

    setsid();
    execl("/bin/sh", "sh", "-c", e.exec.c_str(), (char *)NULL);
    _exit(127);
}

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

        if (Fl::focus() == this)
            draw_focus(FL_FLAT_BOX, x() + 2, y() + 2, w() - 4, h() - 4);
    }
};

static void show_panel(size_t index);

static void row_cb(Fl_Widget *, void *data)
{
    size_t index = (size_t)(intptr_t)data;

    if (find_panel(g_entries[index].panel))
        show_panel(index);
    else
        exec_entry(g_entries[index]);
}

static Fl_Scroll *g_scroll;

static Fl_Group  *g_panel;
static Fl_Button *g_back;
static Fl_Box    *g_title;
static Fl_Box    *g_subtitle;
static Fl_Window *g_win;

static void build_list(int win_w)
{
    int y = g_scroll->y();
    std::string current_group;

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

class SettingsWindow : public Fl_Double_Window {
public:
    SettingsWindow(int W, int H, const char *L) : Fl_Double_Window(W, H, L) {}

    void resize(int X, int Y, int W, int H)
    {
        int old_w = w();

        Fl_Double_Window::resize(X, Y, W, H);

        if (W == old_w)
            return;

        if (g_scroll) {
            g_scroll->scroll_to(0, 0);
            build_list(W);
        }

        if (g_panel)
            g_panel->resize(0, HEADER_H, W, H - HEADER_H);

        layout_header(W);
        redraw();
    }

    static void layout_header(int W)
    {
        int right_edge = W - ICON_PAD;

        if (g_back) {
            g_back->resize(W - ICON_PAD - BACK_W,
                            (HEADER_H - BACK_H) / 2, BACK_W, BACK_H);
            if (g_back->visible())
                right_edge = g_back->x() - ICON_PAD;
        }

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

static void show_list(void)
{
    if (!g_panel)
        return;

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

    g_scroll->hide();

    g_win->begin();
    g_panel = make(0, HEADER_H, g_win->w(), g_win->h() - HEADER_H);
    g_win->end();

    g_title->copy_label(e.name.c_str());
    g_subtitle->copy_label(e.comment.empty() ? "" : e.comment.c_str());
    g_win->copy_label(e.name.c_str());

    g_back->show();
    SettingsWindow::layout_header(g_win->w());

    g_win->redraw();
}

int main(int argc, char **argv)
{
    piko_register_images();

    signal(SIGCHLD, SIG_IGN);

    scan_settings();
    std::sort(g_entries.begin(), g_entries.end(), entry_less);

    int win_w = Fl::w();
    int win_h = Fl::h();

    SettingsWindow win(win_w, win_h, "Settings");
    win.color(FL_BACKGROUND_COLOR);
    g_win = &win;
    win.begin();

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

