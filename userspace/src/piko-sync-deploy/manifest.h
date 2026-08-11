
#ifndef PIKO_SYNC_DEPLOY_MANIFEST_H
#define PIKO_SYNC_DEPLOY_MANIFEST_H

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "yaml_lite.h"
#include "../piko-sync/protocol.h"

namespace piko_sync {
namespace deploy {

struct DeployFlags {
    bool kernel_only;
    bool no_userspace;
    bool create_backup_files;
    bool replace_dropbear;
    DeployFlags()
        : kernel_only(false), no_userspace(false),
          create_backup_files(false), replace_dropbear(false) {}
};

struct DeployContext {
    std::string repo;
    std::string kernel_dir;
    std::string kver;
    std::string ssh_stage;
    std::string alsa_stage;
    std::string mplayer_stage;
    std::string sdl_stage;
    std::string tcroot;
    std::string x11_payload;
    DeployFlags flags;

    std::string get(const std::string &name) const
    {
        if (name == "REPO") return repo;
        if (name == "KERNEL_DIR") return kernel_dir;
        if (name == "KVER") return kver;
        if (name == "SSH_STAGE") return ssh_stage;
        if (name == "ALSA_STAGE") return alsa_stage;
        if (name == "MPLAYER_STAGE") return mplayer_stage;
        if (name == "SDL_STAGE") return sdl_stage;
        if (name == "TCROOT") return tcroot;
        if (name == "X11_PAYLOAD") return x11_payload;
        return std::string();
    }
};

enum StepType {
    STEP_PUT_FILE,
    STEP_MKDIR,
    STEP_SYMLINK,
    STEP_RUN,
    STEP_EXTRACT_TAR_TREE
};

struct Step {
    StepType type;
    std::string local_path;
    std::string remote_path;
    std::string local_tar_path;
    std::string tar_remote_base;
    uint32_t mode;
    uint32_t policy;
    bool backup;
    uint32_t run_op;

    Step()
        : type(STEP_PUT_FILE), mode(0644), policy(PUT_ALWAYS),
          backup(false), run_op(0) {}
};

inline bool path_exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

inline bool read_whole_file(const std::string &path, std::string &out)
{
    std::ifstream f(path.c_str());
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline std::string basename_of(const std::string &p)
{
    std::string::size_type s = p.find_last_of('/');
    return (s == std::string::npos) ? p : p.substr(s + 1);
}

inline std::string join_path(const std::string &base, const std::string &leaf)
{
    if (!base.empty() && base[base.size() - 1] == '/')
        return base + leaf;
    return base + "/" + leaf;
}

inline uint32_t parse_mode(const std::string &s)
{
    if (s.empty())
        return 0644;
    unsigned int v = 0;
    if (sscanf(s.c_str(), "%o", &v) != 1)
        return 0644;
    return static_cast<uint32_t>(v);
}

inline std::string substitute(const std::string &s, const DeployContext &ctx)
{
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
            size_t close = s.find('}', i + 2);
            if (close != std::string::npos) {
                std::string name = s.substr(i + 2, close - (i + 2));
                out += ctx.get(name);
                i = close + 1;
                continue;
            }
        }
        out += s[i];
        i++;
    }
    return out;
}

inline bool read_shell_var_list(const std::string &file_path, const std::string &var_name,
                                 std::vector<std::string> &out, std::string &error)
{
    out.clear();
    std::string text;
    if (!read_whole_file(file_path, text)) {
        error = "cannot read " + file_path;
        return false;
    }

    std::string marker = var_name + "=\"";
    size_t start = text.find(marker);
    if (start == std::string::npos) {
        error = var_name + " not found in " + file_path;
        return false;
    }
    start += marker.size();
    size_t end = text.find('"', start);
    if (end == std::string::npos) {
        error = var_name + " in " + file_path + " has no closing quote";
        return false;
    }

    std::istringstream in(text.substr(start, end - start));
    std::string line;
    while (std::getline(in, line)) {
        std::string t = yaml::detail::trim(line);
        if (t.empty() || t[0] == '#')
            continue;
        out.push_back(t);
    }
    return true;
}

inline bool put_files_from_dir(const std::string &local_dir, const std::string &remote_base,
                                uint32_t fixed_mode, std::vector<Step> &out, std::string &error)
{
    DIR *d = opendir(local_dir.c_str());
    if (!d) {
        error = "cannot open directory " + local_dir;
        return false;
    }

    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        std::string name(e->d_name);
        if (name == "." || name == "..")
            continue;
        std::string full = join_path(local_dir, name);

        struct stat st;
        if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        Step s;
        s.type = STEP_PUT_FILE;
        s.local_path = full;
        s.remote_path = join_path(remote_base, name);
        s.mode = fixed_mode ? fixed_mode : (st.st_mode & 0777);
        s.policy = PUT_ALWAYS;
        out.push_back(s);
    }
    closedir(d);
    return true;
}

inline bool put_files_from_tree(const std::string &local_dir, const std::string &remote_base,
                                 std::vector<Step> &out, std::string &error)
{
    DIR *d = opendir(local_dir.c_str());
    if (!d) {
        error = "cannot open directory " + local_dir;
        return false;
    }

    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        std::string name(e->d_name);
        if (name == "." || name == "..")
            continue;
        std::string full = join_path(local_dir, name);
        std::string remote = join_path(remote_base, name);

        struct stat st;
        if (stat(full.c_str(), &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode)) {
            if (!put_files_from_tree(full, remote, out, error))
                return false;
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;

        Step s;
        s.type = STEP_PUT_FILE;
        s.local_path = full;
        s.remote_path = remote;
        s.mode = st.st_mode & 0777;
        s.policy = PUT_ALWAYS;
        out.push_back(s);
    }
    closedir(d);
    return true;
}

inline std::vector<std::string> select_sections(const DeployFlags &flags)
{
    std::vector<std::string> sec;
    sec.push_back("kernel");
    if (flags.kernel_only)
        return sec;

    sec.push_back("sound_modules");
    sec.push_back("wifi_modules");
    sec.push_back("sd_modules");
    sec.push_back("core_scripts");
    sec.push_back("sd_card_overlay");
    sec.push_back("ssh_payload");
    sec.push_back("opkg");
    sec.push_back("panel_actions");
    if (!flags.no_userspace)
        sec.push_back("userspace_media");
    sec.push_back("x11_matchbox");
    return sec;
}

inline bool entry_gated_out(const yaml::Entry &e, const DeployContext &ctx)
{
    if (e.has("if_exists")) {
        std::string p = substitute(e.get("if_exists"), ctx);
        if (!path_exists(p))
            return true;
    }
    if (e.has("if_flag")) {
        std::string f = e.get("if_flag");
        if (f == "replace_dropbear" && !ctx.flags.replace_dropbear)
            return true;
    }
    return false;
}

inline bool build_put_file_step(const yaml::Entry &e, const DeployContext &ctx, Step &s)
{
    s.type = STEP_PUT_FILE;
    s.local_path = substitute(e.get("local"), ctx);
    s.remote_path = substitute(e.get("remote"), ctx);
    s.mode = parse_mode(e.get("mode"));
    s.policy = (e.get("policy") == "if_missing") ? PUT_IF_MISSING : PUT_ALWAYS;
    s.backup = (e.get("backup") == "always") ? true : ctx.flags.create_backup_files;
    return !s.local_path.empty() && !s.remote_path.empty();
}

inline bool build_plan(const std::vector<yaml::Section> &sections,
                        const std::vector<std::string> &which_sections,
                        const DeployContext &ctx,
                        std::vector<Step> &out, std::string &error)
{
    out.clear();

    for (size_t si = 0; si < which_sections.size(); si++) {
        const yaml::Section *sec = 0;
        for (size_t j = 0; j < sections.size(); j++) {
            if (sections[j].name == which_sections[si]) { sec = &sections[j]; break; }
        }
        if (!sec) {
            error = "manifest.yaml has no section \"" + which_sections[si] + "\"";
            return false;
        }

        for (size_t ei = 0; ei < sec->entries.size(); ei++) {
            const yaml::Entry &e = sec->entries[ei];
            if (entry_gated_out(e, ctx))
                continue;

            std::string type = e.get("type");

            if (type == "put_file") {
                Step s;
                if (!build_put_file_step(e, ctx, s)) {
                    error = "section " + sec->name + ": put_file entry missing local/remote";
                    return false;
                }
                out.push_back(s);

            } else if (type == "mkdir") {
                Step s;
                s.type = STEP_MKDIR;
                s.remote_path = substitute(e.get("remote"), ctx);
                out.push_back(s);

            } else if (type == "symlink") {
                Step s;
                s.type = STEP_SYMLINK;
                s.local_path = substitute(e.get("target"), ctx);
                s.remote_path = substitute(e.get("linkname"), ctx);
                out.push_back(s);

            } else if (type == "run") {
                Step s;
                s.type = STEP_RUN;
                std::string op = e.get("op");
                if (op == "mount_sd_card") s.run_op = RUN_MOUNT_SD_CARD;
                else { error = "section " + sec->name + ": unknown run op \"" + op + "\""; return false; }
                out.push_back(s);

            } else if (type == "put_tree_from_list") {
                std::string list_file = substitute(e.get("list_file"), ctx);
                std::string list_var = e.get("list_var");
                std::vector<std::string> items;
                if (!read_shell_var_list(list_file, list_var, items, error))
                    return false;

                std::string local_base = substitute(e.get("local_base"), ctx);
                std::string remote_base = substitute(e.get("remote_base"), ctx);
                std::string strip_prefix = e.get("strip_prefix");
                bool flatten = e.get("flatten") == "true";
                uint32_t mode = parse_mode(e.get("mode"));
                std::string format = e.get("format", "path");

                for (size_t k = 0; k < items.size(); k++) {
                    Step s;
                    s.type = STEP_PUT_FILE;
                    s.mode = mode;
                    s.policy = PUT_ALWAYS;
                    s.backup = ctx.flags.create_backup_files;

                    if (format == "local:remote:mode") {
                        std::string::size_type c1 = items[k].find(':');
                        std::string::size_type c2 = (c1 == std::string::npos)
                            ? std::string::npos : items[k].find(':', c1 + 1);
                        if (c1 == std::string::npos || c2 == std::string::npos) {
                            error = "section " + sec->name + ": malformed " + list_var + " entry \""
                                    + items[k] + "\"";
                            return false;
                        }
                        std::string lp = items[k].substr(0, c1);
                        std::string rp = items[k].substr(c1 + 1, c2 - c1 - 1);
                        s.local_path = join_path(local_base, lp);
                        s.remote_path = join_path(remote_base, rp);
                        s.mode = parse_mode(items[k].substr(c2 + 1));
                    } else {
                        std::string item = items[k];
                        std::string local_rel = item;
                        if (!strip_prefix.empty() && item.compare(0, strip_prefix.size(), strip_prefix) == 0)
                            local_rel = item.substr(strip_prefix.size());
                        s.local_path = join_path(local_base, local_rel);
                        s.remote_path = flatten ? join_path(remote_base, basename_of(item))
                                                 : join_path(remote_base, item);
                    }
                    out.push_back(s);
                }

            } else if (type == "put_tree_from_dir") {
                std::string local_dir = substitute(e.get("local_dir"), ctx);
                std::string remote_base = substitute(e.get("remote_base"), ctx);
                uint32_t mode = parse_mode(e.get("mode"));
                if (!put_files_from_dir(local_dir, remote_base, mode, out, error))
                    return false;

            } else if (type == "put_tar_tree") {
                Step s;
                s.type = STEP_EXTRACT_TAR_TREE;
                s.local_tar_path = substitute(e.get("local_tar"), ctx);
                s.tar_remote_base = substitute(e.get("remote"), ctx);
                out.push_back(s);

            } else {
                error = "section " + sec->name + ": unknown entry type \"" + type + "\"";
                return false;
            }
        }
    }

    return true;
}

}
}

#endif
