#include "pikorom.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "rom_detect.h"
#include "emulation_db.h"
#include "jar_meta.h"
#include "bezel_format.h"
#include "bezel_store.h"
#include "device_info.h"

using namespace piko_sync;

namespace {

ssize_t write_retry(int fd, const void *buf, size_t len)
{
    const char *p = static_cast<const char *>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += static_cast<size_t>(n);
    }
    return static_cast<ssize_t>(done);
}

int open_retry(const char *path, int flags, mode_t mode)
{
    for (;;) {
        int fd = open(path, flags, mode);
        if (fd >= 0 || errno != EINTR)
            return fd;
    }
}

std::string dirname_of(const std::string &p)
{
    std::string::size_type s = p.find_last_of('/');
    if (s == std::string::npos)
        return std::string(".");
    if (s == 0)
        return std::string("/");
    return p.substr(0, s);
}

bool mkdir_p(const std::string &path)
{
    if (path.empty() || path == "/" || path == ".")
        return true;
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    if (!mkdir_p(dirname_of(path)))
        return false;
    return mkdir(path.c_str(), 0755) == 0;
}

std::string read_file(const std::string &path)
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

void copy_out(const std::string &s, char *out, size_t outlen)
{
    if (!out || outlen == 0)
        return;
    size_t n = s.size();
    if (n >= outlen)
        n = outlen - 1;
    memcpy(out, s.data(), n);
    out[n] = '\0';
}

bool write_desktop_file(const RomEntry &e, std::string &err)
{
    std::string adir = applications_dir_for(media_of_path(e.path));
    mkdir_p(adir);
    std::string dpath = adir + "/" + e.desktop;
    std::string contents = desktop_contents(e);
    int fd = open_retry(dpath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { err = "cannot write " + dpath; return false; }
    bool ok = write_retry(fd, contents.data(), contents.size())
              == static_cast<ssize_t>(contents.size());
    close(fd);
    if (!ok) { err = dpath + " is truncated"; return false; }
    return true;
}

static int prune_rom_launchers_in(const std::string &adir, const std::vector<RomEntry> &db)
{
    DIR *d = opendir(adir.c_str());
    if (!d)
        return 0;

    int removed = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        std::string name(e->d_name);
        if (name == "." || name == "..")
            continue;

        std::string dpath = adir + "/" + name;
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

int prune_rom_launchers(const std::vector<RomEntry> &db)
{
    int removed = 0;
    for (int m = PART_NAND; m <= PART_CF; m++)
        removed += prune_rom_launchers_in(applications_dir_for(m), db);
    return removed;
}

void run_deskscan()
{
    system("/usr/sbin/deskscan >/dev/null 2>&1");
}

}

struct pikorom_db {
    std::vector<RomEntry> rows;
    std::vector<pikorom_entry> view;
};

struct pikorom_blob {
    std::string data;
};

struct pikorom_bezel_list {
    std::vector<StoredBezel> rows;
    std::vector<pikorom_bezel> view;
};

extern "C" {

const char *pikorom_media_name(int media)
{
    return part_media_name(media);
}

int pikorom_media_of_path(const char *path)
{
    return path ? media_of_path(path) : -1;
}

int pikorom_media_present(int media)
{
    return media_present(media) ? 1 : 0;
}

pikorom_db *pikorom_db_open(void)
{
    pikorom_db *db = new pikorom_db;
    db->rows = load_emulation_db();
    db->view.resize(db->rows.size());
    for (size_t i = 0; i < db->rows.size(); i++) {
        db->view[i].path = db->rows[i].path.c_str();
        db->view[i].machine = db->rows[i].machine.c_str();
        db->view[i].backend = db->rows[i].backend.c_str();
        db->view[i].desktop = db->rows[i].desktop.c_str();
        db->view[i].icon = db->rows[i].icon.c_str();
        db->view[i].options = db->rows[i].options.c_str();
    }
    return db;
}

void pikorom_db_close(pikorom_db *db)
{
    delete db;
}

int pikorom_db_count(const pikorom_db *db)
{
    return db ? static_cast<int>(db->rows.size()) : 0;
}

const struct pikorom_entry *pikorom_db_at(const pikorom_db *db, int index)
{
    if (!db || index < 0 || index >= static_cast<int>(db->view.size()))
        return 0;
    return &db->view[index];
}

int pikorom_db_find(const pikorom_db *db, const char *rom_path)
{
    if (!db || !rom_path)
        return -1;
    for (size_t i = 0; i < db->rows.size(); i++)
        if (db->rows[i].path == rom_path)
            return static_cast<int>(i);
    return -1;
}

pikorom_blob *pikorom_db_records(const pikorom_db *db)
{
    pikorom_blob *b = new pikorom_blob;
    if (db)
        for (size_t i = 0; i < db->rows.size(); i++)
            b->data += encode_entry(db->rows[i]) + "\n";
    return b;
}

int pikorom_cfg_path_for(const char *rom_path, char *out, size_t outlen)
{
    if (!rom_path)
        return 0;
    copy_out(emulation_cfg_for(media_of_path(rom_path)), out, outlen);
    return 1;
}

int pikorom_media_root_for(const char *rom_path, char *out, size_t outlen)
{
    if (!rom_path)
        return 0;
    copy_out(media_zaurus_root(media_of_path(rom_path)), out, outlen);
    return 1;
}

int pikorom_entry_lookup(const char *cfg_path, const char *key,
                         char *machine, size_t machinelen,
                         char *backend, size_t backendlen,
                         char *options, size_t optionslen)
{
    copy_out(std::string(), machine, machinelen);
    copy_out(std::string(), backend, backendlen);
    copy_out(std::string(), options, optionslen);

    if (!cfg_path || !key)
        return 0;

    std::vector<RomEntry> rows = load_emulation_file(cfg_path);
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].path != key)
            continue;
        copy_out(rows[i].machine, machine, machinelen);
        copy_out(rows[i].backend, backend, backendlen);
        copy_out(rows[i].options, options, optionslen);
        return 1;
    }
    return 0;
}

void pikorom_option_unescape(const char *in, char *out, size_t outlen)
{
    copy_out(in ? option_unescape(in) : std::string(), out, outlen);
}

int pikorom_detect_machine(const char *path, char *out, size_t outlen)
{
    if (!path)
        return 0;
    std::string m = detect_machine(path);
    copy_out(m, out, outlen);
    return m.empty() ? 0 : 1;
}

const char *pikorom_backend_for(const char *machine)
{
    if (!machine)
        return "";
    static std::string held;
    held = machine_backend(machine);
    return held.c_str();
}

int pikorom_is_directive(const char *rom_path)
{
    return (rom_path && rom_path[0] == '@') ? 1 : 0;
}

int pikorom_install(const char *rom_path, const char *machine, const char *options,
                    char *status, size_t statuslen)
{
    if (!rom_path || !machine) {
        copy_out("no rom path", status, statuslen);
        return 0;
    }

    std::string opts = options ? options : "";

    RomEntry e;
    e.path = rom_path;
    e.machine = machine;
    e.backend = machine_backend(e.machine);
    if (e.backend.empty()) {
        copy_out("no emulator backend for " + e.machine + " -- "
                 + basename_of_path(e.path) + " is stored but not registered",
                 status, statuslen);
        return 0;
    }
    e.desktop = desktop_name_for(e.machine, e.path);
    e.icon = DEFAULT_ROM_ICON;

    std::string media = option_get(opts, "media");
    if (!media.empty())
        option_set(e.options, "media", media);
    if (option_get(opts, "heavy") == "1" || e.machine != "J2ME")
        option_set(e.options, "heavy", "1");
    std::string title = option_get(opts, "title");

    if (e.machine == "J2ME") {
        JarMeta meta;
        if (jar_read_meta(e.path, meta)) {
            if (title.empty() && !meta.title.empty())
                title = option_escape(meta.title);
            if (!meta.icon_png.empty()) {
                std::string ipath = icon_path_for(e.machine, e.path);
                mkdir_p(dirname_of(ipath));
                int fd = open_retry(ipath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
                if (fd >= 0) {
                    if (write_retry(fd, meta.icon_png.data(), meta.icon_png.size())
                        == (ssize_t)meta.icon_png.size())
                        e.icon = ipath;
                    close(fd);
                }
            }
        }
    }

    if (!title.empty())
        option_set(e.options, "title", title);
    if (e.machine == "J2ME" && option_get(opts, "rotate") == "1")
        option_set(e.options, "rotate", "1");

    std::vector<RomEntry> db = load_emulation_db();
    for (size_t i = 0; i < db.size(); i++)
        if (db[i].path == e.path) { db.erase(db.begin() + i); break; }
    db.push_back(e);

    if (!save_emulation_db(db)) {
        copy_out("rom registered but " + emulation_cfg_for(media_of_path(e.path))
                 + " is not writable", status, statuslen);
        return 0;
    }

    std::string err;
    if (!write_desktop_file(e, err)) {
        copy_out("rom registered but " + err, status, statuslen);
        return 0;
    }

    run_deskscan();
    copy_out(e.machine + " rom registered: " + e.desktop, status, statuslen);
    return 1;
}

int pikorom_remove(const char *rom_path, char *err, size_t errlen)
{
    if (!rom_path) {
        copy_out("no rom path", err, errlen);
        return 0;
    }

    std::vector<RomEntry> db = load_emulation_db();
    RomEntry found;
    bool have = false;
    for (size_t i = 0; i < db.size(); i++) {
        if (db[i].path == rom_path) {
            found = db[i];
            have = true;
            db.erase(db.begin() + i);
            break;
        }
    }
    if (!have) {
        copy_out("no such rom on any mounted media", err, errlen);
        return 0;
    }

    if (unlink(found.path.c_str()) != 0 && errno != ENOENT) {
        copy_out("cannot delete " + found.path + ": " + strerror(errno), err, errlen);
        return 0;
    }

    if (!found.desktop.empty())
        unlink((applications_dir_for(media_of_path(found.path)) + "/" + found.desktop).c_str());

    if (!save_emulation_db(db)) {
        copy_out("rom deleted but " + emulation_cfg_for(media_of_path(found.path))
                 + " is not writable", err, errlen);
        return 0;
    }

    run_deskscan();
    return 1;
}

int pikorom_set_icon(const char *rom_path, const void *png, size_t len,
                     char *err, size_t errlen)
{
    if (!rom_path || (!png && len)) {
        copy_out("no rom path", err, errlen);
        return 0;
    }

    std::vector<RomEntry> db = load_emulation_db();
    int idx = -1;
    for (size_t i = 0; i < db.size(); i++)
        if (db[i].path == rom_path) { idx = static_cast<int>(i); break; }
    if (idx < 0) {
        copy_out("no such rom on any mounted media", err, errlen);
        return 0;
    }

    std::string ipath = icon_path_for(db[idx].machine, db[idx].path);
    mkdir_p(pixmaps_dir_for(media_of_path(db[idx].path)));
    int fd = open_retry(ipath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        copy_out("cannot write " + ipath + ": " + strerror(errno), err, errlen);
        return 0;
    }
    bool ok = write_retry(fd, png, len) == static_cast<ssize_t>(len);
    close(fd);
    if (!ok) {
        copy_out(ipath + " is truncated", err, errlen);
        return 0;
    }

    db[idx].icon = ipath;
    if (!save_emulation_db(db)) {
        copy_out("cannot update " + emulation_cfg_for(media_of_path(db[idx].path)),
                 err, errlen);
        return 0;
    }

    std::string werr;
    if (!write_desktop_file(db[idx], werr)) {
        copy_out(werr, err, errlen);
        return 0;
    }

    run_deskscan();
    return 1;
}

int pikorom_set_option(const char *rom_path, const char *key, const char *value,
                       char *err, size_t errlen)
{
    if (!rom_path || !*rom_path || !key || !*key) {
        copy_out("empty path or key", err, errlen);
        return 0;
    }

    std::string val = value ? value : "";
    std::vector<RomEntry> db = load_emulation_db();

    bool found = false;
    for (size_t i = 0; i < db.size(); i++) {
        if (db[i].path != rom_path)
            continue;
        found = true;
        option_set(db[i].options, key, val.empty() ? std::string() : option_escape(val));
        break;
    }

    if (!found) {
        if (rom_path[0] != '@') {
            copy_out("no such entry", err, errlen);
            return 0;
        }
        if (!val.empty()) {
            RomEntry e;
            e.path = rom_path;
            e.machine = "-";
            e.backend = "-";
            e.desktop = "-";
            e.icon = "-";
            option_set(e.options, key, option_escape(val));
            db.push_back(e);
        }
    }

    if (!save_emulation_db(db)) {
        copy_out("could not write emulation.cfg", err, errlen);
        return 0;
    }
    return 1;
}

int pikorom_set_backend(const char *rom_path, const char *backend,
                        char *err, size_t errlen)
{
    if (!rom_path || !*rom_path || !backend || !*backend) {
        copy_out("empty path or backend", err, errlen);
        return 0;
    }

    std::vector<RomEntry> db = load_emulation_db();

    bool found = false;
    for (size_t i = 0; i < db.size(); i++) {
        if (db[i].path == rom_path) {
            found = true;
            db[i].backend = backend;
            break;
        }
    }

    if (!found) {
        copy_out("no such entry", err, errlen);
        return 0;
    }

    if (!save_emulation_db(db)) {
        copy_out("could not write emulation.cfg", err, errlen);
        return 0;
    }
    return 1;
}

int pikorom_apply(const char *rom_path, const char *backend,
                  const char *const *keys, const char *const *values, int option_count,
                  const void *icon, size_t icon_len,
                  char *err, size_t errlen)
{
    if (!rom_path || !*rom_path) {
        copy_out("empty path", err, errlen);
        return 0;
    }

    std::vector<RomEntry> db = load_emulation_db();
    int idx = -1;
    for (size_t i = 0; i < db.size(); i++)
        if (db[i].path == rom_path) { idx = static_cast<int>(i); break; }

    if (idx < 0) {
        copy_out("no such entry", err, errlen);
        return 0;
    }

    if (backend && *backend)
        db[idx].backend = backend;

    for (int i = 0; i < option_count; i++) {
        if (!keys[i] || !*keys[i])
            continue;
        std::string value = values[i] ? values[i] : "";
        option_set(db[idx].options, keys[i],
                   value.empty() ? std::string() : option_escape(value));
    }

    if (icon && icon_len > 0) {
        std::string ipath = icon_path_for(db[idx].machine, db[idx].path);
        mkdir_p(dirname_of(ipath));
        int fd = open_retry(ipath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
            copy_out("cannot write " + ipath + ": " + strerror(errno), err, errlen);
            return 0;
        }
        bool ok = write_retry(fd, icon, icon_len) == static_cast<ssize_t>(icon_len);
        close(fd);
        if (!ok) {
            copy_out("icon is truncated", err, errlen);
            return 0;
        }
        db[idx].icon = ipath;
    }

    if (!save_emulation_db(db)) {
        copy_out("could not write emulation.cfg", err, errlen);
        return 0;
    }

    std::string werr;
    if (!write_desktop_file(db[idx], werr)) {
        copy_out(werr, err, errlen);
        return 0;
    }
    run_deskscan();
    return 1;
}

int pikorom_option_get(const char *options, const char *key, char *out, size_t outlen)
{
    if (!options || !key)
        return 0;
    std::string v = option_get(options, key);
    copy_out(v, out, outlen);
    return v.empty() ? 0 : 1;
}

int pikorom_sync_launchers(void)
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
        std::string dpath = applications_dir_for(media_of_path(db[i].path)) + "/" + db[i].desktop;
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

void pikorom_migrate_legacy(void)
{
    migrate_legacy_emulation_db();
}

int pikorom_read_jar_meta(const char *path, struct pikorom_jar_meta *out)
{
    if (!path || !out)
        return 0;
    JarMeta m;
    if (!jar_read_meta(path, m))
        return 0;

    char *title = strdup(m.title.c_str());
    char *icon_name = strdup(m.icon_name.c_str());
    void *png = 0;
    if (!m.icon_png.empty()) {
        png = malloc(m.icon_png.size());
        if (png)
            memcpy(png, m.icon_png.data(), m.icon_png.size());
    }

    out->title = title;
    out->icon_name = icon_name;
    out->icon_png = png;
    out->icon_png_len = png ? m.icon_png.size() : 0;
    return 1;
}

void pikorom_free_jar_meta(struct pikorom_jar_meta *meta)
{
    if (!meta)
        return;
    free(const_cast<char *>(meta->title));
    free(const_cast<char *>(meta->icon_name));
    free(const_cast<void *>(meta->icon_png));
    meta->title = 0;
    meta->icon_name = 0;
    meta->icon_png = 0;
    meta->icon_png_len = 0;
}

pikorom_blob *pikorom_blob_read(const char *path)
{
    if (!path)
        return 0;
    pikorom_blob *b = new pikorom_blob;
    if (!bezel_read_file(path, b->data)) {
        delete b;
        return 0;
    }
    return b;
}

const void *pikorom_blob_data(const pikorom_blob *b)
{
    return b ? b->data.data() : 0;
}

size_t pikorom_blob_size(const pikorom_blob *b)
{
    return b ? b->data.size() : 0;
}

void pikorom_blob_free(pikorom_blob *b)
{
    delete b;
}

pikorom_bezel_list *pikobezel_list(void)
{
    pikorom_bezel_list *l = new pikorom_bezel_list;
    l->rows = bezel_list_all();
    l->view.resize(l->rows.size());
    for (size_t i = 0; i < l->rows.size(); i++) {
        l->view[i].name = l->rows[i].name.c_str();
        l->view[i].media = l->rows[i].media;
        l->view[i].width = l->rows[i].master.width;
        l->view[i].height = l->rows[i].master.height;
        l->view[i].screen_x = l->rows[i].master.screen_x;
        l->view[i].screen_y = l->rows[i].master.screen_y;
        l->view[i].screen_w = l->rows[i].master.screen_w;
        l->view[i].screen_h = l->rows[i].master.screen_h;
        l->view[i].source = l->rows[i].master.source.c_str();
    }
    return l;
}

int pikobezel_count(const pikorom_bezel_list *l)
{
    return l ? static_cast<int>(l->rows.size()) : 0;
}

const struct pikorom_bezel *pikobezel_at(const pikorom_bezel_list *l, int index)
{
    if (!l || index < 0 || index >= static_cast<int>(l->view.size()))
        return 0;
    return &l->view[index];
}

void pikobezel_list_free(pikorom_bezel_list *l)
{
    delete l;
}

int pikobezel_name_safe(const char *name)
{
    return (name && bezel_name_safe(name)) ? 1 : 0;
}

int pikobezel_media_of(const char *name)
{
    return name ? bezel_media_of(name) : -1;
}

int pikobezel_path_for(int media, const char *name, char *out, size_t outlen)
{
    if (!name || !bezel_name_safe(name))
        return 0;
    copy_out(bezel_file_for(media, name), out, outlen);
    return 1;
}

pikorom_blob *pikobezel_read(const char *name)
{
    if (!name || !bezel_name_safe(name))
        return 0;
    int media = bezel_media_of(name);
    if (media < 0)
        return 0;
    return pikorom_blob_read(bezel_file_for(media, name).c_str());
}

pikorom_blob *pikobezel_records(const pikorom_bezel_list *l)
{
    pikorom_blob *b = new pikorom_blob;
    if (!l)
        return b;
    char buf[256];
    for (size_t i = 0; i < l->rows.size(); i++) {
        const PkbzHeader &h = l->rows[i].master;
        uint32_t crc = cached_file_crc32(bezel_file_for(l->rows[i].media, l->rows[i].name));
        snprintf(buf, sizeof(buf), "|%u|%u|%u|%u|%u|%u|%u",
                 h.width, h.height, h.screen_x, h.screen_y, h.screen_w, h.screen_h, crc);
        b->data += l->rows[i].name + buf + "\n";
    }
    return b;
}

int pikobezel_write(int media, const char *name, const void *data, size_t len,
                    char *err, size_t errlen)
{
    if (!name || !bezel_name_safe(name)) {
        copy_out("bad bezel name", err, errlen);
        return 0;
    }
    if (!data && len) {
        copy_out("no bezel data", err, errlen);
        return 0;
    }

    std::string blob(static_cast<const char *>(data), len);

    PkbzHeader h;
    size_t off = 0;
    if (!pkbz_decode_header(blob, h, off)) {
        copy_out("not a valid pkbz blob", err, errlen);
        return 0;
    }

    if (media != PART_SD && media != PART_CF && media != PART_NAND)
        media = PART_SD;
    if (!media_present(media)) {
        copy_out(std::string(part_media_name(media)) + " is not present", err, errlen);
        return 0;
    }
    if (!bezel_make_dir(media)) {
        copy_out("cannot create bezel directory", err, errlen);
        return 0;
    }
    if (!bezel_write_file(bezel_file_for(media, name), blob)) {
        copy_out("cannot write bezel file", err, errlen);
        return 0;
    }
    return 1;
}

int pikobezel_remove(const char *name)
{
    if (!name || !bezel_name_safe(name))
        return 0;
    int media = bezel_media_of(name);
    if (media < 0)
        return 0;
    return unlink(bezel_file_for(media, name).c_str()) == 0 ? 1 : 0;
}

int pikobezel_set_rect(const char *name, unsigned int x, unsigned int y,
                       unsigned int w, unsigned int h)
{
    if (!name || !bezel_name_safe(name))
        return 0;
    int media = bezel_media_of(name);
    if (media < 0)
        return 0;
    return bezel_patch_rect(bezel_file_for(media, name), x, y, w, h) ? 1 : 0;
}

}
