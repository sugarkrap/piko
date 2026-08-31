#ifndef PIKO_SYNC_BASE64_H
#define PIKO_SYNC_BASE64_H

#include <stddef.h>

#include <string>

namespace piko_sync {

inline std::string base64_encode(const std::string &in)
{
    static const char *tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned v = (static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16)
                   | (static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8)
                   | static_cast<unsigned>(static_cast<unsigned char>(in[i + 2]));
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }

    size_t rest = in.size() - i;
    if (rest == 1) {
        unsigned v = static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += '=';
        out += '=';
    } else if (rest == 2) {
        unsigned v = (static_cast<unsigned>(static_cast<unsigned char>(in[i])) << 16)
                   | (static_cast<unsigned>(static_cast<unsigned char>(in[i + 1])) << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

}

#endif
