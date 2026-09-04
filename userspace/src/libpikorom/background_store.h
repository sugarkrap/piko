#ifndef PIKO_BACKGROUND_STORE_H
#define PIKO_BACKGROUND_STORE_H

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <string>
#include <vector>

#include "emulation_db.h"

#include "piko_asset_format.h"

namespace piko_sync {


struct PkbgHeader {
    unsigned int version;
    unsigned int width, height;
    std::string  source;

    PkbgHeader() : version(PKBG_VERSION), width(0), height(0) {}
};

inline unsigned int pkbg_get_u32(const unsigned char *p)
{
    return piko_asset_u32(p);
}

inline bool pkbg_decode_header(const std::string &blob, PkbgHeader &h,
                               size_t &pixel_offset)
{
    struct pkbg_head raw;
    if (!pkbg_parse_head((const unsigned char *)blob.data(), blob.size(), &raw))
        return false;
    if (blob.size() < raw.pixel_offset)
        return false;
    h.version = raw.version;
    h.width   = raw.width;
    h.height  = raw.height;
    h.source.assign(blob, PKBG_HDR_FIXED, raw.source_len);
    pixel_offset = raw.pixel_offset;
    return blob.size() - pixel_offset >= pkbg_colour_bytes(&raw);
}

inline std::string background_dir_for(int media)
{
    return std::string(media_zaurus_root(media)) + "/backgrounds";
}

inline bool background_name_safe(const std::string &name)
{
    if (name.empty() || name.size() > 128)
        return false;
    if (name.find('/') != std::string::npos)
        return false;
    if (name == "." || name == "..")
        return false;
    return name.find("..") == std::string::npos;
}

inline std::string background_file_for(int media, const std::string &name)
{
    return background_dir_for(media) + "/" + name + ".pkbg";
}

inline bool background_read_header_only(const std::string &path, PkbgHeader &h)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    unsigned char buf[512];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    struct pkbg_head raw;
    if (!pkbg_parse_head(buf, n, &raw))
        return false;
    h.version = raw.version;
    h.width   = raw.width;
    h.height  = raw.height;
    h.source.clear();
    return true;
}

struct StoredBackground {
    std::string name;
    int media;
    PkbgHeader head;
};

inline void background_scan_media(int media, std::vector<StoredBackground> &out)
{
    std::string dir = background_dir_for(media);
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent *e;
    const char *suffix = ".pkbg";
    size_t slen = strlen(suffix);
    while ((e = readdir(d)) != NULL) {
        std::string fn = e->d_name;
        if (fn.size() <= slen || fn.compare(fn.size() - slen, slen, suffix) != 0)
            continue;
        StoredBackground b;
        b.name = fn.substr(0, fn.size() - slen);
        b.media = media;
        if (background_read_header_only(dir + "/" + fn, b.head))
            out.push_back(b);
    }
    closedir(d);
}

inline std::vector<StoredBackground> background_list_all()
{
    std::vector<StoredBackground> out;
    static const int media[] = { PART_SD, PART_CF };
    for (int i = 0; i < 2; i++) {
        if (!media_present(media[i]))
            continue;
        background_scan_media(media[i], out);
    }
    return out;
}

inline std::string background_for_bezel(const std::string &bezel)
{
    static const int media[] = { PART_SD, PART_CF };
    if (bezel.empty())
        return std::string();
    for (int i = 0; i < 2; i++) {
        if (!media_present(media[i]))
            continue;
        std::string path = std::string(media_zaurus_root(media[i]))
            + "/bezels/defaults.cfg";
        FILE *f = fopen(path.c_str(), "r");
        if (!f)
            continue;
        char line[256];
        while (fgets(line, sizeof(line), f) != NULL) {
            char *bar = strchr(line, '|');
            if (bar == NULL)
                continue;
            *bar = '\0';
            if (bezel != line)
                continue;
            char *value = bar + 1;
            char *nl = strpbrk(value, "\r\n");
            if (nl != NULL)
                *nl = '\0';
            fclose(f);
            return std::string(value);
        }
        fclose(f);
    }
    return std::string();
}

inline int background_media_of(const std::string &name)
{
    static const int media[] = { PART_SD, PART_CF };
    struct stat st;
    for (int i = 0; i < 2; i++) {
        if (!media_present(media[i]))
            continue;
        if (stat(background_file_for(media[i], name).c_str(), &st) == 0)
            return media[i];
    }
    return -1;
}

}

#endif
