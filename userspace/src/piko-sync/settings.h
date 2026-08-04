#ifndef PIKO_SYNC_SETTINGS_H
#define PIKO_SYNC_SETTINGS_H

/*
 * settings.h -- the small persistent key=value store behind
 * piko-sync-client's settable fields.
 *
 * Everything the user can type or tick in the client (the Zaurus
 * address, the repo/toolchain/jobs paths from Settings..., the
 * build-and-deploy flags) used to live only in the widgets, so every
 * launch started from the defaults again. That is a poor fit for how
 * this app is actually used: the same one board, the same one checkout,
 * the same handful of flags, many times a day.
 *
 * FORMAT. `key = value`, one per line; `#` or `;` in column one is a
 * comment. Whitespace around the key and around the value is trimmed,
 * so a value cannot begin or end with a space -- no quoting, no
 * escapes, no sections. Deliberate: the file is meant to be readable
 * and hand-editable, and none of the values stored here (paths,
 * interface names, host names, numbers, 0/1 flags) can contain a
 * newline or need leading whitespace. If a future setting does, this is
 * the place to add quoting, not the callers.
 *
 * UNKNOWN KEYS SURVIVE A SAVE. Entries are kept in an ordered vector in
 * the order the file listed them, and save() writes back everything it
 * holds -- including keys this build knows nothing about. So running an
 * older client against a config written by a newer one does not quietly
 * discard the newer one's settings.
 *
 * SAVING IS ATOMIC AND NEVER FATAL. save() writes a sibling .tmp and
 * rename()s it over the target, so an interrupted write cannot leave a
 * half-file that fails to parse next launch. Both load() and save()
 * return false rather than complaining anywhere the user can see it: a
 * config that can't be read or written is a reason to fall back to
 * defaults, not a reason to interrupt a build.
 *
 * C++98, no FLTK, no sockets -- so tests/settings-test.cxx can exercise
 * it on the build machine like protocol.h's own tests do.
 */

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
    /* $XDG_CONFIG_HOME/piko-sync (when set to an absolute path, as the
     * spec requires) else $HOME/.config/piko-sync. Empty if neither
     * variable gives us anything usable -- callers treat that as "no
     * persistence available" and carry on with defaults. */
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

    /* Reads file_path() if it exists. Returns false when there is
     * nothing to read (first ever launch, no $HOME) or the file could
     * not be opened -- in both cases this object is simply left empty
     * and every get() falls back to its caller's default. A malformed
     * LINE is skipped rather than aborting the parse, so one bad
     * hand-edit doesn't cost the user every other setting. */
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
                continue; /* not key=value -- ignore, don't give up on the file */

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

        /* fflush before the rename: without it the data can still be
         * sitting in stdio's buffer while the rename has already made
         * the new name visible. */
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

    /* Anything that isn't a plain integer -- a hand-edited "yes", an
     * empty value, trailing junk -- falls back rather than silently
     * becoming 0, which for a checkbox would look like a deliberate
     * "off" the user never chose. */
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

    /* mkdir -p. An existing directory counts as success; an existing
     * non-directory does not, and is left alone rather than replaced. */
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

} /* namespace piko_sync */

#endif /* PIKO_SYNC_SETTINGS_H */
