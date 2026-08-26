#ifndef PIKO_BEZEL_STORE_H
#define PIKO_BEZEL_STORE_H

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "emulation_db.h"
#include "bezel_format.h"

namespace piko_sync {

inline std::string bezel_dir_for(int media)
{
    return std::string(media_zaurus_root(media)) + "/bezels";
}

inline bool bezel_name_safe(const std::string &name)
{
    if (name.empty() || name.size() > 128)
        return false;
    if (name.find('/') != std::string::npos)
        return false;
    if (name == "." || name == "..")
        return false;
    if (name.find("..") != std::string::npos)
        return false;
    return true;
}

inline std::string bezel_file_for(int media, const std::string &name)
{
    return bezel_dir_for(media) + "/" + name + ".pkbz";
}

inline bool bezel_make_dir(int media)
{
    if (!media_make_zaurus_root(media))
        return false;
    std::string dir = bezel_dir_for(media);
    struct stat st;
    if (stat(dir.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    return mkdir(dir.c_str(), 0755) == 0;
}

inline bool bezel_read_file(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[8192];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

inline bool bezel_write_file(const std::string &path, const std::string &data)
{
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok) { unlink(tmp.c_str()); return false; }
    if (rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}

inline bool bezel_read_header_only(const std::string &path, PkbzHeader &h)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    std::string head(buf, n);
    const unsigned char *p = (const unsigned char *)head.data();
    if (n < PKBZ_HDR_FIXED)
        return false;
    if (p[0] != PKBZ_MAGIC0 || p[1] != PKBZ_MAGIC1
        || p[2] != PKBZ_MAGIC2 || p[3] != PKBZ_MAGIC3)
        return false;
    h.version  = pkbz_get_u32(p + 4);
    h.width    = pkbz_get_u32(p + 8);
    h.height   = pkbz_get_u32(p + 12);
    h.screen_x = pkbz_get_u32(p + 16);
    h.screen_y = pkbz_get_u32(p + 20);
    h.screen_w = pkbz_get_u32(p + 24);
    h.screen_h = pkbz_get_u32(p + 28);
    h.source.clear();
    return h.version == PKBZ_VERSION && h.width > 0 && h.height > 0;
}

inline bool bezel_patch_rect(const std::string &path, unsigned x, unsigned y,
                             unsigned w, unsigned h)
{
    FILE *f = fopen(path.c_str(), "r+b");
    if (!f)
        return false;
    unsigned char magic[4];
    if (fread(magic, 1, 4, f) != 4
        || magic[0] != PKBZ_MAGIC0 || magic[1] != PKBZ_MAGIC1
        || magic[2] != PKBZ_MAGIC2 || magic[3] != PKBZ_MAGIC3) {
        fclose(f);
        return false;
    }
    unsigned char rect[16];
    unsigned vals[4] = { x, y, w, h };
    for (int i = 0; i < 4; i++) {
        rect[i * 4 + 0] = (unsigned char)(vals[i] & 0xFF);
        rect[i * 4 + 1] = (unsigned char)((vals[i] >> 8) & 0xFF);
        rect[i * 4 + 2] = (unsigned char)((vals[i] >> 16) & 0xFF);
        rect[i * 4 + 3] = (unsigned char)((vals[i] >> 24) & 0xFF);
    }
    bool ok = fseek(f, 16, SEEK_SET) == 0 && fwrite(rect, 1, 16, f) == 16;
    fclose(f);
    return ok;
}

struct StoredBezel {
    std::string name;
    int media;
    PkbzHeader master;
};

inline void bezel_scan_media(int media, std::vector<StoredBezel> &out)
{
    std::string dir = bezel_dir_for(media);
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent *e;
    const char *suffix = ".pkbz";
    size_t slen = strlen(suffix);
    while ((e = readdir(d)) != NULL) {
        std::string fn = e->d_name;
        if (fn.size() <= slen || fn.compare(fn.size() - slen, slen, suffix) != 0)
            continue;
        StoredBezel b;
        b.name = fn.substr(0, fn.size() - slen);
        b.media = media;
        if (bezel_read_header_only(dir + "/" + fn, b.master))
            out.push_back(b);
    }
    closedir(d);
}

inline std::vector<StoredBezel> bezel_list_all()
{
    std::vector<StoredBezel> out;
    static const int media[] = { PART_SD, PART_CF, PART_NAND };
    for (int i = 0; i < 3; i++) {
        if (media[i] != PART_NAND && !media_present(media[i]))
            continue;
        bezel_scan_media(media[i], out);
    }
    return out;
}

inline int bezel_media_of(const std::string &name)
{
    static const int media[] = { PART_SD, PART_CF, PART_NAND };
    struct stat st;
    for (int i = 0; i < 3; i++) {
        if (media[i] != PART_NAND && !media_present(media[i]))
            continue;
        if (stat(bezel_file_for(media[i], name).c_str(), &st) == 0)
            return media[i];
    }
    return -1;
}

} // namespace piko_sync

#endif
