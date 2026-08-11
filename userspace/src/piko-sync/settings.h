#ifndef PIKO_SYNC_SETTINGS_H
#define PIKO_SYNC_SETTINGS_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace piko_sync {

class Settings {
public:
    static std::string dir_path()
    {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        if (xdg && xdg[0] == '/')
            return std::string(xdg) + "/piko-sync";
        const char *home = getenv("HOME");
        if (home && home[0])
            return std::string(home) + "/.config/piko-sync";
        return std::string();
    }

    static std::string file_path()
    {
        std::string d = dir_path();
        return d.empty() ? d : d + "/settings.cfg";
    }

    bool load() { return load_from(file_path()); }

    bool load_from(const std::string &path)
    {
        entries_.clear();
        if (path.empty())
            return false;

        FILE *f = fopen(path.c_str(), "r");
        if (!f)
            return false;

        char buf[2048];
        while (fgets(buf, sizeof(buf), f)) {
            std::string line(buf);
            std::string::size_type nl = line.find_first_of("\r\n");
            if (nl != std::string::npos)
                line.erase(nl);

            std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
                continue;

            std::string::size_type eq = trimmed.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = trim(trimmed.substr(0, eq));
            if (key.empty())
                continue;
            set(key, trim(trimmed.substr(eq + 1)));
        }
        fclose(f);
        return true;
    }

    bool save() const { return save_to(file_path()); }

    bool save_to(const std::string &path) const
    {
        if (path.empty())
            return false;

        std::string::size_type slash = path.rfind('/');
        if (slash != std::string::npos && !make_dirs(path.substr(0, slash)))
            return false;

        std::string tmp = path + ".tmp";
        FILE *f = fopen(tmp.c_str(), "w");
        if (!f)
            return false;

        fprintf(f, "# piko-sync-client settings. Written on exit and when the\n"
                   "# Settings dialog is confirmed; hand-editing is fine, but a\n"
                   "# running client will overwrite this file when it quits.\n");
        for (size_t i = 0; i < entries_.size(); i++)
            fprintf(f, "%s = %s\n", entries_[i].key.c_str(), entries_[i].value.c_str());

        bool ok = (fflush(f) == 0);
        if (fclose(f) != 0)
            ok = false;
        if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
            unlink(tmp.c_str());
            return false;
        }
        return true;
    }

    bool has(const std::string &key) const { return find(key) >= 0; }

    std::string get(const std::string &key, const std::string &fallback = std::string()) const
    {
        int i = find(key);
        return (i < 0) ? fallback : entries_[i].value;
    }

    int get_int(const std::string &key, int fallback) const
    {
        int i = find(key);
        if (i < 0 || entries_[i].value.empty())
            return fallback;
        char *end = 0;
        long n = strtol(entries_[i].value.c_str(), &end, 10);
        if (!end || *end != '\0')
            return fallback;
        return static_cast<int>(n);
    }

    bool get_bool(const std::string &key, bool fallback) const
    {
        return get_int(key, fallback ? 1 : 0) != 0;
    }

    void set(const std::string &key, const std::string &value)
    {
        int i = find(key);
        if (i >= 0) {
            entries_[i].value = value;
            return;
        }
        Entry e;
        e.key = key;
        e.value = value;
        entries_.push_back(e);
    }

    void set_int(const std::string &key, int value)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", value);
        set(key, buf);
    }

    void set_bool(const std::string &key, bool value) { set_int(key, value ? 1 : 0); }

private:
    struct Entry {
        std::string key;
        std::string value;
    };

    int find(const std::string &key) const
    {
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].key == key)
                return static_cast<int>(i);
        }
        return -1;
    }

    static std::string trim(const std::string &s)
    {
        std::string::size_type b = s.find_first_not_of(" \t");
        if (b == std::string::npos)
            return std::string();
        std::string::size_type e = s.find_last_not_of(" \t");
        return s.substr(b, e - b + 1);
    }

    static bool make_dirs(const std::string &dir)
    {
        if (dir.empty())
            return true;

        for (std::string::size_type i = 1; i <= dir.size(); i++) {
            if (i < dir.size() && dir[i] != '/')
                continue;
            std::string part = dir.substr(0, i);
            if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
        struct stat st;
        return stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    std::vector<Entry> entries_;
};

}

#endif
