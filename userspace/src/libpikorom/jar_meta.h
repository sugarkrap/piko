#ifndef PIKO_SYNC_JAR_META_H
#define PIKO_SYNC_JAR_META_H

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include <string>
#include <vector>

namespace piko_sync {

struct JarMeta {
    std::string title;
    std::string icon_name;
    std::string icon_png;
};

inline unsigned jar_u16(const std::string &b, size_t off)
{
    if (off + 2 > b.size())
        return 0;
    return (unsigned char)b[off] | ((unsigned char)b[off + 1] << 8);
}

inline unsigned long jar_u32(const std::string &b, size_t off)
{
    if (off + 4 > b.size())
        return 0;
    return (unsigned long)(unsigned char)b[off]
         | ((unsigned long)(unsigned char)b[off + 1] << 8)
         | ((unsigned long)(unsigned char)b[off + 2] << 16)
         | ((unsigned long)(unsigned char)b[off + 3] << 24);
}

inline bool jar_read_file(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

inline bool jar_inflate_raw(const std::string &in, unsigned long want, std::string &out)
{
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        return false;

    out.assign((size_t)want, '\0');
    zs.next_in = (Bytef *)in.data();
    zs.avail_in = (uInt)in.size();
    zs.next_out = (Bytef *)&out[0];
    zs.avail_out = (uInt)want;

    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK)
        return false;
    out.resize((size_t)(want - zs.avail_out));
    return true;
}

inline bool jar_extract(const std::string &zip, unsigned long lho,
                        unsigned method, unsigned long csize,
                        unsigned long usize, std::string &out)
{
    if (lho + 30 > zip.size() || jar_u32(zip, lho) != 0x04034b50UL)
        return false;

    unsigned nlen = jar_u16(zip, lho + 26);
    unsigned elen = jar_u16(zip, lho + 28);
    size_t data = (size_t)lho + 30 + nlen + elen;
    if (data + csize > zip.size())
        return false;

    if (method == 0) {
        out.assign(zip, data, (size_t)csize);
        return true;
    }
    if (method == 8)
        return jar_inflate_raw(zip.substr(data, (size_t)csize), usize, out);
    return false;
}

inline std::string jar_trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline void jar_parse_manifest(const std::string &mf, JarMeta &meta)
{
    std::string logical;
    std::string midlet1;
    std::string suite_name;
    std::string line;

    for (size_t i = 0; i <= mf.size(); i++) {
        char c = (i < mf.size()) ? mf[i] : '\n';
        if (c == '\r')
            continue;
        if (c != '\n') {
            line += c;
            continue;
        }
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            logical += jar_trim(line);
            line.clear();
            continue;
        }
        if (!logical.empty()) {
            size_t colon = logical.find(':');
            if (colon != std::string::npos) {
                std::string key = jar_trim(logical.substr(0, colon));
                std::string val = jar_trim(logical.substr(colon + 1));
                if (key == "MIDlet-Name")
                    suite_name = val;
                else if (key == "MIDlet-1")
                    midlet1 = val;
            }
        }
        logical = line;
        line.clear();
    }

    if (!midlet1.empty()) {
        size_t c1 = midlet1.find(',');
        size_t c2 = (c1 == std::string::npos) ? std::string::npos
                                              : midlet1.find(',', c1 + 1);
        if (c1 != std::string::npos) {
            std::string label = jar_trim(midlet1.substr(0, c1));
            if (!label.empty())
                meta.title = label;
            if (c2 != std::string::npos) {
                std::string icon = jar_trim(midlet1.substr(c1 + 1, c2 - c1 - 1));
                while (!icon.empty() && icon[0] == '/')
                    icon.erase(0, 1);
                meta.icon_name = icon;
            }
        }
    }
    if (meta.title.empty())
        meta.title = suite_name;
}

inline bool jar_read_meta(const std::string &path, JarMeta &meta)
{
    std::string zip;
    if (!jar_read_file(path, zip) || zip.size() < 22)
        return false;

    size_t eocd = std::string::npos;
    size_t start = (zip.size() > 66000) ? zip.size() - 66000 : 0;
    for (size_t i = zip.size() - 22 + 1; i-- > start; ) {
        if (jar_u32(zip, i) == 0x06054b50UL) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos)
        return false;

    unsigned count = jar_u16(zip, eocd + 10);
    unsigned long cdoff = jar_u32(zip, eocd + 16);

    std::string manifest;
    struct Ent { unsigned long lho, csize, usize; unsigned method; };
    std::vector<std::string> names;
    std::vector<Ent> ents;

    size_t p = (size_t)cdoff;
    for (unsigned i = 0; i < count; i++) {
        if (p + 46 > zip.size() || jar_u32(zip, p) != 0x02014b50UL)
            break;
        Ent e;
        e.method = jar_u16(zip, p + 10);
        e.csize = jar_u32(zip, p + 20);
        e.usize = jar_u32(zip, p + 24);
        unsigned nlen = jar_u16(zip, p + 28);
        unsigned elen = jar_u16(zip, p + 30);
        unsigned clen = jar_u16(zip, p + 32);
        e.lho = jar_u32(zip, p + 42);
        std::string name = zip.substr(p + 46, nlen);
        names.push_back(name);
        ents.push_back(e);
        p += 46 + nlen + elen + clen;
    }

    for (size_t i = 0; i < names.size(); i++) {
        std::string upper = names[i];
        for (size_t k = 0; k < upper.size(); k++)
            if (upper[k] >= 'a' && upper[k] <= 'z')
                upper[k] = (char)(upper[k] - 'a' + 'A');
        if (upper == "META-INF/MANIFEST.MF") {
            jar_extract(zip, ents[i].lho, ents[i].method,
                        ents[i].csize, ents[i].usize, manifest);
            break;
        }
    }
    if (manifest.empty())
        return false;

    jar_parse_manifest(manifest, meta);

    if (!meta.icon_name.empty()) {
        for (size_t i = 0; i < names.size(); i++) {
            if (names[i] == meta.icon_name) {
                jar_extract(zip, ents[i].lho, ents[i].method,
                            ents[i].csize, ents[i].usize, meta.icon_png);
                break;
            }
        }
        if (meta.icon_png.size() < 8
            || meta.icon_png.compare(1, 3, "PNG") != 0)
            meta.icon_png.clear();
    }
    return true;
}

}

#endif
