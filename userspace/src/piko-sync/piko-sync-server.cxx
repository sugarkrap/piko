#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Pixmap.H>
#include <FL/fl_ask.H>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "protocol.h"
#include "transfer_state.h"
#include "transfer_queue.h"
#include "transfer_table.h"
#include "net_io.h"
#include "icon_xpm.h"
#include "rom_detect.h"
#include "emulation_db.h"
#include "bezel_store.h"
#include "jar_meta.h"

using namespace piko_sync;

static const char *TRANSFERS_DIR = "/mnt/card/Transfers";
static const char *PART_SUFFIX = ".piko-sync-part";

static const int HEADER_H = 44;
static const int TABBAR_H = 24;
static const int DOCK_SHUT_H = 28;
static const int DOCK_OPEN_H = 168;

static const int JFFS2_EAGAIN_RETRIES = 20;
static const int JFFS2_EAGAIN_DELAY_US = 100000;

static ssize_t write_retry(int fd, const void *buf, size_t count)
{
    for (int attempt = 0; ; attempt++) {
        ssize_t n = write(fd, buf, count);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static ssize_t pwrite_retry(int fd, const void *buf, size_t count, off_t offset)
{
    for (int attempt = 0; ; attempt++) {
        ssize_t n = pwrite(fd, buf, count, offset);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static int rename_retry(const char *oldpath, const char *newpath)
{
    for (int attempt = 0; ; attempt++) {
        if (rename(oldpath, newpath) == 0)
            return 0;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static int mkdir_retry(const char *path, mode_t mode)
{
    for (int attempt = 0; ; attempt++) {
        if (mkdir(path, mode) == 0)
            return 0;
        if (errno == EEXIST)
            return 0;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static int open_retry(const char *path, int flags, mode_t mode)
{
    for (int attempt = 0; ; attempt++) {
        int fd = open(path, flags, mode);
        if (fd >= 0)
            return fd;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static std::string dirname_of(const std::string &path)
{
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

static bool free_bytes_on(const std::string &path, uint64_t &out)
{
    std::string probe = path;
    struct stat st;
    while (!probe.empty() && stat(probe.c_str(), &st) != 0) {
        std::string parent = dirname_of(probe);
        if (parent == probe)
            break;
        probe = parent;
    }
    if (probe.empty())
        probe = "/";

    struct statvfs sv;
    if (statvfs(probe.c_str(), &sv) != 0)
        return false;
    out = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
    return true;
}

static void names_in_dir(const std::string &dir, std::vector<std::string> &out)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    std::string suffix(PART_SUFFIX);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        std::string name(e->d_name);
        if (name == "." || name == "..")
            continue;
        if (name.size() >= suffix.size()
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            continue;
        struct stat st;
        if (stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        out.push_back(name);
    }
    closedir(d);
}

static bool mkdir_p(const std::string &path)
{
    if (path.empty() || path == "/")
        return true;
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    if (!mkdir_p(dirname_of(path)))
        return false;
    return mkdir_retry(path.c_str(), 0755) == 0;
}

static bool is_sd_card_mounted()
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, " /mnt/card ")) { found = true; break; }
    }
    fclose(f);
    return found;
}

static std::string staging_filename_for(const std::string &dest)
{
    std::string s = dest;
    if (!s.empty() && s[0] == '/')
        s.erase(0, 1);
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '/')
            s[i] = '_';
    return s;
}

static bool resolve_staging_dir(uint32_t staging, std::string &dir, std::string &error)
{
    switch (staging) {
    case STAGE_NAND:
        dir = "/tmp";
        return true;
    case STAGE_SD:
        system("mount /mnt/card >/dev/null 2>&1");
        if (!is_sd_card_mounted()) { error = "SD card is not mounted"; return false; }
        dir = "/mnt/card/.zaurus/tmp";
        return mkdir_p(dir);
    case STAGE_CF:
        error = "CF staging is not yet supported on this device";
        return false;
    default:
        error = "unknown staging kind";
        return false;
    }
}

static bool copy_file(const std::string &src, const std::string &dst)
{
    int in = open(src.c_str(), O_RDONLY);
    if (in < 0)
        return false;
    int out = open_retry(dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) { close(in); return false; }

    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = write_retry(out, buf, static_cast<size_t>(n));
        if (w != n) { ok = false; break; }
    }
    if (n < 0)
        ok = false;

    close(in);
    close(out);
    return ok;
}

static bool write_desktop_file(const RomEntry &e, std::string &err)
{
    mkdir_p(APPLICATIONS_DIR);
    std::string dpath = std::string(APPLICATIONS_DIR) + "/" + e.desktop;
    std::string contents = desktop_contents(e);
    int fd = open_retry(dpath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { err = "cannot write " + dpath; return false; }
    bool ok = write_retry(fd, contents.data(), contents.size()) == (ssize_t)contents.size();
    close(fd);
    if (!ok) { err = dpath + " is truncated"; return false; }
    return true;
}

static bool piko_media(HelloAckMsg &out)
{
    FILE *f = popen(". /etc/piko-media 2>/dev/null || exit 1\n"
                    "echo \"$PIKO_KERNEL\"\n"
                    "echo \"$PIKO_BOOT_MNT\"\n"
                    "echo \"$PIKO_CARD_MNT\"\n"
                    "echo \"$PIKO_CARD_ROOT\"", "r");
    if (!f)
        return false;
    std::string fields[4];
    int got = 0;
    char line[512];
    while (got < 4 && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        fields[got++] = line;
    }
    if (pclose(f) != 0 || got < 4)
        return false;
    out.kernel = fields[0];
    out.boot_mnt = fields[1];
    out.card_mnt = fields[2];
    out.card_root = fields[3];
    return true;
}

static std::string read_file(const std::string &path)
{
    std::string out;
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return out;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

static int prune_rom_launchers(const std::vector<RomEntry> &db)
{
    DIR *d = opendir(APPLICATIONS_DIR);
    if (!d)
        return 0;

    int removed = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        std::string name(e->d_name);
        if (name == "." || name == "..")
            continue;

        std::string dpath = std::string(APPLICATIONS_DIR) + "/" + name;
        std::string rom = desktop_rom_path(read_file(dpath));
        if (rom.empty())
            continue;

        bool live = false;
        if (rom[0] != '@')
            for (size_t i = 0; i < db.size(); i++)
                if (db[i].path == rom && db[i].desktop == name) { live = true; break; }
        if (!live && unlink(dpath.c_str()) == 0)
            removed++;
    }
    closedir(d);
    return removed;
}

static void migrate_legacy_dbs()
{
    migrate_legacy_emulation_db();
}

static int sync_rom_launchers()
{
    std::vector<RomEntry> db = load_emulation_db();

    bool db_changed = false;
    int changed = 0;
    for (size_t i = 0; i < db.size(); i++) {
        if (db[i].path.empty() || db[i].path[0] == '@')
            continue;
        if (!db[i].icon.empty() && db[i].icon.find('/') == std::string::npos) {
            db[i].icon = DEFAULT_ROM_ICON;
            db_changed = true;
        }
        if (db[i].desktop.empty()) {
            db[i].desktop = desktop_name_for(db[i].machine, db[i].path);
            db_changed = true;
        }
        std::string dpath = std::string(APPLICATIONS_DIR) + "/" + db[i].desktop;
        if (read_file(dpath) == desktop_contents(db[i]))
            continue;
        std::string err;
        if (write_desktop_file(db[i], err))
            changed++;
    }
    if (db_changed)
        save_emulation_db(db);

    return changed + prune_rom_launchers(db);
}

static bool register_local_rom(const std::string &rom_path, const std::string &machine,
                               const std::string &options, std::string &status)
{
    RomEntry e;
    e.path = rom_path;
    e.machine = machine;
    e.backend = machine_backend(machine);
    if (e.backend.empty()) {
        status = "no emulator backend for " + machine
                 + " -- " + basename_of_path(rom_path) + " is stored but not registered";
        return false;
    }
    e.desktop = desktop_name_for(e.machine, rom_path);
    e.icon = DEFAULT_ROM_ICON;

    {
        std::string media = option_get(options, "media");
        if (!media.empty())
            option_set(e.options, "media", media);
        if (option_get(options, "heavy") == "1" || e.machine != "J2ME")
            option_set(e.options, "heavy", "1");
        std::string title = option_get(options, "title");
        if (!title.empty())
            option_set(e.options, "title", title);
    }

    if (e.machine == "J2ME") {
        if (option_get(options, "rotate") == "1")
            option_set(e.options, "rotate", "1");
    }

    std::vector<RomEntry> db = load_emulation_db();
    for (size_t i = 0; i < db.size(); i++) {
        if (db[i].path == e.path) { db.erase(db.begin() + i); break; }
    }
    db.push_back(e);

    if (!save_emulation_db(db)) {
        status = "rom registered but " + emulation_cfg_for(media_of_path(e.path))
                 + " is not writable";
        return false;
    }

    std::string err;
    if (!write_desktop_file(e, err)) {
        status = "rom registered but " + err;
        return false;
    }

    system("/usr/sbin/deskscan >/dev/null 2>&1");
    status = e.machine + " rom registered: " + e.desktop;
    return true;
}

static bool delete_local_rom(const std::string &rom_path, std::string &err)
{
    std::vector<RomEntry> db = load_emulation_db();
    RomEntry found;
    bool have = false;
    for (size_t i = 0; i < db.size(); i++) {
        if (db[i].path == rom_path) { found = db[i]; have = true; db.erase(db.begin() + i); break; }
    }
    if (!have) {
        err = "no such rom on any mounted media";
        return false;
    }

    if (unlink(found.path.c_str()) != 0 && errno != ENOENT) {
        err = "cannot delete " + found.path + ": " + strerror(errno);
        return false;
    }

    if (!found.desktop.empty())
        unlink((std::string(APPLICATIONS_DIR) + "/" + found.desktop).c_str());

    if (!save_emulation_db(db)) {
        err = "rom deleted but " + emulation_cfg_for(media_of_path(found.path))
              + " is not writable";
        return false;
    }

    system("/usr/sbin/deskscan >/dev/null 2>&1");
    return true;
}

static bool set_local_rom_icon(const std::string &rom_path, const std::string &png,
                               std::string &err)
{
    std::vector<RomEntry> db = load_emulation_db();
    int idx = -1;
    for (size_t i = 0; i < db.size(); i++)
        if (db[i].path == rom_path) { idx = (int)i; break; }
    if (idx < 0) {
        err = "no such rom on any mounted media";
        return false;
    }

    std::string ipath = icon_path_for(db[idx].machine, db[idx].path);
    mkdir_p(PIXMAPS_DIR);
    int fd = open_retry(ipath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        err = "cannot write " + ipath + ": " + strerror(errno);
        return false;
    }
    bool ok = write_retry(fd, png.data(), png.size()) == (ssize_t)png.size();
    close(fd);
    if (!ok) {
        err = ipath + " is truncated";
        return false;
    }

    db[idx].icon = ipath;
    if (!save_emulation_db(db)) {
        err = "cannot update " + emulation_cfg_for(media_of_path(db[idx].path));
        return false;
    }
    if (!write_desktop_file(db[idx], err))
        return false;

    system("/usr/sbin/deskscan >/dev/null 2>&1");
    return true;
}

class ServerApp;

class Connection {
public:
    Connection(int fd, ServerApp *app);
    ~Connection();

    void close_connection();

private:
    enum Phase { WAIT_HELLO, WAIT_OFFER, RECEIVING, CLOSED };

    static void read_cb(int, void *v) { static_cast<Connection *>(v)->on_read(); }
    void on_read();
    void handle_frame(uint32_t type, const std::string &payload);
    void handle_hello(const std::string &payload);
    void handle_offer(const std::string &payload);
    void handle_chunk(const std::string &payload);
    void handle_complete(const std::string &payload);
    void handle_deploy_complete(const FileCompleteMsg &fc);

    void handle_put_offer(const std::string &payload);
    void handle_mkdir(const std::string &payload);
    void handle_symlink(const std::string &payload);
    void handle_run(const std::string &payload);
    void handle_query_existing(const std::string &payload);
    void handle_free_space(const std::string &payload);
    void handle_deploy_begin(const std::string &payload);
    void handle_screenshot(const std::string &payload);
    void register_rom(const std::string &rom_path);
    void handle_rom_list(const std::string &payload);
    void handle_rom_delete(const std::string &payload);
    void handle_rom_set_icon(const std::string &payload);
    void handle_rom_get_icon(const std::string &payload);
    void handle_bezel_list(const std::string &payload);
    void handle_bezel_put(const std::string &payload);
    void handle_bezel_delete(const std::string &payload);
    void handle_bezel_get(const std::string &payload);
    void handle_bezel_set_rect(const std::string &payload);
    void handle_rom_set_option(const std::string &payload);

    std::string dest_dir_;
    std::string rom_machine_;
    std::string rom_options_;

    bool send(uint32_t type, const std::string &payload);
    void fail(const std::string &reason);
    static void deferred_delete_cb(void *v);

    ServerApp *app_;
    int fd_;
    FrameReader reader_;
    Phase phase_;

    std::string original_name_;
    std::string final_name_;
    uint64_t total_size_;
    uint64_t next_offset_;
    bool already_fully_done_;
    int row_;
    int part_fd_;

    bool is_deploy_;
    uint32_t deploy_mode_;
    bool deploy_backup_;
    std::string staging_part_path_;
};

struct LocalManagerSpec {
    const char *subdir;
    const char *add_label;
    const char *add_tip;
    const char *pattern;
    const char *chooser_title;
    const char *empty_text;
    const char *reject_hint;
    bool j2me;
    bool want_icon;
};

class LocalManagerPane {
public:
    LocalManagerPane(Fl_Group *page, Fl_Box *status, const LocalManagerSpec &spec)
        : spec_(spec), status_(status), icon_btn_(0), preview_(0), preview_image_(0)
    {
        page->begin();

        media_ = new Fl_Choice(0, 0, 10, 10, "Stored on:");
        media_->align(FL_ALIGN_LEFT);
        media_->add("SD");
        media_->add("CF");
        media_->value(media_present(PART_SD) || !media_present(PART_CF) ? 0 : 1);
        media_->tooltip("Which storage this device keeps these on");

        add_btn_ = new Fl_Button(0, 0, 10, 10, spec_.add_label);
        add_btn_->callback(add_cb, this);
        add_btn_->tooltip(spec_.add_tip);

        delete_btn_ = new Fl_Button(0, 0, 10, 10, "Delete");
        delete_btn_->callback(delete_cb, this);

        if (spec_.want_icon) {
            icon_btn_ = new Fl_Button(0, 0, 10, 10, "Set Icon...");
            icon_btn_->callback(icon_cb, this);
            icon_btn_->tooltip("Choose the icon matchbox-desktop shows for this entry");
        }

        list_ = new Fl_Hold_Browser(0, 0, 10, 10);
        list_->callback(select_cb, this);
        list_->when(FL_WHEN_CHANGED);
        static const int widths[] = { 230, 50, 70, 0 };
        list_->column_widths(widths);
        list_->column_char('\t');

        if (spec_.want_icon) {
            preview_ = new Fl_Box(0, 0, 48, 48);
            preview_->box(FL_DOWN_BOX);
            preview_->color(FL_BACKGROUND2_COLOR);
        }

        page->end();
        page->resizable(0);
    }

    ~LocalManagerPane() { delete preview_image_; }

    void relayout(int X, int Y, int W, int H)
    {
        int m = 8;
        int y = Y + m;

        media_->resize(X + m + 76, y, 84, 24);
        delete_btn_->resize(X + m + 176, y, 80, 24);
        if (icon_btn_)
            icon_btn_->resize(X + m + 264, y, 92, 24);
        add_btn_->resize(X + W - m - 152, y, 152, 24);

        y += 28;
        int list_h = Y + H - y - m;
        if (list_h < 24)
            list_h = 24;
        int list_w = W - 2 * m - (preview_ ? 56 : 0);
        if (list_w < 24)
            list_w = 24;
        list_->resize(X + m, y, list_w, list_h);
        if (preview_)
            preview_->resize(X + W - m - 48, y, 48, 48);
    }

    void refresh()
    {
        int keep = list_->value();
        entries_.clear();
        list_->clear();

        std::vector<RomEntry> db = load_emulation_db();
        for (size_t i = 0; i < db.size(); i++)
            if ((db[i].machine == "J2ME") == spec_.j2me)
                entries_.push_back(db[i]);

        for (size_t i = 0; i < entries_.size(); i++) {
            const RomEntry &e = entries_[i];
            std::string media = option_get(e.options, "media");
            if (media.empty())
                media = part_media_name(media_of_path(e.path));
            std::string label = option_unescape(option_get(e.options, "title"));
            if (label.empty())
                label = strip_extension(basename_of_path(e.path));
            std::string row = label + "\t" + media + "\t" + e.machine + "\t" + e.path;
            list_->add(row.c_str());
        }
        if (entries_.empty())
            list_->add(spec_.empty_text);
        else if (keep >= 1 && (size_t)keep <= entries_.size())
            list_->value(keep);

        show_icon();
    }

private:
    static void delete_cb(Fl_Widget *, void *v) { ((LocalManagerPane *)v)->remove_selected(); }
    static void add_cb(Fl_Widget *, void *v) { ((LocalManagerPane *)v)->add_entries(); }
    static void select_cb(Fl_Widget *, void *v) { ((LocalManagerPane *)v)->show_icon(); }
    static void icon_cb(Fl_Widget *, void *v) { ((LocalManagerPane *)v)->choose_icon(); }

    void status(const std::string &text)
    {
        if (!status_)
            return;
        status_->copy_label(text.c_str());
        status_->redraw();
    }

    const RomEntry *selected() const
    {
        int sel = list_->value();
        if (sel < 1 || (size_t)sel > entries_.size())
            return 0;
        return &entries_[sel - 1];
    }

    std::string start_dir() const
    {
        if (!last_dir_.empty())
            return last_dir_;
        if (media_present(PART_SD))
            return media_mount_point(PART_SD);
        if (media_present(PART_CF))
            return media_mount_point(PART_CF);
        return "/";
    }

    void add_entries()
    {
        Fl_File_Chooser chooser(start_dir().c_str(), spec_.pattern,
                                Fl_File_Chooser::MULTI, spec_.chooser_title);
        chooser.show();
        while (chooser.shown())
            Fl::wait();
        if (!chooser.value(1))
            return;

        int media = media_from_choice(media_->value());
        if (!media_present(media)) {
            fl_alert("%s is not mounted, so nothing can be stored on it.",
                     part_media_name(media));
            return;
        }

        std::string dir = std::string(media_mount_point(media)) + "/" + spec_.subdir;
        if (!mkdir_p(dir)) {
            fl_alert("Cannot create %s", dir.c_str());
            return;
        }

        std::string rejected;
        std::string failed;
        int added = 0;

        for (int i = 1; i <= chooser.count(); i++) {
            const char *path = chooser.value(i);
            if (!path)
                continue;
            last_dir_ = dirname_of(path);

            std::string machine = detect_machine(path);
            bool wanted = spec_.j2me ? (machine == "J2ME")
                                     : (!machine.empty() && machine != "J2ME");
            if (!wanted) {
                rejected += std::string("\n  ") + basename_of_path(path);
                continue;
            }

            std::string dest = dir + "/" + basename_of_path(path);
            if (dest != std::string(path) && !copy_file(path, dest)) {
                failed += std::string("\n  ") + basename_of_path(path)
                        + " (cannot copy to " + dir + ")";
                continue;
            }

            std::string options;
            option_set(options, "media", part_media_name(media));

            std::string icon_png;
            if (spec_.j2me) {
                JarMeta meta;
                if (jar_read_meta(path, meta)) {
                    if (!meta.title.empty())
                        option_set(options, "title", option_escape(meta.title));
                    icon_png = meta.icon_png;
                }
            }

            std::string note;
            if (!register_local_rom(dest, machine, options, note)) {
                failed += std::string("\n  ") + basename_of_path(path) + " (" + note + ")";
                continue;
            }

            if (!icon_png.empty()) {
                std::string err;
                set_local_rom_icon(dest, icon_png, err);
            }

            added++;
            status(note);
        }

        refresh();

        if (added > 1 && rejected.empty() && failed.empty()) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%d added", added);
            status(msg);
        }
        if (!rejected.empty())
            fl_alert("Not added, because they were not recognised:%s\n\n%s",
                     rejected.c_str(), spec_.reject_hint);
        if (!failed.empty())
            fl_alert("Not added:%s", failed.c_str());
    }

    void remove_selected()
    {
        const RomEntry *e = selected();
        if (!e) { fl_alert("Select an entry to delete first."); return; }

        std::string name = strip_extension(basename_of_path(e->path));
        std::string path = e->path;
        if (fl_choice("Delete \"%s\"?\n%s", "Cancel", "Delete", 0,
                      name.c_str(), path.c_str()) != 1)
            return;

        std::string err;
        if (!delete_local_rom(path, err))
            fl_alert("Could not delete %s:\n%s", name.c_str(), err.c_str());
        else
            status("deleted: " + name);
        refresh();
    }

    void show_icon()
    {
        if (!preview_)
            return;

        preview_->image(0);
        delete preview_image_;
        preview_image_ = 0;

        const RomEntry *e = selected();
        if (e && !e->icon.empty()) {
            Fl_PNG_Image *png = new Fl_PNG_Image(e->icon.c_str());
            if (png->w() > 0 && png->h() > 0) {
                if (png->w() > 48 || png->h() > 48) {
                    preview_image_ = png->copy(48, 48);
                    delete png;
                } else {
                    preview_image_ = png;
                }
                preview_->image(preview_image_);
            } else {
                delete png;
            }
        }
        preview_->redraw();
    }

    void choose_icon()
    {
        const RomEntry *e = selected();
        if (!e) { fl_alert("Select an entry first."); return; }
        std::string rom = e->path;

        Fl_File_Chooser chooser(start_dir().c_str(), "PNG icons (*.png)",
                                Fl_File_Chooser::SINGLE, "Choose an icon");
        chooser.show();
        while (chooser.shown())
            Fl::wait();
        if (!chooser.value())
            return;
        last_dir_ = dirname_of(chooser.value());

        std::string png = read_file(chooser.value());
        if (png.size() < 8 || png.compare(1, 3, "PNG") != 0) {
            fl_alert("%s is not a PNG image.", chooser.value());
            return;
        }

        std::string err;
        if (!set_local_rom_icon(rom, png, err)) {
            fl_alert("Could not set the icon:\n%s", err.c_str());
            return;
        }
        status("icon set for " + strip_extension(basename_of_path(rom)));
        refresh();
    }

    LocalManagerSpec spec_;
    Fl_Box *status_;
    Fl_Choice *media_;
    Fl_Button *add_btn_;
    Fl_Button *delete_btn_;
    Fl_Button *icon_btn_;
    Fl_Hold_Browser *list_;
    Fl_Box *preview_;
    Fl_Image *preview_image_;
    std::vector<RomEntry> entries_;
    std::string last_dir_;
};

class ServerApp : public Fl_Group {
public:
    ServerApp(int X, int Y, int W, int H);
    ~ServerApp();

    void resize(int X, int Y, int W, int H)
    {
        Fl_Group::resize(X, Y, W, H);
        relayout();
    }

    void refresh_managers()
    {
        if (roms_)
            roms_->refresh();
        if (midlets_)
            midlets_->refresh();
    }

    void refresh_shown_manager()
    {
        if (tabs_->value() == midlet_page_ && midlets_)
            midlets_->refresh();
        else if (roms_)
            roms_->refresh();
    }

    TransferQueue &queue() { return queue_; }
    TransferMap &transfer_map() { return transfer_map_; }
    const std::vector<std::string> &complete_names() const { return complete_names_; }
    void note_complete_name(const std::string &name) { complete_names_.push_back(name); }
    DeploySession &deploy_session() { return deploy_session_; }

    void set_status(const std::string &text)
    {
        status_label_->copy_label(text.c_str());
        status_label_->redraw();
    }

    void sync_table()
    {
        table_->sync();
        double pct = deploy_session_.active() ? deploy_session_.percent() : queue_.aggregate_percent();
        aggregate_bar_->value(static_cast<float>(pct));
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%d%%", static_cast<int>(pct + 0.5));
        aggregate_bar_->copy_label(lbl);
        aggregate_bar_->redraw();
    }

    void forget_connection(Connection *c)
    {
        for (size_t i = 0; i < connections_.size(); i++) {
            if (connections_[i] == c) {
                connections_.erase(connections_.begin() + i);
                break;
            }
        }
    }

private:
    static void accept_cb(int, void *v) { static_cast<ServerApp *>(v)->on_accept(); }
    void on_accept();

    static void refresh_address_cb(void *v)
    {
        static_cast<ServerApp *>(v)->refresh_address();
        Fl::repeat_timeout(3.0, refresh_address_cb, v);
    }
    void refresh_address();

    void scan_existing();
    void relayout();

    static void tab_cb(Fl_Widget *, void *v)
    {
        static_cast<ServerApp *>(v)->refresh_shown_manager();
    }

    static void toggle_dock_cb(Fl_Widget *, void *v)
    {
        ServerApp *a = static_cast<ServerApp *>(v);
        a->set_dock_open(!a->dock_open_);
    }
    void set_dock_open(bool on)
    {
        dock_open_ = on;
        if (dock_open_)
            table_->show();
        else
            table_->hide();
        toggle_btn_->label(dock_open_ ? "@2>  Transfers" : "@>  Transfers");
        relayout();
        redraw();
    }

    Fl_Box *address_box_;
    Fl_Box *status_label_;
    Fl_Tabs *tabs_;
    Fl_Group *rom_page_;
    Fl_Group *midlet_page_;
    LocalManagerPane *roms_;
    LocalManagerPane *midlets_;
    Fl_Button *toggle_btn_;
    Fl_Progress *aggregate_bar_;
    TransferTable *table_;
    bool dock_open_;
    TransferQueue queue_;
    DeploySession deploy_session_;
    TransferMap transfer_map_;
    std::vector<std::string> complete_names_;
    int listen_fd_;
    std::vector<Connection *> connections_;
};

Connection::Connection(int fd, ServerApp *app)
    : app_(app), fd_(fd), phase_(WAIT_HELLO),
      total_size_(0), next_offset_(0), already_fully_done_(false),
      row_(-1), part_fd_(-1),
      is_deploy_(false), deploy_mode_(0644), deploy_backup_(false)
{
    set_nonblock(fd_);
    Fl::add_fd(fd_, FL_READ, read_cb, this);
}

Connection::~Connection()
{
    if (fd_ >= 0) { Fl::remove_fd(fd_); close(fd_); }
    if (part_fd_ >= 0) close(part_fd_);
}

bool Connection::send(uint32_t type, const std::string &payload)
{
    if (!send_frame_blocking(fd_, type, payload)) {
        close_connection();
        return false;
    }
    return true;
}

void Connection::fail(const std::string &reason)
{
    ErrorMsg em;
    em.message = reason;
    send_frame_blocking(fd_, MSG_ERROR, encode(em));
    close_connection();
}

void Connection::close_connection()
{
    if (phase_ == CLOSED)
        return;
    phase_ = CLOSED;

    if (row_ >= 0) {
        const TransferRow &r = app_->queue().row(row_);
        if (r.status == XFER_TRANSFERRING)
            app_->queue().set_status(row_, XFER_RECONNECTING);
        app_->sync_table();
    }

    if (fd_ >= 0)      { Fl::remove_fd(fd_); close(fd_); fd_ = -1; }
    if (part_fd_ >= 0) { close(part_fd_); part_fd_ = -1; }

    app_->forget_connection(this);

    Fl::add_timeout(0.0, deferred_delete_cb, this);
}

void Connection::deferred_delete_cb(void *v)
{
    delete static_cast<Connection *>(v);
}

void Connection::on_read()
{
    char buf[16384];
    ssize_t n = read(fd_, buf, sizeof(buf));

    if (n == 0) { close_connection(); return; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return;
        close_connection();
        return;
    }

    reader_.feed(buf, static_cast<size_t>(n));

    for (;;) {
        uint32_t type;
        std::string payload;
        FrameReader::Result r = reader_.next(type, payload);
        if (r == FrameReader::NEED_MORE)
            return;
        if (r == FrameReader::DESYNC) {
            close_connection();
            return;
        }
        handle_frame(type, payload);
        if (phase_ == CLOSED)
            return;
    }
}

void Connection::handle_frame(uint32_t type, const std::string &payload)
{
    switch (phase_) {
    case WAIT_HELLO:
        if (type != MSG_HELLO) { fail("expected HELLO"); return; }
        handle_hello(payload);
        return;
    case WAIT_OFFER:
        switch (type) {
        case MSG_FILE_OFFER:       handle_offer(payload); return;
        case MSG_PUT_OFFER:        handle_put_offer(payload); return;
        case MSG_MKDIR:            handle_mkdir(payload); return;
        case MSG_SYMLINK:          handle_symlink(payload); return;
        case MSG_RUN:              handle_run(payload); return;
        case MSG_QUERY_EXISTING:   handle_query_existing(payload); return;
        case MSG_FREE_SPACE:       handle_free_space(payload); return;
        case MSG_DEPLOY_BEGIN:     handle_deploy_begin(payload); return;
        case MSG_SCREENSHOT:       handle_screenshot(payload); return;
        case MSG_ROM_LIST:         handle_rom_list(payload); return;
        case MSG_ROM_DELETE:       handle_rom_delete(payload); return;
        case MSG_ROM_SET_ICON:     handle_rom_set_icon(payload); return;
        case MSG_ROM_GET_ICON:     handle_rom_get_icon(payload); return;
        case MSG_BEZEL_LIST:       handle_bezel_list(payload); return;
        case MSG_BEZEL_PUT:        handle_bezel_put(payload); return;
        case MSG_BEZEL_DELETE:     handle_bezel_delete(payload); return;
        case MSG_BEZEL_GET:        handle_bezel_get(payload); return;
        case MSG_BEZEL_SET_RECT:   handle_bezel_set_rect(payload); return;
        case MSG_ROM_SET_OPTION:   handle_rom_set_option(payload); return;
        default:
            fail("expected an offer or a deploy request");
            return;
        }
    case RECEIVING:
        if (type == MSG_DATA_CHUNK) { handle_chunk(payload); return; }
        if (type == MSG_FILE_COMPLETE) { handle_complete(payload); return; }
        fail("expected DATA_CHUNK or FILE_COMPLETE");
        return;
    case CLOSED:
        return;
    }
}

void Connection::handle_hello(const std::string &payload)
{
    HelloMsg h;
    if (!decode_hello(payload, h)) { fail("malformed HELLO"); return; }
    if (h.version != PROTO_VERSION) {
        char msg[64];
        snprintf(msg, sizeof(msg), "unsupported protocol version %u", h.version);
        fail(msg);
        return;
    }

    HelloAckMsg ack; ack.version = PROTO_VERSION;
    piko_media(ack);
    if (!send(MSG_HELLO_ACK, encode(ack)))
        return;
    phase_ = WAIT_OFFER;
}

void Connection::handle_offer(const std::string &payload)
{
    FileOfferMsg fo;
    if (!decode_file_offer(payload, fo)) { fail("malformed FILE_OFFER"); return; }
    if (fo.name.empty() || fo.name.find('/') != std::string::npos) {
        fail("invalid file name");
        return;
    }
    if (fo.total_size == 0) {
        FileOfferAckMsg ack;
        ack.accepted = false;
        ack.reason = "empty files are not supported";
        send(MSG_FILE_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    dest_dir_ = TRANSFERS_DIR;
    if (!fo.dest_dir.empty()) {
        if (fo.dest_dir[0] != '/') {
            FileOfferAckMsg ack;
            ack.accepted = false;
            ack.reason = "destination must be an absolute path";
            send(MSG_FILE_OFFER_ACK, encode(ack));
            close_connection();
            return;
        }
        std::string want = fo.dest_dir;
        while (want.size() > 1 && want[want.size() - 1] == '/')
            want.erase(want.size() - 1);
        if (!mkdir_p(want) || access(want.c_str(), W_OK) != 0) {
            char msg[320];
            snprintf(msg, sizeof(msg), "cannot write to %s: %s",
                     want.c_str(), strerror(errno));
            FileOfferAckMsg ack;
            ack.accepted = false;
            ack.reason = msg;
            send(MSG_FILE_OFFER_ACK, encode(ack));
            close_connection();
            return;
        }
        dest_dir_ = want;
    }

    original_name_ = fo.name;
    total_size_ = fo.total_size;
    rom_machine_ = fo.rom_machine;
    rom_options_ = fo.rom_options;

    TransferKey key(fo.name, fo.total_size);
    TransferMap::const_iterator it = app_->transfer_map().find(key);
    already_fully_done_ = (it != app_->transfer_map().end() && it->second.complete);

    std::vector<std::string> other_names;
    const std::vector<std::string> *names = &app_->complete_names();
    if (dest_dir_ != TRANSFERS_DIR) {
        names_in_dir(dest_dir_, other_names);
        names = &other_names;
    }

    if (option_get(rom_options_, "overwrite") == "1") {
        final_name_ = fo.name;
        struct stat est;
        std::string existing = dest_dir_ + "/" + final_name_;
        if (stat(existing.c_str(), &est) == 0
            && (uint64_t)est.st_size == total_size_) {
            next_offset_ = total_size_;
            already_fully_done_ = true;
        } else {
            next_offset_ = 0;
        }
    } else {
        OfferDecision d = decide_offer(app_->transfer_map(), fo.name, fo.total_size,
                                        *names);
        final_name_ = d.final_name;
        next_offset_ = d.resume_offset;
    }

    if (next_offset_ < total_size_) {
        std::string part_path = dest_dir_ + "/" + final_name_ + PART_SUFFIX;
        part_fd_ = open(part_path.c_str(), O_CREAT | O_RDWR, 0644);
        if (part_fd_ < 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "cannot open %s: %s", part_path.c_str(), strerror(errno));
            FileOfferAckMsg ack;
            ack.accepted = false;
            ack.reason = msg;
            send(MSG_FILE_OFFER_ACK, encode(ack));
            close_connection();
            return;
        }
    }

    row_ = app_->queue().find_or_add(final_name_, total_size_);
    app_->queue().set_status(row_, XFER_TRANSFERRING);
    app_->queue().set_progress(row_, next_offset_);
    app_->set_status("Receiving " + final_name_ + "...");
    app_->sync_table();

    FileOfferAckMsg ack;
    ack.accepted = true;
    ack.final_name = final_name_;
    ack.resume_offset = next_offset_;
    if (!send(MSG_FILE_OFFER_ACK, encode(ack)))
        return;
    phase_ = RECEIVING;
}

void Connection::handle_chunk(const std::string &payload)
{
    DataChunkMsg dc;
    if (!decode_data_chunk(payload, dc)) { fail("malformed DATA_CHUNK"); return; }

    if (dc.offset != next_offset_) { fail("unexpected chunk offset"); return; }
    if (next_offset_ + dc.data.size() > total_size_) { fail("chunk exceeds file size"); return; }
    if (part_fd_ < 0) { fail("no data expected for this file"); return; }

    ssize_t w = pwrite_retry(part_fd_, dc.data.data(), dc.data.size(),
                              static_cast<off_t>(dc.offset));
    if (w < 0 || static_cast<size_t>(w) != dc.data.size()) {
        char msg[128];
        snprintf(msg, sizeof(msg), "write failed: %s", strerror(errno));
        fail(msg);
        return;
    }

    next_offset_ += dc.data.size();
    if (!is_deploy_)
        app_->transfer_map()[TransferKey(original_name_, total_size_)].bytes_on_disk = next_offset_;
    else
        app_->deploy_session().add_bytes(dc.data.size());

    if (row_ >= 0) {
        app_->queue().set_progress(row_, next_offset_);
        app_->sync_table();
    }

    ChunkAckMsg ack; ack.bytes_written = next_offset_;
    send(MSG_CHUNK_ACK, encode(ack));
}

void Connection::handle_complete(const std::string &payload)
{
    FileCompleteMsg fc;
    if (!decode_file_complete(payload, fc)) { fail("malformed FILE_COMPLETE"); return; }

    if (is_deploy_) {
        handle_deploy_complete(fc);
        return;
    }

    TransferKey key(original_name_, total_size_);

    if (already_fully_done_) {
        FileCompleteAckMsg ack; ack.ok = true;
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        app_->queue().set_status(row_, XFER_DONE);
        app_->queue().set_progress(row_, total_size_);
        app_->sync_table();
        close_connection();
        return;
    }

    if (next_offset_ != total_size_) {
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = "incomplete: not all bytes were received";
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        close_connection();
        return;
    }

    app_->set_status("Verifying " + final_name_ + "...");
    Crc32 crc;
    if (lseek(part_fd_, 0, SEEK_SET) == 0) {
        char buf[65536];
        for (;;) {
            ssize_t n = read(part_fd_, buf, sizeof(buf));
            if (n <= 0) break;
            crc.update(buf, static_cast<size_t>(n));
        }
    }

    if (crc.final_value() != fc.crc32) {
        ftruncate(part_fd_, 0);
        app_->transfer_map()[key].bytes_on_disk = 0;

        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = "checksum mismatch -- please retry";
        send(MSG_FILE_COMPLETE_ACK, encode(ack));

        app_->queue().set_status(row_, XFER_ERROR, "checksum mismatch");
        app_->sync_table();
        close_connection();
        return;
    }

    std::string part_path = dest_dir_ + "/" + final_name_ + PART_SUFFIX;
    std::string final_path = dest_dir_ + "/" + final_name_;
    close(part_fd_);
    part_fd_ = -1;

    if (rename_retry(part_path.c_str(), final_path.c_str()) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "could not finalize: %s", strerror(errno));
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = msg;
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        app_->queue().set_status(row_, XFER_ERROR, msg);
        app_->sync_table();
        close_connection();
        return;
    }

    app_->transfer_map()[key].complete = true;
    app_->note_complete_name(final_name_);

    if (!rom_machine_.empty())
        register_rom(final_path);

    FileCompleteAckMsg ack; ack.ok = true;
    send(MSG_FILE_COMPLETE_ACK, encode(ack));
    app_->queue().set_status(row_, XFER_DONE);
    app_->sync_table();
    close_connection();
}

void Connection::register_rom(const std::string &rom_path)
{
    std::string status;
    register_local_rom(rom_path, rom_machine_, rom_options_, status);
    app_->set_status(status);
    app_->refresh_managers();
}


void Connection::handle_rom_set_option(const std::string &payload)
{
    RomOptionMsg m;
    OkReasonMsg ack;
    if (!decode_rom_option(payload, m)) { fail("malformed ROM_SET_OPTION"); return; }

    if (m.path.empty() || m.key.empty()) {
        ack.ok = false;
        ack.reason = "empty path or key";
        send(MSG_ROM_SET_OPTION_ACK, encode(ack));
        close_connection();
        return;
    }

    std::vector<RomEntry> db = load_emulation_db();
    bool found = false;
    for (size_t i = 0; i < db.size(); i++) {
        if (db[i].path != m.path)
            continue;
        found = true;
        if (m.value.empty())
            option_set(db[i].options, m.key, "");
        else
            option_set(db[i].options, m.key, option_escape(m.value));
        break;
    }

    if (!found) {
        if (m.path[0] != '@') {
            ack.ok = false;
            ack.reason = "no such entry";
            send(MSG_ROM_SET_OPTION_ACK, encode(ack));
            close_connection();
            return;
        }
        if (!m.value.empty()) {
            RomEntry e;
            e.path = m.path;
            e.machine = "-";
            e.backend = "-";
            e.desktop = "-";
            e.icon = "-";
            option_set(e.options, m.key, option_escape(m.value));
            db.push_back(e);
        }
    }

    if (!save_emulation_db(db)) {
        ack.ok = false;
        ack.reason = "could not write emulation.cfg";
    } else {
        ack.ok = true;
        app_->set_status(m.key + " set on " + m.path);
        app_->refresh_managers();
    }
    send(MSG_ROM_SET_OPTION_ACK, encode(ack));
    close_connection();
}

void Connection::handle_bezel_list(const std::string &payload)
{
    (void)payload;
    std::vector<StoredBezel> all = bezel_list_all();
    BezelListAckMsg ack;
    char buf[256];
    for (size_t i = 0; i < all.size(); i++) {
        snprintf(buf, sizeof(buf), "|%u|%u|%u|%u|%u|%u",
                 all[i].master.width, all[i].master.height,
                 all[i].master.screen_x, all[i].master.screen_y,
                 all[i].master.screen_w, all[i].master.screen_h);
        ack.records += all[i].name + buf + "\n";
    }
    send(MSG_BEZEL_LIST_ACK, encode(ack));
    close_connection();
}

void Connection::handle_bezel_put(const std::string &payload)
{
    BezelBlobMsg m;
    OkReasonMsg ack;
    if (!decode_bezel_blob(payload, m)) { fail("malformed BEZEL_PUT"); return; }

    if (!bezel_name_safe(m.name)) {
        ack.ok = false;
        ack.reason = "bad bezel name";
    } else {
        PkbzHeader h;
        size_t off = 0;
        if (!pkbz_decode_header(m.data, h, off)) {
            ack.ok = false;
            ack.reason = "not a valid pkbz blob";
        } else {
            int media = (int)m.media;
            if (media != PART_SD && media != PART_CF && media != PART_NAND)
                media = PART_SD;
            if (!media_present(media)) {
                ack.ok = false;
                ack.reason = std::string(part_media_name(media)) + " is not present";
                send(MSG_BEZEL_PUT_ACK, encode(ack));
                close_connection();
                return;
            }
            if (!bezel_make_dir(media)) {
                ack.ok = false;
                ack.reason = "cannot create bezel directory";
            } else if (!bezel_write_file(bezel_file_for(media, m.name), m.data)) {
                ack.ok = false;
                ack.reason = "cannot write bezel file";
            } else {
                ack.ok = true;
                app_->set_status("bezel " + m.name + " stored");
            }
        }
    }
    send(MSG_BEZEL_PUT_ACK, encode(ack));
    close_connection();
}

void Connection::handle_bezel_delete(const std::string &payload)
{
    BezelBlobMsg m;
    OkReasonMsg ack;
    if (!decode_bezel_blob(payload, m)) { fail("malformed BEZEL_DELETE"); return; }

    if (!bezel_name_safe(m.name)) {
        ack.ok = false;
        ack.reason = "bad bezel name";
    } else {
        int media = bezel_media_of(m.name);
        if (media < 0) {
            ack.ok = false;
            ack.reason = "no such bezel";
        } else {
            unlink(bezel_file_for(media, m.name).c_str());
            ack.ok = true;
            app_->set_status("bezel " + m.name + " deleted");
        }
    }
    send(MSG_BEZEL_DELETE_ACK, encode(ack));
    close_connection();
}

void Connection::handle_bezel_get(const std::string &payload)
{
    BezelBlobMsg m;
    if (!decode_bezel_blob(payload, m)) { fail("malformed BEZEL_GET"); return; }

    std::string blob;
    if (bezel_name_safe(m.name)) {
        int media = bezel_media_of(m.name);
        if (media >= 0)
            bezel_read_file(bezel_file_for(media, m.name), blob);
    }

    if (blob.empty()) {
        BezelChunkMsg empty;
        send(MSG_BEZEL_GET_ACK, encode(empty));
        close_connection();
        return;
    }

    size_t sent = 0;
    while (sent < blob.size()) {
        size_t n = blob.size() - sent;
        if (n > MAX_CHUNK - 64)
            n = MAX_CHUNK - 64;
        BezelChunkMsg c;
        c.total = (uint32_t)blob.size();
        c.offset = (uint32_t)sent;
        c.data.assign(blob, sent, n);
        if (!send(MSG_BEZEL_GET_ACK, encode(c)))
            break;
        sent += n;
    }
    close_connection();
}

void Connection::handle_bezel_set_rect(const std::string &payload)
{
    BezelRectMsg m;
    OkReasonMsg ack;
    if (!decode_bezel_rect(payload, m)) { fail("malformed BEZEL_SET_RECT"); return; }

    if (!bezel_name_safe(m.name)) {
        ack.ok = false;
        ack.reason = "bad bezel name";
    } else {
        int media = bezel_media_of(m.name);
        if (media < 0) {
            ack.ok = false;
            ack.reason = "no such bezel";
        } else {
            bool ok = bezel_patch_rect(bezel_file_for(media, m.name),
                                       m.x, m.y, m.w, m.h);
            ack.ok = ok;
            if (!ok)
                ack.reason = "cannot patch bezel header";
            else
                app_->set_status("bezel " + m.name + " screen rect updated");
        }
    }
    send(MSG_BEZEL_SET_RECT_ACK, encode(ack));
    close_connection();
}

void Connection::handle_rom_list(const std::string &payload)
{
    (void)payload;
    std::vector<RomEntry> db = load_emulation_db();
    RomListAckMsg ack;
    for (size_t i = 0; i < db.size(); i++)
        ack.records += encode_entry(db[i]) + "\n";
    send(MSG_ROM_LIST_ACK, encode(ack));
    close_connection();
}

void Connection::handle_rom_delete(const std::string &payload)
{
    PathMsg m;
    if (!decode_path(payload, m)) { fail("malformed ROM_DELETE"); return; }

    OkReasonMsg ack;
    std::string err;
    if (!delete_local_rom(m.path, err)) {
        ack.ok = false;
        ack.reason = err;
    } else {
        app_->set_status("rom deleted: " + basename_of_path(m.path));
        app_->refresh_managers();
        ack.ok = true;
    }

    send(MSG_ROM_DELETE_ACK, encode(ack));
    close_connection();
}

void Connection::handle_rom_set_icon(const std::string &payload)
{
    RomIconMsg m;
    OkReasonMsg ack;
    if (!decode_rom_icon(payload, m)) { fail("malformed ROM_SET_ICON"); return; }

    std::string err;
    if (!set_local_rom_icon(m.rom_path, m.data, err)) {
        ack.ok = false;
        ack.reason = err;
    } else {
        app_->set_status("icon set for " + basename_of_path(m.rom_path));
        app_->refresh_managers();
        ack.ok = true;
    }

    send(MSG_ROM_SET_ICON_ACK, encode(ack));
    close_connection();
}

void Connection::handle_rom_get_icon(const std::string &payload)
{
    PathMsg m;
    if (!decode_path(payload, m)) { fail("malformed ROM_GET_ICON"); return; }

    std::vector<RomEntry> db = load_emulation_db();
    std::string ipath;
    for (size_t i = 0; i < db.size(); i++)
        if (db[i].path == m.path) { ipath = db[i].icon; break; }

    RomIconMsg out;
    out.rom_path = m.path;
    out.icon_name = ipath;
    if (!ipath.empty()) {
        FILE *f = fopen(ipath.c_str(), "rb");
        if (f) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                out.data.append(buf, n);
            fclose(f);
        }
    }
    send(MSG_ROM_GET_ICON_ACK, encode(out));
    close_connection();
}

void Connection::handle_put_offer(const std::string &payload)
{
    PutOfferMsg po;
    if (!decode_put_offer(payload, po)) { fail("malformed PUT_OFFER"); return; }
    if (po.path.empty() || po.path[0] != '/') { fail("PUT_OFFER path must be absolute"); return; }
    if (po.total_size == 0) {
        PutOfferAckMsg ack;
        ack.outcome = PUT_REJECTED;
        ack.reason = "empty files are not supported";
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    is_deploy_ = true;
    original_name_ = po.path;
    total_size_ = po.total_size;
    deploy_mode_ = po.mode;
    deploy_backup_ = po.backup;

    struct stat st;
    bool exists = (stat(po.path.c_str(), &st) == 0);

    if (po.policy == PUT_IF_MISSING && exists) {
        app_->deploy_session().add_bytes(po.total_size);
        app_->sync_table();
        PutOfferAckMsg ack;
        ack.outcome = PUT_ALREADY_SATISFIED;
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    if (exists && static_cast<uint64_t>(st.st_size) == po.total_size) {
        int fd = open(po.path.c_str(), O_RDONLY);
        if (fd >= 0) {
            Crc32 crc;
            char buf[65536];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                crc.update(buf, static_cast<size_t>(n));
            close(fd);
            if (crc.final_value() == po.crc32) {
                app_->deploy_session().add_bytes(po.total_size);
                app_->sync_table();
                PutOfferAckMsg ack;
                ack.outcome = PUT_ALREADY_SATISFIED;
                send(MSG_PUT_OFFER_ACK, encode(ack));
                close_connection();
                return;
            }
        }
    }

    uint64_t dest_free = 0;
    if (free_bytes_on(po.path, dest_free) && dest_free < po.total_size) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "not enough free space on the destination filesystem "
                 "(need %llu bytes, %llu available)",
                 static_cast<unsigned long long>(po.total_size),
                 static_cast<unsigned long long>(dest_free));
        PutOfferAckMsg ack;
        ack.outcome = PUT_REJECTED;
        ack.reason = msg;
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    std::string staging_dir;
    std::string staging_error;
    if (!resolve_staging_dir(po.staging, staging_dir, staging_error)) {
        PutOfferAckMsg ack;
        ack.outcome = PUT_REJECTED;
        ack.reason = staging_error;
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    staging_part_path_ = staging_dir + "/" + staging_filename_for(po.path) + PART_SUFFIX;
    uint64_t resume_offset = 0;
    struct stat pst;
    if (stat(staging_part_path_.c_str(), &pst) == 0) {
        resume_offset = static_cast<uint64_t>(pst.st_size);
        if (resume_offset > po.total_size)
            resume_offset = 0;
    }

    part_fd_ = open(staging_part_path_.c_str(), O_CREAT | O_RDWR, 0644);
    if (part_fd_ < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot open %s: %s", staging_part_path_.c_str(), strerror(errno));
        PutOfferAckMsg ack;
        ack.outcome = PUT_REJECTED;
        ack.reason = msg;
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    next_offset_ = resume_offset;
    row_ = app_->queue().find_or_add(po.path, po.total_size);
    app_->queue().set_status(row_, XFER_TRANSFERRING);
    app_->queue().set_progress(row_, next_offset_);
    app_->set_status("Receiving " + po.path + "...");
    app_->sync_table();

    PutOfferAckMsg ack;
    ack.outcome = PUT_RESUME;
    ack.resume_offset = next_offset_;
    if (!send(MSG_PUT_OFFER_ACK, encode(ack)))
        return;
    phase_ = RECEIVING;
}

void Connection::handle_deploy_complete(const FileCompleteMsg &fc)
{
    if (next_offset_ != total_size_) {
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = "incomplete: not all bytes were received";
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        close_connection();
        return;
    }

    app_->set_status("Verifying " + original_name_ + "...");
    Crc32 crc;
    if (lseek(part_fd_, 0, SEEK_SET) == 0) {
        char buf[65536];
        for (;;) {
            ssize_t n = read(part_fd_, buf, sizeof(buf));
            if (n <= 0) break;
            crc.update(buf, static_cast<size_t>(n));
        }
    }

    if (crc.final_value() != fc.crc32) {
        ftruncate(part_fd_, 0);

        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = "checksum mismatch -- please retry";
        send(MSG_FILE_COMPLETE_ACK, encode(ack));

        if (row_ >= 0) {
            app_->queue().set_status(row_, XFER_ERROR, "checksum mismatch");
            app_->sync_table();
        }
        close_connection();
        return;
    }

    const std::string &dest = original_name_;
    close(part_fd_);
    part_fd_ = -1;

    if (!mkdir_p(dirname_of(dest))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot create directory for %s: %s", dest.c_str(), strerror(errno));
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = msg;
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        if (row_ >= 0) {
            app_->queue().set_status(row_, XFER_ERROR, msg);
            app_->sync_table();
        }
        close_connection();
        return;
    }

    if (deploy_backup_) {
        struct stat st;
        if (stat(dest.c_str(), &st) == 0)
            rename(dest.c_str(), (dest + ".bak").c_str());
    }

    bool finalized = (rename_retry(staging_part_path_.c_str(), dest.c_str()) == 0);
    if (!finalized && errno == EXDEV) {
        std::string local_tmp = dest + PART_SUFFIX;
        finalized = copy_file(staging_part_path_, local_tmp)
                 && rename_retry(local_tmp.c_str(), dest.c_str()) == 0;
        int finalize_errno = errno;
        if (finalized)
            unlink(staging_part_path_.c_str());
        else
            unlink(local_tmp.c_str());
        errno = finalize_errno;
    }

    if (!finalized) {
        char msg[256];
        snprintf(msg, sizeof(msg), "could not finalize: %s", strerror(errno));
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = msg;
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        if (row_ >= 0) {
            app_->queue().set_status(row_, XFER_ERROR, msg);
            app_->sync_table();
        }
        close_connection();
        return;
    }
    chmod(dest.c_str(), deploy_mode_);

    FileCompleteAckMsg ack;
    ack.ok = true;
    send(MSG_FILE_COMPLETE_ACK, encode(ack));
    if (row_ >= 0) {
        app_->queue().set_status(row_, XFER_DONE);
        app_->sync_table();
    }
    close_connection();
}

void Connection::handle_mkdir(const std::string &payload)
{
    PathMsg m;
    if (!decode_path(payload, m)) { fail("malformed MKDIR"); return; }

    OkReasonMsg ack;
    ack.ok = mkdir_p(m.path);
    if (!ack.ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "mkdir failed: %s", strerror(errno));
        ack.reason = msg;
    }
    send(MSG_MKDIR_ACK, encode(ack));
    close_connection();
}

void Connection::handle_symlink(const std::string &payload)
{
    SymlinkMsg m;
    if (!decode_symlink(payload, m)) { fail("malformed SYMLINK"); return; }

    unlink(m.linkname.c_str());

    OkReasonMsg ack;
    ack.ok = (symlink(m.target.c_str(), m.linkname.c_str()) == 0);
    if (!ack.ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "symlink failed: %s", strerror(errno));
        ack.reason = msg;
    }
    send(MSG_SYMLINK_ACK, encode(ack));
    close_connection();
}

void Connection::handle_run(const std::string &payload)
{
    RunMsg m;
    if (!decode_run(payload, m)) { fail("malformed RUN"); return; }

    OkReasonMsg ack;
    if (m.op == RUN_MOUNT_SD_CARD) {
        system("mount /mnt/card >/dev/null 2>&1");
        ack.ok = is_sd_card_mounted();
        if (!ack.ok)
            ack.reason = "SD card is not mounted";
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "unknown run op %u", m.op);
        ack.ok = false;
        ack.reason = msg;
    }
    send(MSG_RUN_ACK, encode(ack));
    close_connection();
}

void Connection::handle_query_existing(const std::string &payload)
{
    PathMsg m;
    if (!decode_path(payload, m)) { fail("malformed QUERY_EXISTING"); return; }

    struct stat st;
    QueryExistingAckMsg ack;
    if (stat(m.path.c_str(), &st) == 0) {
        ack.exists = true;
        ack.size = static_cast<uint64_t>(st.st_size);
    } else {
        ack.exists = false;
    }
    send(MSG_QUERY_EXISTING_ACK, encode(ack));
    close_connection();
}

void Connection::handle_free_space(const std::string &payload)
{
    PathMsg m;
    if (!decode_path(payload, m)) { fail("malformed FREE_SPACE"); return; }

    FreeSpaceAckMsg ack;
    free_bytes_on(m.path, ack.free_bytes);
    send(MSG_FREE_SPACE_ACK, encode(ack));
    close_connection();
}

static bool capture_framebuffer(std::string &out, uint32_t &width,
                                uint32_t &height, uint32_t &bpp,
                                std::string &err)
{
    const char *dev = "/dev/fb0";
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        err = std::string("cannot open ") + dev + ": " + strerror(errno);
        return false;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        err = std::string("framebuffer ioctl: ") + strerror(errno);
        close(fd);
        return false;
    }
    if (var.bits_per_pixel != 16) {
        char buf[96];
        snprintf(buf, sizeof(buf), "unsupported depth: %u bpp (expected 16)",
                 var.bits_per_pixel);
        err = buf;
        close(fd);
        return false;
    }

    size_t rowbytes = static_cast<size_t>(var.xres) * var.bits_per_pixel / 8;
    size_t maplen = static_cast<size_t>(fix.line_length) * (var.yres + var.yoffset);
    unsigned char *fb = static_cast<unsigned char *>(
        mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, 0));
    if (fb == MAP_FAILED) {
        err = std::string("mmap framebuffer: ") + strerror(errno);
        close(fd);
        return false;
    }

    out.clear();
    out.reserve(rowbytes * var.yres);
    for (unsigned int y = 0; y < var.yres; y++) {
        const unsigned char *row = fb
            + static_cast<size_t>(y + var.yoffset) * fix.line_length
            + static_cast<size_t>(var.xoffset) * var.bits_per_pixel / 8;
        out.append(reinterpret_cast<const char *>(row), rowbytes);
    }

    munmap(fb, maplen);
    close(fd);

    width = var.xres;
    height = var.yres;
    bpp = var.bits_per_pixel;
    return true;
}

void Connection::handle_screenshot(const std::string &payload)
{
    (void)payload;

    std::string pixels, err;
    ScreenshotInfoMsg info;
    if (!capture_framebuffer(pixels, info.width, info.height, info.bpp, err)) {
        info.ok = false;
        info.reason = err;
        send(MSG_SCREENSHOT_INFO, encode(info));
        close_connection();
        return;
    }

    info.ok = true;
    info.byte_count = static_cast<uint32_t>(pixels.size());
    send(MSG_SCREENSHOT_INFO, encode(info));

    Crc32 crc;
    size_t off = 0;
    while (off < pixels.size()) {
        size_t n = pixels.size() - off;
        if (n > MAX_CHUNK)
            n = MAX_CHUNK;
        DataChunkMsg chunk;
        chunk.offset = off;
        chunk.data.assign(pixels, off, n);
        crc.update(pixels.data() + off, n);
        if (!send_frame_blocking(fd_, MSG_DATA_CHUNK, encode(chunk)))
            return;
        off += n;
    }

    FileCompleteMsg done;
    done.crc32 = crc.final_value();
    send(MSG_FILE_COMPLETE, encode(done));
    close_connection();
}

void Connection::handle_deploy_begin(const std::string &payload)
{
    DeployBeginMsg m;
    if (!decode_deploy_begin(payload, m)) { fail("malformed DEPLOY_BEGIN"); return; }

    app_->deploy_session().begin(m.total_bytes);
    app_->sync_table();

    OkReasonMsg ack; ack.ok = true;
    send(MSG_DEPLOY_BEGIN_ACK, encode(ack));
    close_connection();
}

ServerApp::ServerApp(int X, int Y, int W, int H)
    : Fl_Group(X, Y, W, H), roms_(0), midlets_(0), dock_open_(false), listen_fd_(-1)
{
    int m = 8;

    address_box_ = new Fl_Box(X + m, Y + 4, W - 2 * m, 18);
    address_box_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    address_box_->labelsize(12);
    address_box_->label("starting...");

    status_label_ = new Fl_Box(X + m, Y + 24, W - 2 * m, 16);
    status_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    status_label_->labelfont(FL_HELVETICA_ITALIC);
    status_label_->labelsize(12);
    status_label_->label("");

    tabs_ = new Fl_Tabs(X, Y + HEADER_H, W, H - HEADER_H - DOCK_SHUT_H);
    tabs_->begin();

    LocalManagerSpec rom_spec;
    rom_spec.subdir = "Emulation";
    rom_spec.add_label = "Add ROM...";
    rom_spec.add_tip = "Move a ROM into storage and register it as a game";
    rom_spec.pattern = "ROM files (*.{smc,sfc,fig,swc})";
    rom_spec.chooser_title = "Add ROMs";
    rom_spec.empty_text = "no roms registered yet";
    rom_spec.reject_hint = "A ROM is recognised by its header, not only by its name.";
    rom_spec.j2me = false;
    rom_spec.want_icon = true;

    rom_page_ = new Fl_Group(X, Y + HEADER_H + TABBAR_H, W,
                             tabs_->h() - TABBAR_H, "ROMs");
    roms_ = new LocalManagerPane(rom_page_, status_label_, rom_spec);

    LocalManagerSpec midlet_spec;
    midlet_spec.subdir = "Applets";
    midlet_spec.add_label = "Add J2ME applet...";
    midlet_spec.add_tip = "Move a MIDlet into storage and register it";
    midlet_spec.pattern = "J2ME applets (*.{jar,jad})";
    midlet_spec.chooser_title = "Add J2ME applets";
    midlet_spec.empty_text = "no applets installed yet";
    midlet_spec.reject_hint = "A MIDlet is a .jar with a MIDlet manifest, or its .jad descriptor.";
    midlet_spec.j2me = true;
    midlet_spec.want_icon = false;

    midlet_page_ = new Fl_Group(X, Y + HEADER_H + TABBAR_H, W,
                                tabs_->h() - TABBAR_H, "J2ME");
    midlets_ = new LocalManagerPane(midlet_page_, status_label_, midlet_spec);

    tabs_->end();
    tabs_->resizable(0);
    tabs_->callback(tab_cb, this);
    tabs_->when(FL_WHEN_CHANGED);

    toggle_btn_ = new Fl_Button(X + m, Y + H - DOCK_SHUT_H, 120, 22, "@>  Transfers");
    toggle_btn_->box(FL_FLAT_BOX);
    toggle_btn_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    toggle_btn_->labelsize(12);
    toggle_btn_->callback(toggle_dock_cb, this);

    aggregate_bar_ = new Fl_Progress(X + m + 126, Y + H - DOCK_SHUT_H + 1,
                                     W - 2 * m - 126, 20);
    aggregate_bar_->minimum(0);
    aggregate_bar_->maximum(100);
    aggregate_bar_->value(0);
    aggregate_bar_->color(FL_BACKGROUND_COLOR);
    aggregate_bar_->selection_color(FL_BLUE);
    aggregate_bar_->label("0%");

    table_ = new TransferTable(X + m, Y + H, W - 2 * m, 1);
    table_->queue(&queue_);
    table_->hide();

    end();
    resizable(0);
    relayout();

    mkdir(TRANSFERS_DIR, 0755);

    migrate_legacy_dbs();
    if (sync_rom_launchers() > 0)
        system("/usr/sbin/deskscan >/dev/null 2>&1");
    refresh_managers();

    if (access(TRANSFERS_DIR, W_OK) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "SD card not available: %s is not writable", TRANSFERS_DIR);
        address_box_->copy_label(msg);
        return;
    }

    scan_existing();
    sync_table();

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ >= 0) {
        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(DEFAULT_PORT);

        if (bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0
            && listen(listen_fd_, 16) == 0) {
            set_nonblock(listen_fd_);
            Fl::add_fd(listen_fd_, FL_READ, accept_cb, this);
        } else {
            close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    refresh_address();
    Fl::add_timeout(3.0, refresh_address_cb, this);
}

void ServerApp::relayout()
{
    int m = 8;
    int X = x(), Y = y(), W = w(), H = h();

    address_box_->resize(X + m, Y + 4, W - 2 * m, 18);
    status_label_->resize(X + m, Y + 24, W - 2 * m, 16);

    int dock_h = dock_open_ ? DOCK_OPEN_H : DOCK_SHUT_H;
    int tabs_h = H - HEADER_H - dock_h;
    if (tabs_h < TABBAR_H + 24)
        tabs_h = TABBAR_H + 24;

    tabs_->resize(X, Y + HEADER_H, W, tabs_h);
    rom_page_->resize(X, Y + HEADER_H + TABBAR_H, W, tabs_h - TABBAR_H);
    midlet_page_->resize(X, Y + HEADER_H + TABBAR_H, W, tabs_h - TABBAR_H);
    roms_->relayout(X, Y + HEADER_H + TABBAR_H, W, tabs_h - TABBAR_H);
    midlets_->relayout(X, Y + HEADER_H + TABBAR_H, W, tabs_h - TABBAR_H);

    int dy = Y + HEADER_H + tabs_h;
    toggle_btn_->resize(X + m, dy + 2, 120, 22);
    aggregate_bar_->resize(X + m + 126, dy + 3, W - 2 * m - 126, 20);

    int table_h = dock_h - 28 - m;
    if (table_h < 1)
        table_h = 1;
    table_->resize(X + m, dy + 28, W - 2 * m, table_h);
}

ServerApp::~ServerApp()
{
    delete roms_;
    delete midlets_;
    while (!connections_.empty())
        connections_.back()->close_connection();
    if (listen_fd_ >= 0) {
        Fl::remove_fd(listen_fd_);
        close(listen_fd_);
    }
    Fl::remove_timeout(refresh_address_cb, this);
}

void ServerApp::scan_existing()
{
    DIR *d = opendir(TRANSFERS_DIR);
    if (!d)
        return;

    std::string suffix(PART_SUFFIX);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        std::string name(e->d_name);
        if (name == "." || name == "..")
            continue;
        if (name.size() >= suffix.size()
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            continue;

        std::string path = std::string(TRANSFERS_DIR) + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        int row = queue_.add(name, static_cast<uint64_t>(st.st_size));
        queue_.set_status(row, XFER_DONE);
        queue_.set_progress(row, static_cast<uint64_t>(st.st_size));
        complete_names_.push_back(name);
    }
    closedir(d);
}

void ServerApp::refresh_address()
{
    std::string ip = wlan0_address();
    char msg[256];
    if (ip.empty()) {
        snprintf(msg, sizeof(msg), "Waiting for WiFi (wlan0)...");
    } else if (listen_fd_ < 0) {
        snprintf(msg, sizeof(msg), "%s -- could not start listening on port %u",
                 ip.c_str(), static_cast<unsigned>(DEFAULT_PORT));
    } else {
        snprintf(msg, sizeof(msg), "Listening on %s:%u", ip.c_str(),
                 static_cast<unsigned>(DEFAULT_PORT));
    }
    address_box_->copy_label(msg);
    address_box_->redraw();
}

void ServerApp::on_accept()
{
    for (;;) {
        int fd = accept(listen_fd_, 0, 0);
        if (fd < 0)
            return;
        connections_.push_back(new Connection(fd, this));
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    if (argc > 1 && strcmp(argv[1], "--resync") == 0) {
        migrate_legacy_dbs();
        if (sync_rom_launchers() > 0)
            system("/usr/sbin/deskscan >/dev/null 2>&1");
        return 0;
    }

    Fl_Double_Window win(640, 480, "Piko Sync");
    win.begin();
    ServerApp app(0, 0, 640, 480);
    win.end();
    win.resizable(&app);

    static Fl_Pixmap icon_pixmap(piko_sync_icon_xpm);
    static Fl_RGB_Image icon_img(&icon_pixmap);
    win.icon(&icon_img);

    win.show(argc, argv);

    return Fl::run();
}
