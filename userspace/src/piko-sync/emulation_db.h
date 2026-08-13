
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
const char *const DEFAULT_ROM_ICON = "/usr/share/pixmaps/pocketsnes.png";
const char FIELD_SEP = '|';

struct RomEntry {
    std::string path;
    std::string machine;
    std::string backend;
    std::string desktop;
    std::string icon;
};

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
           + e.desktop + FIELD_SEP + e.icon;
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
    std::string name = strip_extension(basename_of_path(e.path));
    std::string out;
    out += "[Desktop Entry]\n";
    out += "Type=Application\n";
    out += "Name=" + name + "\n";
    out += "Comment=" + e.machine + " game\n";
    out += "Exec=/usr/local/bin/pocketsnes-run interp \"" + e.path + "\"\n";
    out += "X-Piko-Heavy=true\n";
    out += "X-Piko-Heavy-Reason=" + e.machine
           + " emulation needs the framebuffer and the input devices to itself.\n";
    out += "Icon=" + e.icon + "\n";
    out += "Terminal=false\n";
    out += "Categories=Game;Emulation;" + e.machine + ";\n";
    return out;
}

}

#endif
