#ifndef PIKO_SYNC_WEBSOCKET_H
#define PIKO_SYNC_WEBSOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "base64.h"
#include "sha1.h"

namespace piko_sync {

const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const char WS_SUBPROTOCOL[] = "piko-sync";
const char WS_PATH[] = "/piko-sync";
const size_t WS_MAX_MESSAGE = 262144;
const size_t WS_MAX_HANDSHAKE = 8192;

enum WsOpcode {
    WS_CONTINUATION = 0x0,
    WS_TEXT         = 0x1,
    WS_BINARY       = 0x2,
    WS_CLOSE        = 0x8,
    WS_PING         = 0x9,
    WS_PONG         = 0xa
};

inline bool ws_ieq(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y)
            return false;
    }
    return true;
}

inline std::string ws_trim(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

class HttpHead {
public:
    bool parse(const std::string &raw)
    {
        size_t eol = raw.find("\r\n");
        if (eol == std::string::npos)
            return false;
        start_line_ = raw.substr(0, eol);

        size_t pos = eol + 2;
        for (;;) {
            size_t e = raw.find("\r\n", pos);
            if (e == std::string::npos || e == pos)
                break;
            std::string line = raw.substr(pos, e - pos);
            size_t c = line.find(':');
            if (c != std::string::npos)
                fields_.push_back(std::make_pair(ws_trim(line.substr(0, c)),
                                                 ws_trim(line.substr(c + 1))));
            pos = e + 2;
        }
        return true;
    }

    const std::string &start_line() const { return start_line_; }

    std::string get(const std::string &name) const
    {
        for (size_t i = 0; i < fields_.size(); i++)
            if (ws_ieq(fields_[i].first, name))
                return fields_[i].second;
        return std::string();
    }

    bool has_token(const std::string &name, const std::string &token) const
    {
        std::string v = get(name);
        size_t pos = 0;
        for (;;) {
            size_t c = v.find(',', pos);
            size_t n = (c == std::string::npos) ? std::string::npos : c - pos;
            if (ws_ieq(ws_trim(v.substr(pos, n)), token))
                return true;
            if (c == std::string::npos)
                return false;
            pos = c + 1;
        }
    }

private:
    std::string start_line_;
    std::vector<std::pair<std::string, std::string> > fields_;
};

inline std::string ws_accept_key(const std::string &client_key)
{
    Sha1 s;
    s.update(client_key);
    s.update(WS_GUID, strlen(WS_GUID));
    return base64_encode(s.final_value());
}

struct WsUpgrade {
    std::string key;
    std::string path;
    bool want_subprotocol;
    WsUpgrade() : want_subprotocol(false) {}
};

inline bool ws_parse_upgrade(const std::string &raw, WsUpgrade &out, std::string &error)
{
    HttpHead h;
    if (!h.parse(raw)) { error = "malformed HTTP request"; return false; }

    const std::string &sl = h.start_line();
    if (sl.size() < 4 || sl.compare(0, 4, "GET ") != 0) {
        error = "not a GET request";
        return false;
    }
    size_t sp = sl.find(' ', 4);
    out.path = sl.substr(4, sp == std::string::npos ? std::string::npos : sp - 4);

    if (!h.has_token("Upgrade", "websocket")) { error = "missing Upgrade: websocket"; return false; }
    if (!h.has_token("Connection", "Upgrade")) { error = "missing Connection: Upgrade"; return false; }
    if (h.get("Sec-WebSocket-Version") != "13") { error = "need Sec-WebSocket-Version: 13"; return false; }

    out.key = h.get("Sec-WebSocket-Key");
    if (out.key.empty()) { error = "missing Sec-WebSocket-Key"; return false; }

    out.want_subprotocol = h.has_token("Sec-WebSocket-Protocol", WS_SUBPROTOCOL);
    return true;
}

inline std::string ws_accept_response(const WsUpgrade &up)
{
    std::string r = "HTTP/1.1 101 Switching Protocols\r\n";
    r += "Upgrade: websocket\r\n";
    r += "Connection: Upgrade\r\n";
    r += "Sec-WebSocket-Accept: " + ws_accept_key(up.key) + "\r\n";
    if (up.want_subprotocol)
        r += std::string("Sec-WebSocket-Protocol: ") + WS_SUBPROTOCOL + "\r\n";
    r += "\r\n";
    return r;
}

inline std::string ws_reject_response(const std::string &reason)
{
    std::string body = reason + "\n";
    char len[32];
    snprintf(len, sizeof(len), "%lu", static_cast<unsigned long>(body.size()));
    std::string r = "HTTP/1.1 400 Bad Request\r\n";
    r += "Content-Type: text/plain\r\n";
    r += std::string("Content-Length: ") + len + "\r\n";
    r += "Connection: close\r\n";
    r += "\r\n";
    r += body;
    return r;
}

inline std::string ws_random_key()
{
    std::string raw;
    raw.resize(16);
    for (int i = 0; i < 16; i++)
        raw[i] = static_cast<char>(rand() & 0xff);
    return base64_encode(raw);
}

inline std::string ws_request(const std::string &host, int port, const std::string &key)
{
    char hostline[320];
    snprintf(hostline, sizeof(hostline), "%s:%d", host.c_str(), port);

    std::string r = std::string("GET ") + WS_PATH + " HTTP/1.1\r\n";
    r += std::string("Host: ") + hostline + "\r\n";
    r += "Upgrade: websocket\r\n";
    r += "Connection: Upgrade\r\n";
    r += std::string("Sec-WebSocket-Key: ") + key + "\r\n";
    r += "Sec-WebSocket-Version: 13\r\n";
    r += std::string("Sec-WebSocket-Protocol: ") + WS_SUBPROTOCOL + "\r\n";
    r += "\r\n";
    return r;
}

inline bool ws_check_response(const std::string &raw, const std::string &key, std::string &error)
{
    HttpHead h;
    if (!h.parse(raw)) { error = "malformed handshake response"; return false; }
    if (h.start_line().find(" 101") == std::string::npos) {
        error = "upgrade refused: " + h.start_line();
        return false;
    }
    if (h.get("Sec-WebSocket-Accept") != ws_accept_key(key)) {
        error = "bad Sec-WebSocket-Accept";
        return false;
    }
    return true;
}

inline std::string ws_encode(int opcode, const std::string &payload, bool mask)
{
    std::string out;
    out += static_cast<char>(0x80 | (opcode & 0x0f));

    size_t n = payload.size();
    unsigned char mbit = mask ? 0x80 : 0x00;
    if (n < 126) {
        out += static_cast<char>(mbit | static_cast<unsigned char>(n));
    } else if (n <= 0xffff) {
        out += static_cast<char>(mbit | 126);
        out += static_cast<char>((n >> 8) & 0xff);
        out += static_cast<char>(n & 0xff);
    } else {
        out += static_cast<char>(mbit | 127);
        for (int i = 7; i >= 0; i--)
            out += static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xff);
    }

    if (!mask) {
        out += payload;
        return out;
    }

    unsigned char key[4];
    for (int i = 0; i < 4; i++) {
        key[i] = static_cast<unsigned char>(rand() & 0xff);
        out += static_cast<char>(key[i]);
    }

    size_t base = out.size();
    out += payload;
    for (size_t i = 0; i < n; i++)
        out[base + i] = static_cast<char>(static_cast<unsigned char>(out[base + i]) ^ key[i & 3]);
    return out;
}

class WsReader {
public:
    enum Result { NEED_MORE, GOT_MESSAGE, GOT_PING, GOT_PONG, GOT_CLOSE, PROTOCOL_ERROR };

    WsReader() : expect_mask_(true), in_message_(false) {}

    void expect_masked(bool v) { expect_mask_ = v; }

    void feed(const char *data, size_t len) { buf_.append(data, len); }

    Result next(std::string &payload)
    {
        for (;;) {
            if (buf_.size() < 2)
                return NEED_MORE;

            unsigned char b0 = static_cast<unsigned char>(buf_[0]);
            unsigned char b1 = static_cast<unsigned char>(buf_[1]);
            size_t pos = 2;

            if (b0 & 0x70)
                return PROTOCOL_ERROR;

            bool fin = (b0 & 0x80) != 0;
            int opcode = b0 & 0x0f;
            bool masked = (b1 & 0x80) != 0;
            uint64_t len = b1 & 0x7f;

            if (len == 126) {
                if (buf_.size() < pos + 2)
                    return NEED_MORE;
                len = (static_cast<uint64_t>(static_cast<unsigned char>(buf_[pos])) << 8)
                    | static_cast<unsigned char>(buf_[pos + 1]);
                pos += 2;
            } else if (len == 127) {
                if (buf_.size() < pos + 8)
                    return NEED_MORE;
                len = 0;
                for (int i = 0; i < 8; i++)
                    len = (len << 8) | static_cast<unsigned char>(buf_[pos + i]);
                pos += 8;
            }

            if (masked != expect_mask_)
                return PROTOCOL_ERROR;
            if (len > WS_MAX_MESSAGE)
                return PROTOCOL_ERROR;
            if ((opcode & 0x08) && (!fin || len > 125))
                return PROTOCOL_ERROR;

            unsigned char key[4] = { 0, 0, 0, 0 };
            if (masked) {
                if (buf_.size() < pos + 4)
                    return NEED_MORE;
                for (int i = 0; i < 4; i++)
                    key[i] = static_cast<unsigned char>(buf_[pos + i]);
                pos += 4;
            }

            if (buf_.size() < pos + static_cast<size_t>(len))
                return NEED_MORE;

            std::string body(buf_, pos, static_cast<size_t>(len));
            if (masked)
                for (size_t i = 0; i < body.size(); i++)
                    body[i] = static_cast<char>(static_cast<unsigned char>(body[i]) ^ key[i & 3]);
            buf_.erase(0, pos + static_cast<size_t>(len));

            if (opcode == WS_PING)  { payload = body; return GOT_PING; }
            if (opcode == WS_PONG)  { payload = body; return GOT_PONG; }
            if (opcode == WS_CLOSE) { payload = body; return GOT_CLOSE; }

            if (opcode == WS_CONTINUATION) {
                if (!in_message_)
                    return PROTOCOL_ERROR;
                if (partial_.size() + body.size() > WS_MAX_MESSAGE)
                    return PROTOCOL_ERROR;
                partial_ += body;
            } else if (opcode == WS_BINARY || opcode == WS_TEXT) {
                if (in_message_)
                    return PROTOCOL_ERROR;
                partial_ = body;
                in_message_ = true;
            } else {
                return PROTOCOL_ERROR;
            }

            if (fin) {
                payload = partial_;
                partial_.clear();
                in_message_ = false;
                return GOT_MESSAGE;
            }
        }
    }

private:
    std::string buf_;
    std::string partial_;
    bool expect_mask_;
    bool in_message_;
};

}

#endif
