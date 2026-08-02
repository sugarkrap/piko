/*
 * protocol.h -- the pikoxfer wire format, shared by pikoxfer-server (runs
 * on the Zaurus) and pikoxfer-client (runs on the host). Depends on
 * nothing but libc/POSIX, so it builds and is tested on the build host
 * with no FLTK, no X and no device -- see tests/protocol-test.cxx, same
 * split as pikostore's romstate.h.
 *
 * DESIGN NOTES THAT ARE NOT OBVIOUS
 *
 * Byte count is the source of truth for resume, not a session cookie.
 * The receiver always knows exactly how many bytes of a file it has
 * durably written; a reconnecting sender just asks (FILE_OFFER) and is
 * told where to seek to (resume_offset in FILE_OFFER_ACK). See
 * chunked-deploy.sh in the piko repo for the same philosophy applied to
 * one-shot scp transfers.
 *
 * Every chunk already crosses the wire inside a TCP stream, which is
 * itself checksummed in flight. The whole-file CRC32 at FILE_COMPLETE
 * only has to catch what TCP can't -- corruption after a byte was
 * already ack'd and written to flash, say -- so a mismatch there means
 * "resend the whole file", not "resend one bad chunk". chunked-deploy.sh
 * accepts the identical tradeoff for the same reason: rare path, cheap
 * fix, not worth the complexity of tracking which chunk was bad.
 *
 * All multi-byte integers are big-endian on the wire (network byte
 * order), so the protocol does not care whether either end is
 * little-endian (the host) or the device's ARM EABI target.
 */

#ifndef PIKOXFER_PROTOCOL_H
#define PIKOXFER_PROTOCOL_H

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

#include <string>

namespace pikoxfer {

const unsigned short DEFAULT_PORT = 7862;
const uint32_t FRAME_MAGIC = 0x504b5846u; /* "PKXF" */
const uint32_t PROTO_VERSION = 1;
const size_t MAX_CHUNK = 65536;           /* payload bytes per DATA_CHUNK */
const size_t MAX_FRAME = MAX_CHUNK + 4096; /* chunk payload + header room */
const size_t MAX_NAME = 255;               /* sanity cap, not a real limit */

enum MessageType {
    MSG_HELLO             = 1, /* client -> server */
    MSG_HELLO_ACK         = 2, /* server -> client */
    MSG_FILE_OFFER        = 3, /* client -> server */
    MSG_FILE_OFFER_ACK    = 4, /* server -> client */
    MSG_DATA_CHUNK        = 5, /* client -> server */
    MSG_CHUNK_ACK         = 6, /* server -> client */
    MSG_FILE_COMPLETE     = 7, /* client -> server */
    MSG_FILE_COMPLETE_ACK = 8, /* server -> client */
    MSG_ERROR             = 9  /* either direction */
};

/* ---------------------------------------------------------------------- *
 * Integer (de)serialization -- big-endian, no struct punning              *
 * ---------------------------------------------------------------------- */

inline void put_u32(std::string &out, uint32_t v)
{
    uint32_t n = htonl(v);
    out.append(reinterpret_cast<const char *>(&n), 4);
}

inline void put_u64(std::string &out, uint64_t v)
{
    put_u32(out, static_cast<uint32_t>(v >> 32));
    put_u32(out, static_cast<uint32_t>(v & 0xffffffffu));
}

inline void put_u16(std::string &out, uint16_t v)
{
    uint16_t n = htons(v);
    out.append(reinterpret_cast<const char *>(&n), 2);
}

inline void put_str16(std::string &out, const std::string &s)
{
    std::string t = s.size() > MAX_NAME ? s.substr(0, MAX_NAME) : s;
    put_u16(out, static_cast<uint16_t>(t.size()));
    out.append(t);
}

/* All get_* functions return false (without advancing pos) on truncated
 * input rather than reading past the end -- a malformed/short frame must
 * be a clean protocol error, never an out-of-bounds read. */

inline bool get_u32(const std::string &in, size_t &pos, uint32_t &v)
{
    if (in.size() - pos < 4)
        return false;
    uint32_t n;
    memcpy(&n, in.data() + pos, 4);
    v = ntohl(n);
    pos += 4;
    return true;
}

inline bool get_u64(const std::string &in, size_t &pos, uint64_t &v)
{
    uint32_t hi, lo;
    if (!get_u32(in, pos, hi) || !get_u32(in, pos, lo))
        return false;
    v = (static_cast<uint64_t>(hi) << 32) | lo;
    return true;
}

inline bool get_u16(const std::string &in, size_t &pos, uint16_t &v)
{
    if (in.size() - pos < 2)
        return false;
    uint16_t n;
    memcpy(&n, in.data() + pos, 2);
    v = ntohs(n);
    pos += 2;
    return true;
}

inline bool get_str16(const std::string &in, size_t &pos, std::string &s)
{
    uint16_t len;
    if (!get_u16(in, pos, len))
        return false;
    if (in.size() - pos < len)
        return false;
    s.assign(in, pos, len);
    pos += len;
    return true;
}

/* ---------------------------------------------------------------------- *
 * CRC32 (IEEE 802.3 polynomial) -- self-contained, no external dep        *
 * ---------------------------------------------------------------------- */

class Crc32 {
public:
    Crc32() : crc_(0xffffffffu) {}

    void update(const char *data, size_t len)
    {
        const uint32_t *table = table_();
        const unsigned char *p = reinterpret_cast<const unsigned char *>(data);
        for (size_t i = 0; i < len; i++)
            crc_ = table[(crc_ ^ p[i]) & 0xff] ^ (crc_ >> 8);
    }

    uint32_t final_value() const { return crc_ ^ 0xffffffffu; }

private:
    static const uint32_t *table_()
    {
        static uint32_t t[256];
        static bool ready = false;
        if (!ready) {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int k = 0; k < 8; k++)
                    c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
                t[i] = c;
            }
            ready = true;
        }
        return t;
    }

    uint32_t crc_;
};

/* ---------------------------------------------------------------------- *
 * Frame = [magic u32][type u32][length u32][payload, length bytes]        *
 * ---------------------------------------------------------------------- */

inline std::string encode_frame(uint32_t type, const std::string &payload)
{
    std::string f;
    put_u32(f, FRAME_MAGIC);
    put_u32(f, type);
    put_u32(f, static_cast<uint32_t>(payload.size()));
    f.append(payload);
    return f;
}

/* Feed arbitrary-sized chunks of a byte stream (as they arrive from a
 * non-blocking socket) and pull out complete frames as they become
 * available. Deliberately has no socket in it -- see FrameReaderTest in
 * tests/protocol-test.cxx, which feeds it split across arbitrary byte
 * boundaries the way a real flaky link would. */
class FrameReader {
public:
    enum Result { NEED_MORE, GOT_FRAME, DESYNC };

    void feed(const char *data, size_t len) { buf_.append(data, len); }

    /* On GOT_FRAME, type/payload are filled and the consumed bytes are
     * dropped from the internal buffer -- call again immediately in a
     * loop, since one feed() can contain more than one frame. */
    Result next(uint32_t &type, std::string &payload)
    {
        if (buf_.size() < 12)
            return NEED_MORE;

        size_t pos = 0;
        uint32_t magic, t, len;
        get_u32(buf_, pos, magic);
        get_u32(buf_, pos, t);
        get_u32(buf_, pos, len);

        if (magic != FRAME_MAGIC || len > MAX_FRAME)
            return DESYNC;
        if (buf_.size() < 12 + len)
            return NEED_MORE;

        type = t;
        payload.assign(buf_, 12, len);
        buf_.erase(0, 12 + len);
        return GOT_FRAME;
    }

private:
    std::string buf_;
};

/* ---------------------------------------------------------------------- *
 * Per-message payloads                                                    *
 * ---------------------------------------------------------------------- */

struct HelloMsg {
    uint32_t version;
    HelloMsg() : version(0) {}
};
inline std::string encode(const HelloMsg &m)
{
    std::string p;
    put_u32(p, m.version);
    return p;
}
inline bool decode_hello(const std::string &p, HelloMsg &m)
{
    size_t pos = 0;
    return get_u32(p, pos, m.version);
}

struct FileOfferMsg {
    std::string name;      /* original filename, no path */
    uint64_t total_size;
    FileOfferMsg() : total_size(0) {}
};
inline std::string encode(const FileOfferMsg &m)
{
    std::string p;
    put_str16(p, m.name);
    put_u64(p, m.total_size);
    return p;
}
inline bool decode_file_offer(const std::string &p, FileOfferMsg &m)
{
    size_t pos = 0;
    return get_str16(p, pos, m.name) && get_u64(p, pos, m.total_size);
}

struct FileOfferAckMsg {
    bool accepted;
    std::string final_name;  /* collision-resolved name; only if accepted */
    uint64_t resume_offset;  /* only if accepted */
    std::string reason;      /* only if !accepted */
    FileOfferAckMsg() : accepted(false), resume_offset(0) {}
};
inline std::string encode(const FileOfferAckMsg &m)
{
    std::string p;
    p.push_back(m.accepted ? 1 : 0);
    if (m.accepted) {
        put_str16(p, m.final_name);
        put_u64(p, m.resume_offset);
    } else {
        put_str16(p, m.reason);
    }
    return p;
}
inline bool decode_file_offer_ack(const std::string &p, FileOfferAckMsg &m)
{
    if (p.empty())
        return false;
    size_t pos = 0;
    m.accepted = p[0] != 0;
    pos = 1;
    if (m.accepted)
        return get_str16(p, pos, m.final_name) && get_u64(p, pos, m.resume_offset);
    return get_str16(p, pos, m.reason);
}

struct DataChunkMsg {
    uint64_t offset;
    std::string data;
    DataChunkMsg() : offset(0) {}
};
inline std::string encode(const DataChunkMsg &m)
{
    std::string p;
    put_u64(p, m.offset);
    put_u32(p, static_cast<uint32_t>(m.data.size()));
    p.append(m.data);
    return p;
}
inline bool decode_data_chunk(const std::string &p, DataChunkMsg &m)
{
    size_t pos = 0;
    uint32_t len;
    if (!get_u64(p, pos, m.offset) || !get_u32(p, pos, len))
        return false;
    if (len > MAX_CHUNK || p.size() - pos < len)
        return false;
    m.data.assign(p, pos, len);
    return true;
}

struct ChunkAckMsg {
    uint64_t bytes_written; /* cumulative, durable bytes on disk */
    ChunkAckMsg() : bytes_written(0) {}
};
inline std::string encode(const ChunkAckMsg &m)
{
    std::string p;
    put_u64(p, m.bytes_written);
    return p;
}
inline bool decode_chunk_ack(const std::string &p, ChunkAckMsg &m)
{
    size_t pos = 0;
    return get_u64(p, pos, m.bytes_written);
}

struct FileCompleteMsg {
    uint32_t crc32; /* over the whole file, 0..total_size */
    FileCompleteMsg() : crc32(0) {}
};
inline std::string encode(const FileCompleteMsg &m)
{
    std::string p;
    put_u32(p, m.crc32);
    return p;
}
inline bool decode_file_complete(const std::string &p, FileCompleteMsg &m)
{
    size_t pos = 0;
    return get_u32(p, pos, m.crc32);
}

struct FileCompleteAckMsg {
    bool ok;
    std::string reason; /* only if !ok */
    FileCompleteAckMsg() : ok(false) {}
};
inline std::string encode(const FileCompleteAckMsg &m)
{
    std::string p;
    p.push_back(m.ok ? 1 : 0);
    if (!m.ok)
        put_str16(p, m.reason);
    return p;
}
inline bool decode_file_complete_ack(const std::string &p, FileCompleteAckMsg &m)
{
    if (p.empty())
        return false;
    m.ok = p[0] != 0;
    if (m.ok)
        return true;
    size_t pos = 1;
    return get_str16(p, pos, m.reason);
}

struct ErrorMsg {
    std::string message;
};
inline std::string encode(const ErrorMsg &m)
{
    std::string p;
    put_str16(p, m.message);
    return p;
}
inline bool decode_error(const std::string &p, ErrorMsg &m)
{
    size_t pos = 0;
    return get_str16(p, pos, m.message);
}

} /* namespace pikoxfer */

#endif /* PIKOXFER_PROTOCOL_H */
