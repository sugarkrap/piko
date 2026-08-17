#ifndef PIKO_SYNC_PARTS_H
#define PIKO_SYNC_PARTS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "emulation_db.h"

namespace piko_sync {

const char *const CONFIG_CFG = "/etc/zaurus/config.cfg";
const char *const PARTS_CFG = "/etc/zaurus/parts.cfg";

enum PartMedia {
    PART_NAND = 0,
    PART_SD   = 1,
    PART_CF   = 2
};

struct PartEntry {
    std::string id;
    int media;
    std::string path;
    std::string version;
    std::string options;

    PartEntry() : media(PART_NAND) {}
};

struct PartSpec {
    std::string id;
    std::string label;
    std::string detail;
    std::string stage_dir;
    std::string subdir;
    std::string marker;
    std::string version;
    int default_media;
    long size_kb;

    PartSpec() : default_media(0), size_kb(0) {}
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

inline const char *part_media_base(int media)
{
    switch (media) {
    case PART_SD: return "/mnt/card/.zaurus/usr";
    case PART_CF: return "/mnt/cf/.zaurus/usr";
    default:      return "/usr/local";
    }
}

inline std::string part_install_path(const PartSpec &spec, int media)
{
    return std::string(part_media_base(media)) + "/" + spec.subdir;
}

inline std::string part_marker_path(const PartSpec &spec, const std::string &install_path)
{
    if (spec.marker.empty())
        return install_path;
    return install_path + "/" + spec.marker;
}

inline std::string encode_part(const PartEntry &e)
{
    return e.id + FIELD_SEP + part_media_name(e.media) + FIELD_SEP + e.path
           + FIELD_SEP + e.version + FIELD_SEP + e.options;
}

inline bool decode_part(const std::string &line, PartEntry &e)
{
    std::vector<std::string> f;
    std::string cur;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == FIELD_SEP) { f.push_back(cur); cur.clear(); }
        else cur += line[i];
    }
    f.push_back(cur);
    if (f.size() < 3 || f[0].empty())
        return false;
    e.id = f[0];
    e.media = part_media_from_name(f[1]);
    e.path = f[2];
    e.version = (f.size() >= 4) ? f[3] : std::string();
    e.options = (f.size() >= 5) ? f[4] : std::string();
    return true;
}

inline std::vector<PartEntry> load_parts(const char *path = CONFIG_CFG)
{
    std::vector<PartEntry> out;
    FILE *f = fopen(path, "r");
    if (!f)
        return out;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r'))
            line.erase(line.size() - 1);
        if (line.empty() || line[0] == '#')
            continue;
        PartEntry e;
        if (decode_part(line, e))
            out.push_back(e);
    }
    fclose(f);
    return out;
}

inline bool save_parts(const std::vector<PartEntry> &db, const char *path = CONFIG_CFG)
{
    std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    bool ok = true;
    for (size_t i = 0; i < db.size(); i++) {
        std::string line = encode_part(db[i]) + "\n";
        if (fwrite(line.data(), 1, line.size(), f) != line.size()) { ok = false; break; }
    }
    if (fclose(f) != 0)
        ok = false;
    if (!ok) { remove(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), path) != 0) { remove(tmp.c_str()); return false; }
    return true;
}

inline const PartEntry *find_part(const std::vector<PartEntry> &db, const std::string &id)
{
    for (size_t i = 0; i < db.size(); i++)
        if (db[i].id == id)
            return &db[i];
    return 0;
}

inline void set_part(std::vector<PartEntry> &db, const PartEntry &e)
{
    for (size_t i = 0; i < db.size(); i++) {
        if (db[i].id == e.id) { db[i] = e; return; }
    }
    db.push_back(e);
}

inline void remove_part(std::vector<PartEntry> &db, const std::string &id)
{
    for (size_t i = 0; i < db.size(); i++) {
        if (db[i].id == id) { db.erase(db.begin() + i); return; }
    }
}

inline std::vector<PartSpec> &part_catalog_storage()
{
    static std::vector<PartSpec> catalog;
    return catalog;
}

inline bool &part_catalog_loaded()
{
    static bool loaded = false;
    return loaded;
}

inline bool decode_part_spec(const std::string &line, PartSpec &spec)
{
    std::vector<std::string> f;
    std::string cur;
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == FIELD_SEP) { f.push_back(cur); cur.clear(); }
        else cur += line[i];
    }
    f.push_back(cur);
    if (f.size() < 9 || f[0].empty())
        return false;
    spec.id = f[0];
    spec.label = f[1];
    spec.detail = f[2];
    spec.stage_dir = f[3];
    spec.subdir = f[4];
    spec.marker = f[5];
    spec.version = f[6];
    spec.default_media = part_media_from_name(f[7]);
    spec.size_kb = atol(f[8].c_str());
    return true;
}

inline size_t load_part_catalog(const std::string &path)
{
    std::vector<PartSpec> &catalog = part_catalog_storage();
    catalog.clear();
    part_catalog_loaded() = true;

    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line[line.size() - 1] == '\n' || line[line.size() - 1] == '\r'))
            line.erase(line.size() - 1);
        if (line.empty() || line[0] == '#')
            continue;
        PartSpec spec;
        if (decode_part_spec(line, spec))
            catalog.push_back(spec);
    }
    fclose(f);
    return catalog.size();
}

inline const std::vector<PartSpec> &part_catalog()
{
    if (!part_catalog_loaded()) {
        const char *env = getenv("PIKO_PARTS_CFG");
        load_part_catalog((env != 0 && *env != '\0') ? env : PARTS_CFG);
    }
    return part_catalog_storage();
}

inline const PartSpec *part_spec(const std::string &id)
{
    const std::vector<PartSpec> &catalog = part_catalog();
    for (size_t i = 0; i < catalog.size(); i++)
        if (catalog[i].id == id)
            return &catalog[i];
    return 0;
}

}

#endif
