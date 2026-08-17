
#ifndef PIKO_SYNC_EMULATION_DB_H
#define PIKO_SYNC_EMULATION_DB_H

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace piko_sync {

const char *const EMULATION_DIR = "/etc/zaurus";
const char *const EMULATION_CFG = "/etc/zaurus/emulation.cfg";
const char *const APPLICATIONS_DIR = "/usr/share/applications";
const char *const PIXMAPS_DIR = "/usr/share/pixmaps";
const char *const DEFAULT_ROM_ICON = "/usr/share/pixmaps/rom.png";
const char FIELD_SEP = '|';

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

inline std::vector<RomEntry> load_emulation_db(const char *path = EMULATION_CFG)
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

inline bool save_emulation_db(const std::vector<RomEntry> &db, const char *path = EMULATION_CFG)
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
    if (e.machine == "J2ME")
        out += "Exec=/usr/local/bin/phoneme-run \"" + e.path + "\"\n";
    else
        out += "Exec=/usr/local/bin/" + e.backend + "-run \"" + e.path + "\"\n";
    if (option_get(e.options, "heavy") == "1") {
        out += "X-Piko-Heavy=true\n";
        out += "X-Piko-Heavy-Reason=" + e.machine
               + " needs the framebuffer and the input devices to itself.\n";
        out += "X-Piko-Drivers=fb\n";
        out += "X-Piko-Video=qvga\n";
    } else {
        out += "X-Piko-Drivers=x11\n";
        if (e.machine == "J2ME") {
            out += "X-Piko-Video=qvga\n";
            out += "X-Piko-Parts=freepats\n";
        }
    }
    out += "Icon=" + e.icon + "\n";
    out += "Terminal=false\n";
    out += "Categories=Game;Emulation;" + e.machine + ";\n";
    return out;
}

}

#endif
