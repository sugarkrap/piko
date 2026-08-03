/*
 * yaml_lite.h -- a parser for exactly the YAML shape manifest.yaml needs,
 * not a general YAML parser. Full YAML (anchors, flow style, multi-document,
 * block scalars, tag handling, ...) is a large spec for a problem that does
 * not exist here: manifest.yaml is always "section name, colon, an indented
 * block-style list of flat key: value entries" -- see manifest.yaml itself.
 * That shape happens to also be valid standard YAML, so any real YAML tool
 * can still read or write it; this just doesn't pretend to handle more than
 * it needs to. Same reasoning as romstate.h's manifest parser or
 * chunked-deploy.sh's own text parsing -- hand-roll the small parser the
 * problem actually calls for.
 *
 * Spaces only for indentation (tabs are a parse error, matching real YAML).
 * `#` starts a comment to end of line unless inside a quoted string.
 * Scalar values may be bare, single-quoted or double-quoted -- quotes are
 * stripped, no escape sequences are interpreted (nothing this file's values
 * hold -- paths, octal modes, enum names -- ever needs one).
 */

#ifndef PIKO_SYNC_DEPLOY_YAML_LITE_H
#define PIKO_SYNC_DEPLOY_YAML_LITE_H

#include <stdio.h>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace piko_sync {
namespace yaml {

struct Entry {
    std::vector<std::pair<std::string, std::string> > fields;

    bool has(const std::string &key) const
    {
        for (size_t i = 0; i < fields.size(); i++)
            if (fields[i].first == key)
                return true;
        return false;
    }

    std::string get(const std::string &key, const std::string &def = std::string()) const
    {
        for (size_t i = 0; i < fields.size(); i++)
            if (fields[i].first == key)
                return fields[i].second;
        return def;
    }
};

struct Section {
    std::string name;
    std::vector<Entry> entries;
};

namespace detail {

inline std::string rtrim(std::string s)
{
    while (!s.empty() && (s[s.size() - 1] == ' ' || s[s.size() - 1] == '\t'
                           || s[s.size() - 1] == '\r'))
        s.erase(s.size() - 1);
    return s;
}

inline std::string trim(std::string s)
{
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos)
        return std::string();
    s = s.substr(a);
    return rtrim(s);
}

/* Strips a trailing `# ...` comment, but not one that appears inside a
 * quoted string -- manifest paths never contain '#', but being careless
 * here would be a landmine for the day one does. */
inline std::string strip_comment(const std::string &line)
{
    bool in_squote = false, in_dquote = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '\'' && !in_dquote) { in_squote = !in_squote; continue; }
        if (c == '"' && !in_squote) { in_dquote = !in_dquote; continue; }
        if (c == '#' && !in_squote && !in_dquote)
            return line.substr(0, i);
    }
    return line;
}

inline std::string unquote(const std::string &s)
{
    if (s.size() >= 2) {
        char first = s[0], last = s[s.size() - 1];
        if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
            return s.substr(1, s.size() - 2);
    }
    return s;
}

inline std::string line_error(int lineno, const std::string &what)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", lineno);
    return "manifest.yaml line " + std::string(buf) + ": " + what;
}

inline bool parse_kv(const std::string &s, Entry &e)
{
    size_t colon = s.find(':');
    if (colon == std::string::npos)
        return false;
    std::string key = trim(s.substr(0, colon));
    std::string val = trim(s.substr(colon + 1));
    if (key.empty())
        return false;
    e.fields.push_back(std::make_pair(key, unquote(val)));
    return true;
}

} /* namespace detail */

/* Parses the whole document into an ordered list of sections. Returns
 * false and fills `error` (with a line number) on anything that doesn't
 * match the expected shape -- a malformed manifest must fail loudly at
 * load time, not silently produce a truncated deploy plan. */
inline bool parse(const std::string &text, std::vector<Section> &out, std::string &error)
{
    out.clear();
    std::istringstream in(text);
    std::string raw;
    Section *cur_section = 0;
    Entry *cur_entry = 0;
    int entry_indent = -1;
    int lineno = 0;

    while (std::getline(in, raw)) {
        lineno++;
        if (raw.find('\t') != std::string::npos) {
            error = detail::line_error(lineno, "tabs are not allowed for indentation");
            return false;
        }

        std::string line = detail::rtrim(detail::strip_comment(raw));
        if (line.find_first_not_of(' ') == std::string::npos)
            continue; /* blank, or was only a comment */

        size_t indent = line.find_first_not_of(' ');
        std::string content = line.substr(indent);

        if (indent == 0) {
            if (content.empty() || content[content.size() - 1] != ':') {
                error = detail::line_error(lineno, "expected a top-level \"section:\" line");
                return false;
            }
            out.push_back(Section());
            cur_section = &out.back();
            cur_section->name = content.substr(0, content.size() - 1);
            cur_entry = 0;
            entry_indent = -1;
            continue;
        }

        if (!cur_section) {
            error = detail::line_error(lineno, "indented content before any \"section:\" line");
            return false;
        }

        if (content.size() >= 2 && content[0] == '-' && content[1] == ' ') {
            cur_section->entries.push_back(Entry());
            cur_entry = &cur_section->entries.back();
            entry_indent = static_cast<int>(indent);
            if (!detail::parse_kv(content.substr(2), *cur_entry)) {
                error = detail::line_error(lineno, "expected \"- key: value\"");
                return false;
            }
            continue;
        }

        if (!cur_entry || static_cast<int>(indent) <= entry_indent) {
            error = detail::line_error(lineno, "unexpected indentation");
            return false;
        }
        if (!detail::parse_kv(content, *cur_entry)) {
            error = detail::line_error(lineno, "expected \"key: value\"");
            return false;
        }
    }

    return true;
}

} /* namespace yaml */
} /* namespace piko_sync */

#endif /* PIKO_SYNC_DEPLOY_YAML_LITE_H */
