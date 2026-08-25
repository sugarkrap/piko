#ifndef PIKO_BEZEL_DB_H
#define PIKO_BEZEL_DB_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

struct BezelPreset {
    std::string name;
    std::string preset_path;
    std::string image;
};

inline std::string bezel_dirname(const std::string &path)
{
    std::string::size_type slash = path.find_last_of('/');
    return (slash == std::string::npos) ? std::string(".") : path.substr(0, slash);
}

inline std::string bezel_basename(const std::string &path)
{
    std::string::size_type slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

inline std::string bezel_normalise(const std::string &path)
{
    std::vector<std::string> parts;
    std::string cur;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            if (cur == "..") {
                if (!parts.empty() && parts.back() != "..")
                    parts.pop_back();
                else
                    parts.push_back(cur);
            } else if (!cur.empty() && cur != ".") {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += path[i];
        }
    }
    std::string out;
    if (!path.empty() && path[0] == '/')
        out = "/";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += "/";
        out += parts[i];
    }
    return out;
}

inline std::string bezel_trim(const std::string &s)
{
    std::string::size_type a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    std::string::size_type b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

inline std::string bezel_unquote(const std::string &s)
{
    std::string t = bezel_trim(s);
    if (t.size() >= 2 && t[0] == '"' && t[t.size() - 1] == '"')
        return t.substr(1, t.size() - 2);
    return t;
}

inline std::string bezel_strip_extension(const std::string &name)
{
    std::string::size_type dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

inline bool bezel_parse_preset(const std::string &path, BezelPreset &out)
{
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return false;

    std::map<std::string, std::string> keys;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        std::string line = bezel_trim(buf);
        if (line.empty() || line[0] == '#')
            continue;
        std::string::size_type eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        keys[bezel_trim(line.substr(0, eq))] = bezel_unquote(line.substr(eq + 1));
    }
    fclose(f);

    std::map<std::string, std::string>::const_iterator it = keys.find("overlay0_overlay");
    if (it == keys.end() || it->second.empty())
        return false;

    std::string dir = bezel_dirname(path);
    out.preset_path = path;
    out.name        = bezel_strip_extension(bezel_basename(path));
    out.image       = (it->second[0] == '/') ? it->second
                                             : bezel_normalise(dir + "/" + it->second);
    return true;
}

#endif
