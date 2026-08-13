
#ifndef PIKO_SYNC_PROTOCOL_H
#define PIKO_SYNC_PROTOCOL_H

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

#include <string>

namespace piko_sync {

const unsigned short DEFAULT_PORT = 7862;
const uint32_t FRAME_MAGIC = 0x504b5846u;
const uint32_t PROTO_VERSION = 1;
const size_t MAX_CHUNK = 65536;
const size_t MAX_FRAME = MAX_CHUNK + 4096;
const size_t MAX_NAME = 255;

enum MessageType {
    MSG_HELLO             = 1,
    MSG_HELLO_ACK         = 2,
    MSG_FILE_OFFER        = 3,
    MSG_FILE_OFFER_ACK    = 4,
    MSG_DATA_CHUNK        = 5,
    MSG_CHUNK_ACK         = 6,
    MSG_FILE_COMPLETE     = 7,
    MSG_FILE_COMPLETE_ACK = 8,
    MSG_ERROR             = 9,

    MSG_PUT_OFFER          = 10,
    MSG_PUT_OFFER_ACK      = 11,
    MSG_MKDIR              = 12,
    MSG_MKDIR_ACK          = 13,
    MSG_SYMLINK            = 14,
    MSG_SYMLINK_ACK        = 15,
    MSG_RUN                = 16,
    MSG_RUN_ACK            = 17,
    MSG_QUERY_EXISTING     = 18,
    MSG_QUERY_EXISTING_ACK = 19,
    MSG_FREE_SPACE         = 20,
    MSG_FREE_SPACE_ACK     = 21,

    MSG_DEPLOY_BEGIN       = 22,
    MSG_DEPLOY_BEGIN_ACK   = 23,

    // Screenshot: client asks, server answers with MSG_SCREENSHOT_INFO and
    // then -- if ok -- streams the raw visible framebuffer back using the
    // ordinary MSG_DATA_CHUNK/MSG_FILE_COMPLETE pair, just in the other
    // direction. Pixel conversion and image encoding happen on the host;
    // a 400MHz board should only have to memcpy.
    MSG_SCREENSHOT         = 24,
    MSG_SCREENSHOT_INFO    = 25
};

enum PutPolicy {
    PUT_ALWAYS     = 0,
    PUT_IF_MISSING = 1
};

enum StagingKind {
    STAGE_NAND = 0,
    STAGE_SD   = 1,
    STAGE_CF   = 2
};

enum PutOutcome {
    PUT_RESUME            = 0,
    PUT_ALREADY_SATISFIED = 1,
    PUT_REJECTED          = 2
};

enum RunOp {
    RUN_MOUNT_SD_CARD = 1
};

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

inline std::string encode_frame(uint32_t type, const std::string &payload)
{
    std::string f;
    put_u32(f, FRAME_MAGIC);
    put_u32(f, type);
    put_u32(f, static_cast<uint32_t>(payload.size()));
    f.append(payload);
    return f;
}

class FrameReader {
public:
    enum Result { NEED_MORE, GOT_FRAME, DESYNC };

    void feed(const char *data, size_t len) { buf_.append(data, len); }

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

// `dest_dir` is an OPTIONAL trailing field: empty means "wherever the server
// puts things by default". It is appended rather than inserted so the two
// ends stay compatible in both directions -- an older server stops decoding
// after total_size and simply uses its default directory, and a newer server
// reading an older offer sees the field missing and does the same. Do not
// reorder the fields above it.
struct FileOfferMsg {
    std::string name;
    uint64_t total_size;
    std::string dest_dir;
    FileOfferMsg() : total_size(0) {}
};
inline std::string encode(const FileOfferMsg &m)
{
    std::string p;
    put_str16(p, m.name);
    put_u64(p, m.total_size);
    if (!m.dest_dir.empty())
        put_str16(p, m.dest_dir);
    return p;
}
inline bool decode_file_offer(const std::string &p, FileOfferMsg &m)
{
    size_t pos = 0;
    if (!get_str16(p, pos, m.name) || !get_u64(p, pos, m.total_size))
        return false;
    if (pos < p.size() && !get_str16(p, pos, m.dest_dir))
        return false;
    return true;
}

struct FileOfferAckMsg {
    bool accepted;
    std::string final_name;
    uint64_t resume_offset;
    std::string reason;
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
    uint64_t bytes_written;
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
    uint32_t crc32;
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
    std::string reason;
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

struct PutOfferMsg {
    std::string path;
    uint64_t total_size;
    uint32_t mode;
    uint32_t policy;
    uint32_t crc32;
    bool backup;
    uint32_t staging;
    PutOfferMsg()
        : total_size(0), mode(0), policy(PUT_ALWAYS), crc32(0), backup(false),
          staging(STAGE_NAND) {}
};
inline std::string encode(const PutOfferMsg &m)
{
    std::string p;
    put_str16(p, m.path);
    put_u64(p, m.total_size);
    put_u32(p, m.mode);
    put_u32(p, m.policy);
    put_u32(p, m.crc32);
    p.push_back(m.backup ? 1 : 0);
    put_u32(p, m.staging);
    return p;
}
inline bool decode_put_offer(const std::string &p, PutOfferMsg &m)
{
    size_t pos = 0;
    if (!get_str16(p, pos, m.path) || !get_u64(p, pos, m.total_size)
        || !get_u32(p, pos, m.mode) || !get_u32(p, pos, m.policy)
        || !get_u32(p, pos, m.crc32))
        return false;
    if (p.size() - pos < 1)
        return false;
    m.backup = p[pos] != 0;
    pos += 1;
    return get_u32(p, pos, m.staging);
}

struct PutOfferAckMsg {
    uint32_t outcome;
    uint64_t resume_offset;
    std::string reason;
    PutOfferAckMsg() : outcome(PUT_REJECTED), resume_offset(0) {}
};
inline std::string encode(const PutOfferAckMsg &m)
{
    std::string p;
    put_u32(p, m.outcome);
    put_u64(p, m.resume_offset);
    put_str16(p, m.reason);
    return p;
}
inline bool decode_put_offer_ack(const std::string &p, PutOfferAckMsg &m)
{
    size_t pos = 0;
    return get_u32(p, pos, m.outcome) && get_u64(p, pos, m.resume_offset)
        && get_str16(p, pos, m.reason);
}

struct DeployBeginMsg {
    uint64_t total_bytes;
    DeployBeginMsg() : total_bytes(0) {}
};
inline std::string encode(const DeployBeginMsg &m)
{
    std::string p;
    put_u64(p, m.total_bytes);
    return p;
}
inline bool decode_deploy_begin(const std::string &p, DeployBeginMsg &m)
{
    size_t pos = 0;
    return get_u64(p, pos, m.total_bytes);
}

struct PathMsg {
    std::string path;
};
inline std::string encode(const PathMsg &m)
{
    std::string p;
    put_str16(p, m.path);
    return p;
}
inline bool decode_path(const std::string &p, PathMsg &m)
{
    size_t pos = 0;
    return get_str16(p, pos, m.path);
}

struct OkReasonMsg {
    bool ok;
    std::string reason;
    OkReasonMsg() : ok(false) {}
};
inline std::string encode(const OkReasonMsg &m)
{
    std::string p;
    p.push_back(m.ok ? 1 : 0);
    if (!m.ok)
        put_str16(p, m.reason);
    return p;
}
inline bool decode_ok_reason(const std::string &p, OkReasonMsg &m)
{
    if (p.empty())
        return false;
    m.ok = p[0] != 0;
    if (m.ok)
        return true;
    size_t pos = 1;
    return get_str16(p, pos, m.reason);
}

struct SymlinkMsg {
    std::string target;
    std::string linkname;
};
inline std::string encode(const SymlinkMsg &m)
{
    std::string p;
    put_str16(p, m.target);
    put_str16(p, m.linkname);
    return p;
}
inline bool decode_symlink(const std::string &p, SymlinkMsg &m)
{
    size_t pos = 0;
    return get_str16(p, pos, m.target) && get_str16(p, pos, m.linkname);
}

struct RunMsg {
    uint32_t op;
    RunMsg() : op(0) {}
};
inline std::string encode(const RunMsg &m)
{
    std::string p;
    put_u32(p, m.op);
    return p;
}
inline bool decode_run(const std::string &p, RunMsg &m)
{
    size_t pos = 0;
    return get_u32(p, pos, m.op);
}

struct QueryExistingAckMsg {
    bool exists;
    uint64_t size;
    QueryExistingAckMsg() : exists(false), size(0) {}
};
inline std::string encode(const QueryExistingAckMsg &m)
{
    std::string p;
    p.push_back(m.exists ? 1 : 0);
    put_u64(p, m.size);
    return p;
}
inline bool decode_query_existing_ack(const std::string &p, QueryExistingAckMsg &m)
{
    if (p.empty())
        return false;
    m.exists = p[0] != 0;
    size_t pos = 1;
    return get_u64(p, pos, m.size);
}

struct FreeSpaceAckMsg {
    uint64_t free_bytes;
    FreeSpaceAckMsg() : free_bytes(0) {}
};
inline std::string encode(const FreeSpaceAckMsg &m)
{
    std::string p;
    put_u64(p, m.free_bytes);
    return p;
}
inline bool decode_free_space_ack(const std::string &p, FreeSpaceAckMsg &m)
{
    size_t pos = 0;
    return get_u64(p, pos, m.free_bytes);
}

// Answer to MSG_SCREENSHOT. On ok, `byte_count` bytes of raw pixel data
// follow as MSG_DATA_CHUNK frames (rows already packed to width*bpp/8, i.e.
// the device's line_length padding is stripped), terminated by
// MSG_FILE_COMPLETE carrying the CRC32 of the whole thing. On !ok, `reason`
// says why and nothing follows.
struct ScreenshotInfoMsg {
    bool ok;
    std::string reason;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t byte_count;
    ScreenshotInfoMsg() : ok(false), width(0), height(0), bpp(0), byte_count(0) {}
};
inline std::string encode(const ScreenshotInfoMsg &m)
{
    std::string p;
    p.append(1, m.ok ? 1 : 0);
    put_u32(p, m.width);
    put_u32(p, m.height);
    put_u32(p, m.bpp);
    put_u32(p, m.byte_count);
    put_str16(p, m.reason);
    return p;
}
inline bool decode_screenshot_info(const std::string &p, ScreenshotInfoMsg &m)
{
    if (p.empty())
        return false;
    m.ok = p[0] != 0;
    size_t pos = 1;
    if (!get_u32(p, pos, m.width) || !get_u32(p, pos, m.height) ||
        !get_u32(p, pos, m.bpp) || !get_u32(p, pos, m.byte_count))
        return false;
    return get_str16(p, pos, m.reason);
}

}

#endif
