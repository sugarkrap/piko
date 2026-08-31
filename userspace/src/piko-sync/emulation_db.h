
#ifndef PIKO_SYNC_EMULATION_DB_H
#define PIKO_SYNC_EMULATION_DB_H

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

namespace piko_sync {

const char *const LEGACY_EMULATION_CFG = "/etc/zaurus/emulation.cfg";
const char *const APPLICATIONS_DIR = "/usr/share/applications";
const char *const PIXMAPS_DIR = "/usr/share/pixmaps";
const char *const DEFAULT_ROM_ICON = "/usr/share/pixmaps/rom.png";
const char FIELD_SEP = '|';

inline std::string field_at(const std::string &line, int index)
{
    size_t start = 0;
    for (int i = 0; i < index; i++) {
        size_t sep = line.find(FIELD_SEP, start);
        if (sep == std::string::npos)
            return std::string();
        start = sep + 1;
    }
    size_t stop = line.find(FIELD_SEP, start);
    return line.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
}

enum PartMedia {
    PART_NAND = 0,
    PART_SD   = 1,
    PART_CF   = 2
};

inline const char *part_media_name(int media)
{
    switch (media) {
    case PART_SD: return "SD";
    case PART_CF: return "CF";
    default:      return "NAND";
    }
}

inline int part_media_from_name(const std::string &name)
{
    if (name == "SD") return PART_SD;
    if (name == "CF") return PART_CF;
    return PART_NAND;
}

const int MEDIA_CHOICE_COUNT = 2;

inline int media_from_choice(int choice)
{
    return choice == 1 ? PART_CF : PART_SD;
}

inline int media_to_choice(int media)
{
    return media == PART_CF ? 1 : 0;
}

inline const char *part_media_base(int media)
{
    switch (media) {
    case PART_SD: return "/mnt/card/.zaurus/usr";
    case PART_CF: return "/mnt/cf/.zaurus/usr";
    default:      return "/usr/local";
    }
}

inline const char *media_mount_point(int media)
{
    switch (media) {
    case PART_SD: return "/mnt/card";
    case PART_CF: return "/mnt/cf";
    default:      return "/";
    }
}

inline const char *media_zaurus_root(int media)
{
    switch (media) {
    case PART_SD: return "/mnt/card/.zaurus";
    case PART_CF: return "/mnt/cf/.zaurus";
    default:      return "/usr/local/.zaurus";
    }
}

inline bool media_present(int media)
{
    if (media == PART_NAND)
        return true;

    std::string want = std::string(" ") + media_mount_point(media) + " ";
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, want.c_str()) != 0) { found = true; break; }
    }
    fclose(f);
    return found;
}

inline int media_of_path(const std::string &path)
{
    for (int m = PART_SD; m <= PART_CF; m++) {
        std::string prefix = std::string(media_mount_point(m)) + "/";
        if (path.compare(0, prefix.size(), prefix) == 0)
            return m;
    }
    return PART_NAND;
}

inline bool media_make_zaurus_root(int media)
{
    struct stat st;
    const char *root = media_zaurus_root(media);
    if (stat(root, &st) == 0)
        return S_ISDIR(st.st_mode);
    return mkdir(root, 0755) == 0;
}

inline std::string emulation_cfg_for(int media)
{
    return std::string(media_zaurus_root(media)) + "/emulation.cfg";
}

struct RomEntry {
    std::string path;
    std::string machine;
    std::string backend;
    std::string desktop;
    std::string icon;
    std::string options;
};

inline std::string option_escape(const std::string &value)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < value.size(); i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '%' || c == ',' || c == '|' || c == '=' || c < 0x20) {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        } else {
            out += (char)c;
        }
    }
    return out;
}

inline std::string option_unescape(const std::string &value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); i++) {
        if (value[i] == '%' && i + 2 < value.size()) {
            int hi = -1, lo = -1;
            char a = value[i + 1], b = value[i + 2];
            if (a >= '0' && a <= '9') hi = a - '0';
            else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
            else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
            if (b >= '0' && b <= '9') lo = b - '0';
            else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
            else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
            if (hi >= 0 && lo >= 0) {
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += value[i];
    }
    return out;
}

inline std::string option_get(const std::string &options, const std::string &key)
{
    std::string want = key + "=";
    std::string::size_type pos = 0;
    while (pos <= options.size()) {
        std::string::size_type end = options.find(',', pos);
        if (end == std::string::npos)
            end = options.size();
        std::string item = options.substr(pos, end - pos);
        if (item.size() > want.size() && item.compare(0, want.size(), want) == 0)
            return item.substr(want.size());
        if (end == options.size())
            break;
        pos = end + 1;
    }
    return std::string();
}

inline void option_set(std::string &options, const std::string &key, const std::string &value)
{
    std::string out;
    std::string want = key + "=";
    std::string::size_type pos = 0;
    bool replaced = false;
    while (pos <= options.size() && !options.empty()) {
        std::string::size_type end = options.find(',', pos);
        if (end == std::string::npos)
            end = options.size();
        std::string item = options.substr(pos, end - pos);
        if (!item.empty()) {
            if (item.size() > want.size() && item.compare(0, want.size(), want) == 0) {
                if (!value.empty()) {
                    if (!out.empty()) out += ",";
                    out += want + value;
                }
                replaced = true;
            } else {
                if (!out.empty()) out += ",";
                out += item;
            }
        }
        if (end == options.size())
            break;
        pos = end + 1;
    }
    if (!replaced && !value.empty()) {
        if (!out.empty()) out += ",";
        out += want + value;
    }
    options = out;
}

inline std::string basename_of_path(const std::string &p)
{
    std::string::size_type s = p.find_last_of('/');
    return (s == std::string::npos) ? p : p.substr(s + 1);
}

inline std::string strip_extension(const std::string &name)
{
    std::string::size_type d = name.find_last_of('.');
    if (d == std::string::npos || d == 0)
        return name;
    return name.substr(0, d);
}

inline std::string lowercase(const std::string &s)
{
    std::string out = s;
    for (size_t i = 0; i < out.size(); i++)
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = out[i] - 'A' + 'a';
    return out;
}

inline std::string slugify(const std::string &s)
{
    std::string out;
    bool dash = false;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (alnum) {
            out += (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            dash = false;
        } else if (!dash && !out.empty()) {
            out += '-';
            dash = true;
        }
    }
    while (!out.empty() && out[out.size() - 1] == '-')
        out.erase(out.size() - 1);
    return out.empty() ? std::string("rom") : out;
}

inline std::string desktop_name_for(const std::string &machine, const std::string &rom_path)
{
    return lowercase(machine) + "-" + slugify(strip_extension(basename_of_path(rom_path)))
           + ".desktop";
}

inline std::string encode_entry(const RomEntry &e)
{
    return e.path + FIELD_SEP + e.machine + FIELD_SEP + e.backend + FIELD_SEP
           + e.desktop + FIELD_SEP + e.icon + FIELD_SEP + e.options;
}

inline bool decode_entry(const std::string &line, RomEntry &e)
{
    std::vector<std::string> f;
    std::string cur;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == FIELD_SEP) { f.push_back(cur); cur.clear(); }
        else cur += line[i];
    }
    f.push_back(cur);
    if (f.size() < 5 || f[0].empty())
        return false;
    e.path = f[0];
    e.machine = f[1];
    e.backend = f[2];
    e.desktop = f[3];
    e.icon = f[4];
    e.options = (f.size() >= 6) ? f[5] : std::string();
    return true;
}

inline std::vector<RomEntry> load_emulation_file(const char *path)
{
    std::vector<RomEntry> out;
    FILE *f = fopen(path, "r");
    if (!f)
        return out;
    char buf[2048];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r'))
            line.erase(line.size() - 1);
        if (line.empty())
            continue;
        RomEntry e;
        if (decode_entry(line, e))
            out.push_back(e);
    }
    fclose(f);
    return out;
}

inline bool save_emulation_file(const std::vector<RomEntry> &db, const char *path)
{
    std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    bool ok = true;
    for (size_t i = 0; i < db.size(); i++) {
        std::string line = encode_entry(db[i]) + "\n";
        if (fwrite(line.data(), 1, line.size(), f) != line.size()) { ok = false; break; }
    }
    if (fclose(f) != 0)
        ok = false;
    if (!ok) { remove(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), path) != 0) { remove(tmp.c_str()); return false; }
    return true;
}

inline bool entry_is_directive(const RomEntry &e)
{
    return !e.path.empty() && e.path[0] == '@';
}

inline std::vector<RomEntry> load_emulation_db()
{
    std::vector<RomEntry> out;
    for (int m = PART_NAND; m <= PART_CF; m++) {
        if (!media_present(m))
            continue;
        std::vector<RomEntry> one = load_emulation_file(emulation_cfg_for(m).c_str());
        for (size_t i = 0; i < one.size(); i++) {
            if (entry_is_directive(one[i])) {
                if (m == PART_NAND)
                    out.push_back(one[i]);
            } else if (media_of_path(one[i].path) == m) {
                out.push_back(one[i]);
            }
        }
    }
    return out;
}

inline bool save_emulation_db(const std::vector<RomEntry> &db)
{
    bool ok = true;
    for (int m = PART_NAND; m <= PART_CF; m++) {
        std::vector<RomEntry> mine;
        for (size_t i = 0; i < db.size(); i++) {
            if (!db[i].path.empty() && db[i].path[0] == '@') {
                if (m == PART_NAND)
                    mine.push_back(db[i]);
            } else if (media_of_path(db[i].path) == m) {
                mine.push_back(db[i]);
            }
        }

        std::string path = emulation_cfg_for(m);
        if (!media_present(m)) {
            if (!mine.empty())
                ok = false;
            continue;
        }

        struct stat st;
        if (mine.empty() && stat(path.c_str(), &st) != 0)
            continue;
        if (!media_make_zaurus_root(m)) { ok = false; continue; }
        if (!save_emulation_file(mine, path.c_str()))
            ok = false;
    }
    return ok;
}

inline bool migrate_legacy_emulation_db()
{
    struct stat st;
    if (stat(LEGACY_EMULATION_CFG, &st) != 0)
        return false;

    std::vector<RomEntry> legacy = load_emulation_file(LEGACY_EMULATION_CFG);
    for (size_t i = 0; i < legacy.size(); i++)
        if (!media_present(media_of_path(legacy[i].path)))
            return false;

    std::vector<RomEntry> db = load_emulation_db();
    for (size_t i = 0; i < legacy.size(); i++) {
        bool known = false;
        for (size_t j = 0; j < db.size(); j++)
            if (db[j].path == legacy[i].path) { known = true; break; }
        if (!known)
            db.push_back(legacy[i]);
    }
    if (!save_emulation_db(db))
        return false;

    std::string old = std::string(LEGACY_EMULATION_CFG) + ".old";
    return rename(LEGACY_EMULATION_CFG, old.c_str()) == 0;
}

inline std::string icon_path_for(const std::string &machine, const std::string &rom_path)
{
    return std::string(PIXMAPS_DIR) + "/" + lowercase(machine) + "-"
           + slugify(strip_extension(basename_of_path(rom_path))) + ".png";
}

inline std::string desktop_contents(const RomEntry &e)
{
    std::string name = option_unescape(option_get(e.options, "title"));
    if (name.empty())
        name = strip_extension(basename_of_path(e.path));
    std::string out;
    out += "[Desktop Entry]\n";
    out += "Type=Application\n";
    out += "Name=" + name + "\n";
    out += "Comment=" + e.machine + " game\n";
    std::string runner = (e.machine == "J2ME")
                         ? std::string("/usr/local/bin/phoneme-run")
                         : "/usr/local/bin/" + e.backend + "-run";
    if (option_get(e.options, "heavy") == "1")
        out += "Exec=" + runner + " \"" + e.path + "\"\n";
    else
        out += "Exec=/usr/local/bin/pikoemu \"" + e.path + "\" -- "
               + runner + " \"" + e.path + "\"\n";
    if (option_get(e.options, "heavy") == "1") {
        out += "X-Piko-Heavy=true\n";
        out += "X-Piko-Heavy-Reason=" + e.machine
               + " needs the framebuffer and the input devices to itself.\n";
        out += "X-Piko-Drivers=fb\n";
        out += "X-Piko-Video=qvga\n";
    } else {
        out += "X-Piko-Drivers=x11\n";
    }
    out += "Icon=" + e.icon + "\n";
    out += "Terminal=false\n";
    out += "X-Piko-Rom=" + e.path + "\n";
    out += "X-Piko-Media=" + std::string(part_media_name(media_of_path(e.path))) + "\n";
    out += "Categories=Game;Emulation;" + e.machine + ";\n";
    return out;
}

inline std::string desktop_rom_path(const std::string &contents)
{
    const std::string key = "X-Piko-Rom=";
    std::string::size_type pos = 0;
    while (pos < contents.size()) {
        std::string::size_type end = contents.find('\n', pos);
        if (end == std::string::npos)
            end = contents.size();
        if (contents.compare(pos, key.size(), key) == 0)
            return contents.substr(pos + key.size(), end - pos - key.size());
        pos = end + 1;
    }
    return std::string();
}

}

#endif
