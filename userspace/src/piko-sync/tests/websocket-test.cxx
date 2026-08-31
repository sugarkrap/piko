#include "../websocket.h"
#include "../protocol.h"

#include <stdio.h>
#include <stdlib.h>

#include <string>

using namespace piko_sync;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_str(const std::string &got, const std::string &want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s\n        got  [%s]\n        want [%s]\n",
               what, got.c_str(), want.c_str());
    }
}

static std::string hex(const std::string &s)
{
    static const char *d = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

static std::string feed_all(WsReader &r, const std::string &wire, WsReader::Result &last)
{
    r.feed(wire.data(), wire.size());
    std::string payload;
    last = r.next(payload);
    return payload;
}

int main()
{
    printf("websocket-test\n");

    {
        Sha1 s;
        check_str(hex(s.final_value()), "da39a3ee5e6b4b0d3255bfef95601890afd80709",
                  "sha1 of the empty string");
    }
    {
        Sha1 s;
        s.update("abc", 3);
        check_str(hex(s.final_value()), "a9993e364706816aba3e25717850c26c9cd0d89d",
                  "sha1(abc)");
    }
    {
        Sha1 s;
        s.update("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
        check_str(hex(s.final_value()), "84983e441c3bd26ebaae4aa1f95129e5e54670f1",
                  "sha1 of the 56-byte vector, exactly one pad block");
    }
    {
        Sha1 s;
        std::string million(1000000, 'a');
        s.update(million);
        check_str(hex(s.final_value()), "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
                  "sha1 of a million a's, multi-block");
    }

    check_str(base64_encode(""), "", "base64 of nothing");
    check_str(base64_encode("f"), "Zg==", "base64 one byte");
    check_str(base64_encode("fo"), "Zm8=", "base64 two bytes");
    check_str(base64_encode("foo"), "Zm9v", "base64 three bytes");
    check_str(base64_encode("foobar"), "Zm9vYmFy", "base64 six bytes");
    check_str(base64_encode(std::string("\x00\xff\x10", 3)), "AP8Q", "base64 of non-ascii bytes");

    check_str(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
              "the RFC 6455 handshake vector");

    {
        std::string req =
            "GET /piko-sync HTTP/1.1\r\n"
            "Host: 10.208.47.1:7862\r\n"
            "Upgrade: websocket\r\n"
            "Connection: keep-alive, Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Protocol: piko-sync\r\n"
            "\r\n";
        WsUpgrade up;
        std::string err;
        check(ws_parse_upgrade(req, up, err), "a browser upgrade request parses");
        check_str(up.key, "dGhlIHNhbXBsZSBub25jZQ==", "the key survives parsing");
        check_str(up.path, "/piko-sync", "the path survives parsing");
        check(up.want_subprotocol, "the piko-sync subprotocol is seen in a token list");

        std::string resp = ws_accept_response(up);
        check(resp.find("HTTP/1.1 101 ") == 0, "the response is a 101");
        check(resp.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos,
              "the response carries the computed accept key");
        check(resp.find("Sec-WebSocket-Protocol: piko-sync\r\n") != std::string::npos,
              "the subprotocol is echoed when it was offered");
        check(resp.find("\r\n\r\n") == resp.size() - 4, "the response ends on a blank line");

        std::string err2;
        check(ws_check_response(resp, "dGhlIHNhbXBsZSBub25jZQ==", err2),
              "the client accepts the server's own response");
        check(!ws_check_response(resp, "AAAAAAAAAAAAAAAAAAAAAA==", err2),
              "a response for a different key is rejected");
    }

    {
        WsUpgrade up;
        std::string err;
        std::string base =
            "GET /piko-sync HTTP/1.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        check(ws_parse_upgrade(base, up, err), "a request without a subprotocol still upgrades");
        check(!up.want_subprotocol, "and does not get one echoed back");
        check(ws_accept_response(up).find("Sec-WebSocket-Protocol") == std::string::npos,
              "no subprotocol header when none was offered");

        check(!ws_parse_upgrade("POST /piko-sync HTTP/1.1\r\nUpgrade: websocket\r\n\r\n", up, err),
              "a POST is refused");
        check(!ws_parse_upgrade("GET / HTTP/1.1\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n", up, err),
              "a request without Upgrade: websocket is refused");
        check(!ws_parse_upgrade("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: k\r\nSec-WebSocket-Version: 8\r\n\r\n", up, err),
              "an old draft version is refused");
        check(!ws_parse_upgrade("GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Version: 13\r\n\r\n", up, err),
              "a request without a key is refused");
    }

    {
        std::string wire = ws_encode(WS_BINARY, "hello", false);
        check_str(hex(wire), "820568656c6c6f", "a short unmasked binary frame");

        WsReader r;
        r.expect_masked(false);
        WsReader::Result res;
        std::string got = feed_all(r, wire, res);
        check(res == WsReader::GOT_MESSAGE, "it reads back as a message");
        check_str(got, "hello", "with its payload intact");
    }

    {
        std::string body(200, 'x');
        std::string wire = ws_encode(WS_BINARY, body, false);
        check_str(hex(wire.substr(0, 4)), "827e00c8", "a 200-byte frame uses the 16-bit length");

        WsReader r;
        r.expect_masked(false);
        WsReader::Result res;
        check_str(feed_all(r, wire, res), body, "the 16-bit length round-trips");
        check(res == WsReader::GOT_MESSAGE, "and reads as a message");
    }

    {
        std::string body(70000, 'y');
        std::string wire = ws_encode(WS_BINARY, body, false);
        check_str(hex(wire.substr(0, 10)), "827f0000000000011170",
                  "a 70000-byte frame uses the 64-bit length");

        WsReader r;
        r.expect_masked(false);
        WsReader::Result res;
        check_str(feed_all(r, wire, res), body, "a frame larger than MAX_FRAME round-trips");
    }

    {
        std::string body(MAX_FRAME, 'z');
        check(MAX_FRAME + 12 <= WS_MAX_MESSAGE,
              "the largest piko frame fits inside one websocket message");
        std::string wire = ws_encode(WS_BINARY, encode_frame(MSG_DATA_CHUNK, body), false);
        WsReader r;
        r.expect_masked(false);
        WsReader::Result res;
        std::string msg = feed_all(r, wire, res);
        check(res == WsReader::GOT_MESSAGE, "a maximum-size chunk survives the transport");

        FrameReader fr;
        fr.feed(msg.data(), msg.size());
        uint32_t type = 0;
        std::string payload;
        check(fr.next(type, payload) == FrameReader::GOT_FRAME, "and the PKXF header still parses");
        check(type == MSG_DATA_CHUNK, "with its message type");
        check_str(payload, body, "and its payload");
    }

    {
        std::string wire = ws_encode(WS_BINARY, "masked payload", true);
        check(static_cast<unsigned char>(wire[1]) & 0x80, "a client frame sets the mask bit");
        check(wire.find("masked payload") == std::string::npos, "and the payload is not in the clear");

        WsReader r;
        WsReader::Result res;
        check_str(feed_all(r, wire, res), "masked payload", "a masked frame unmasks");
        check(res == WsReader::GOT_MESSAGE, "and reads as a message");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string payload;
        check(r.next(payload) == WsReader::NEED_MORE, "an empty reader wants more");

        std::string wire = ws_encode(WS_BINARY, "dribble", false);
        for (size_t i = 0; i + 1 < wire.size(); i++) {
            r.feed(wire.data() + i, 1);
            check(r.next(payload) == WsReader::NEED_MORE, "a partial frame never yields");
        }
        r.feed(wire.data() + wire.size() - 1, 1);
        check(r.next(payload) == WsReader::GOT_MESSAGE, "the last byte completes it");
        check_str(payload, "dribble", "byte-at-a-time delivery loses nothing");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string wire = ws_encode(WS_BINARY, "one", false) + ws_encode(WS_BINARY, "two", false);
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::GOT_MESSAGE && payload == "one", "first of two frames");
        check(r.next(payload) == WsReader::GOT_MESSAGE && payload == "two", "second of two frames");
        check(r.next(payload) == WsReader::NEED_MORE, "then the reader drains");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string a = ws_encode(WS_BINARY, "frag", false);
        a[0] = static_cast<char>(0x02);
        std::string b = ws_encode(WS_CONTINUATION, "ment", false);
        std::string wire = a + b;
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::GOT_MESSAGE, "a fragmented message completes");
        check_str(payload, "fragment", "and is reassembled in order");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string a = ws_encode(WS_BINARY, "frag", false);
        a[0] = static_cast<char>(0x02);
        std::string wire = a + ws_encode(WS_PING, "hi", false)
                             + ws_encode(WS_CONTINUATION, "ment", false);
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::GOT_PING && payload == "hi",
              "a ping interleaved in a fragmented message is delivered");
        check(r.next(payload) == WsReader::GOT_MESSAGE && payload == "fragment",
              "and the message still reassembles around it");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string wire = ws_encode(WS_CLOSE, std::string("\x03\xe8", 2), false);
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::GOT_CLOSE, "a close frame is reported");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string wire = ws_encode(WS_BINARY, "nope", true);
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::PROTOCOL_ERROR,
              "a server rejects a frame that should have been unmasked");
    }

    {
        WsReader r;
        std::string wire = ws_encode(WS_BINARY, "nope", false);
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::PROTOCOL_ERROR,
              "a masked-expecting reader rejects an unmasked frame");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string wire = ws_encode(WS_BINARY, "rsv", false);
        wire[0] = static_cast<char>(static_cast<unsigned char>(wire[0]) | 0x40);
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::PROTOCOL_ERROR, "a reserved bit is refused");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string wire = ws_encode(WS_PING, std::string(126, 'p'), false);
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::PROTOCOL_ERROR, "an oversized control frame is refused");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string wire = ws_encode(WS_CONTINUATION, "orphan", false);
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::PROTOCOL_ERROR,
              "a continuation with no message started is refused");
    }

    {
        WsReader r;
        r.expect_masked(false);
        std::string wire;
        wire += static_cast<char>(0x82);
        wire += static_cast<char>(127);
        for (int i = 0; i < 8; i++)
            wire += static_cast<char>(i == 3 ? 0x10 : 0x00);
        r.feed(wire.data(), wire.size());
        std::string payload;
        check(r.next(payload) == WsReader::PROTOCOL_ERROR,
              "a declared length past WS_MAX_MESSAGE is refused before allocating");
    }

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("WEBSOCKET-TEST: FAIL\n");
        return 1;
    }
    printf("WEBSOCKET-TEST: PASS\n");
    return 0;
}
