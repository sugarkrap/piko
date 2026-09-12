#ifndef PIKO_SYNC_PROTOCOL_H
#define PIKO_SYNC_PROTOCOL_H

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>

#include "crc32.h"

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

    MSG_SCREENSHOT         = 24,
    MSG_SCREENSHOT_INFO    = 25,

    MSG_ROM_LIST           = 26,
    MSG_ROM_LIST_ACK       = 27,
    MSG_ROM_DELETE         = 28,
    MSG_ROM_DELETE_ACK     = 29,
    MSG_ROM_SET_ICON       = 30,
    MSG_ROM_SET_ICON_ACK   = 31,
    MSG_ROM_GET_ICON       = 32,
    MSG_ROM_GET_ICON_ACK   = 33,
    MSG_BEZEL_LIST         = 34,
    MSG_BEZEL_LIST_ACK     = 35,
    MSG_BEZEL_PUT          = 36,
    MSG_BEZEL_PUT_ACK      = 37,
    MSG_BEZEL_DELETE       = 38,
    MSG_BEZEL_DELETE_ACK   = 39,
    MSG_BEZEL_GET          = 40,
    MSG_BEZEL_GET_ACK      = 41,
    MSG_BEZEL_SET_RECT     = 42,
    MSG_BEZEL_SET_RECT_ACK = 43,
    MSG_ROM_SET_OPTION     = 44,
    MSG_ROM_SET_OPTION_ACK = 45,

    MSG_PING               = 46,
    MSG_PONG               = 47,

    MSG_DEVICE_INFO        = 48,
    MSG_DEVICE_INFO_ACK    = 49,
    MSG_DEVICE_STATS       = 50,
    MSG_DEVICE_STATS_ACK   = 51,
    MSG_WALLPAPER          = 52,
    MSG_WALLPAPER_INFO     = 53,

    MSG_ROM_GET_ICONS      = 54,
    MSG_ROM_ICONS_INFO     = 55,

    MSG_BACKEND_LIST       = 56,
    MSG_BACKEND_LIST_ACK   = 57,

    MSG_ROM_SET_BACKEND    = 58,
    MSG_ROM_SET_BACKEND_ACK = 59,

    MSG_ROM_APPLY          = 60,
    MSG_ROM_APPLY_ACK      = 61,

    MSG_BEZEL_GET_MANY     = 62,
    MSG_BEZEL_MANY_INFO    = 63,

    MSG_BG_LIST            = 64,
    MSG_BG_LIST_ACK        = 65,

    MSG_BG_GET             = 66,
    MSG_BG_GET_ACK         = 67,

    MSG_ROM_ENSURE_DELETE       = 68,
    MSG_ROM_ENSURE_DELETE_ACK   = 69,
    MSG_BEZEL_ENSURE_DELETE     = 70,
    MSG_BEZEL_ENSURE_DELETE_ACK = 71,

    MSG_ROM_GET                 = 72,
    MSG_ROM_GET_ACK             = 73
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

struct HelloAckMsg {
    uint32_t version;
    std::string kernel;
    std::string boot_mnt;
    std::string card_mnt;
    std::string card_root;
    HelloAckMsg() : version(0) {}
};
inline std::string encode(const HelloAckMsg &m)
{
    std::string p;
    put_u32(p, m.version);
    if (!m.kernel.empty() || !m.boot_mnt.empty() || !m.card_mnt.empty() || !m.card_root.empty()) {
        put_str16(p, m.kernel);
        put_str16(p, m.boot_mnt);
        put_str16(p, m.card_mnt);
        put_str16(p, m.card_root);
    }
    return p;
}
inline bool decode_hello_ack(const std::string &p, HelloAckMsg &m)
{
    size_t pos = 0;
    if (!get_u32(p, pos, m.version))
        return false;
    if (pos < p.size() && !get_str16(p, pos, m.kernel))
        return false;
    if (pos < p.size() && !get_str16(p, pos, m.boot_mnt))
        return false;
    if (pos < p.size() && !get_str16(p, pos, m.card_mnt))
        return false;
    if (pos < p.size() && !get_str16(p, pos, m.card_root))
        return false;
    return true;
}

struct FileOfferMsg {
    std::string name;
    uint64_t total_size;
    std::string dest_dir;
    std::string rom_machine;
    std::string rom_options;
    FileOfferMsg() : total_size(0) {}
};
inline std::string encode(const FileOfferMsg &m)
{
    std::string p;
    put_str16(p, m.name);
    put_u64(p, m.total_size);
    bool need_machine = !m.rom_machine.empty() || !m.rom_options.empty();
    if (!m.dest_dir.empty() || need_machine)
        put_str16(p, m.dest_dir);
    if (need_machine)
        put_str16(p, m.rom_machine);
    if (!m.rom_options.empty())
        put_str16(p, m.rom_options);
    return p;
}
inline bool decode_file_offer(const std::string &p, FileOfferMsg &m)
{
    size_t pos = 0;
    if (!get_str16(p, pos, m.name) || !get_u64(p, pos, m.total_size))
        return false;
    if (pos < p.size() && !get_str16(p, pos, m.dest_dir))
        return false;
    if (pos < p.size() && !get_str16(p, pos, m.rom_machine))
        return false;
    if (pos < p.size() && !get_str16(p, pos, m.rom_options))
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
    std::string path;
    FileCompleteAckMsg() : ok(false) {}
};
inline std::string encode(const FileCompleteAckMsg &m)
{
    std::string p;
    p.push_back(m.ok ? 1 : 0);
    if (m.ok)
        put_str16(p, m.path);
    else
        put_str16(p, m.reason);
    return p;
}
inline bool decode_file_complete_ack(const std::string &p, FileCompleteAckMsg &m)
{
    if (p.empty())
        return false;
    m.ok = p[0] != 0;
    size_t pos = 1;
    return get_str16(p, pos, m.ok ? m.path : m.reason);
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

struct EnsureDeleteAckMsg {
    std::vector<uint8_t> deleted;
};
inline std::string encode(const EnsureDeleteAckMsg &m)
{
    std::string p;
    put_u16(p, static_cast<uint16_t>(m.deleted.size()));
    for (size_t i = 0; i < m.deleted.size(); i++)
        p += static_cast<char>(m.deleted[i]);
    return p;
}
inline bool decode_ensure_delete_ack(const std::string &p, EnsureDeleteAckMsg &m)
{
    size_t pos = 0;
    uint16_t count;
    if (!get_u16(p, pos, count))
        return false;
    if (p.size() - pos < count)
        return false;
    for (uint16_t i = 0; i < count; i++)
        m.deleted.push_back(static_cast<uint8_t>(p[pos + i]));
    return true;
}

struct BezelListAckMsg {
    std::string records;
};
inline std::string encode(const BezelListAckMsg &m)
{
    std::string p;
    put_u32(p, static_cast<uint32_t>(m.records.size()));
    p.append(m.records);
    return p;
}
inline bool decode_bezel_list_ack(const std::string &p, BezelListAckMsg &m)
{
    size_t off = 0;
    uint32_t n = 0;
    if (!get_u32(p, off, n)) return false;
    if (p.size() - off < n) return false;
    m.records.assign(p, off, n);
    return true;
}

const size_t MAX_BEZEL_BYTES = 8u * 1024u * 1024u;

struct BezelPutMsg {
    BezelPutMsg() : media(0), total(0) {}
    std::string name;
    uint32_t    media;
    uint32_t    total;
};
inline std::string encode(const BezelPutMsg &m)
{
    std::string p;
    put_str16(p, m.name);
    put_u32(p, m.media);
    put_u32(p, m.total);
    return p;
}
inline bool decode_bezel_put(const std::string &p, BezelPutMsg &m)
{
    size_t pos = 0;
    if (!get_str16(p, pos, m.name))
        return false;
    if (!get_u32(p, pos, m.media))
        return false;
    return get_u32(p, pos, m.total);
}

struct BezelBlobMsg {
    BezelBlobMsg() : media(0) {}
    std::string name;
    uint32_t    media;
    std::string data;
};
inline std::string encode(const BezelBlobMsg &m)
{
    std::string p;
    put_str16(p, m.name);
    put_u32(p, m.media);
    put_u32(p, static_cast<uint32_t>(m.data.size()));
    p.append(m.data);
    return p;
}
inline bool decode_bezel_blob(const std::string &p, BezelBlobMsg &m)
{
    size_t off = 0;
    uint32_t n = 0;
    if (!get_str16(p, off, m.name)) return false;
    if (!get_u32(p, off, m.media)) return false;
    if (!get_u32(p, off, n)) return false;
    if (p.size() - off < n) return false;
    m.data.assign(p, off, n);
    return true;
}

struct RomOptionMsg {
    std::string path;
    std::string key;
    std::string value;
};
inline std::string encode(const RomOptionMsg &m)
{
    std::string p;
    put_str16(p, m.path);
    put_str16(p, m.key);
    put_str16(p, m.value);
    return p;
}
inline bool decode_rom_option(const std::string &p, RomOptionMsg &m)
{
    size_t off = 0;
    if (!get_str16(p, off, m.path)) return false;
    if (!get_str16(p, off, m.key)) return false;
    if (!get_str16(p, off, m.value)) return false;
    return true;
}

struct BezelChunkMsg {
    BezelChunkMsg() : total(0), offset(0) {}
    uint32_t total;
    uint32_t offset;
    std::string data;
};
inline std::string encode(const BezelChunkMsg &m)
{
    std::string p;
    put_u32(p, m.total);
    put_u32(p, m.offset);
    put_u32(p, static_cast<uint32_t>(m.data.size()));
    p.append(m.data);
    return p;
}
inline bool decode_bezel_chunk(const std::string &p, BezelChunkMsg &m)
{
    size_t off = 0;
    uint32_t n = 0;
    if (!get_u32(p, off, m.total)) return false;
    if (!get_u32(p, off, m.offset)) return false;
    if (!get_u32(p, off, n)) return false;
    if (p.size() - off < n) return false;
    m.data.assign(p, off, n);
    return true;
}

struct BezelRectMsg {
    std::string name;
    uint32_t x, y, w, h;
};
inline std::string encode(const BezelRectMsg &m)
{
    std::string p;
    put_str16(p, m.name);
    put_u32(p, m.x);
    put_u32(p, m.y);
    put_u32(p, m.w);
    put_u32(p, m.h);
    return p;
}
inline bool decode_bezel_rect(const std::string &p, BezelRectMsg &m)
{
    size_t off = 0;
    if (!get_str16(p, off, m.name)) return false;
    if (!get_u32(p, off, m.x)) return false;
    if (!get_u32(p, off, m.y)) return false;
    if (!get_u32(p, off, m.w)) return false;
    if (!get_u32(p, off, m.h)) return false;
    return true;
}

struct RomIconMsg {
    std::string rom_path;
    std::string icon_name;
    std::string data;
};
inline std::string encode(const RomIconMsg &m)
{
    std::string p;
    put_str16(p, m.rom_path);
    put_str16(p, m.icon_name);
    put_u32(p, static_cast<uint32_t>(m.data.size()));
    p.append(m.data);
    return p;
}
inline bool decode_rom_icon(const std::string &p, RomIconMsg &m)
{
    size_t pos = 0;
    uint32_t len;
    if (!get_str16(p, pos, m.rom_path) || !get_str16(p, pos, m.icon_name))
        return false;
    if (!get_u32(p, pos, len))
        return false;
    if (p.size() - pos < len)
        return false;
    m.data.assign(p, pos, len);
    return true;
}

struct RomListAckMsg {
    std::string records;
};
inline std::string encode(const RomListAckMsg &m)
{
    std::string p;
    put_u32(p, static_cast<uint32_t>(m.records.size()));
    p.append(m.records);
    return p;
}
inline bool decode_rom_list_ack(const std::string &p, RomListAckMsg &m)
{
    size_t pos = 0;
    uint32_t len;
    if (!get_u32(p, pos, len))
        return false;
    if (p.size() - pos < len)
        return false;
    m.records.assign(p, pos, len);
    return true;
}

struct RomApplyMsg {
    std::string path;
    std::string backend;
    std::vector<std::string> keys;
    std::vector<std::string> values;
    std::string icon;
};
inline std::string encode(const RomApplyMsg &m)
{
    std::string p;
    put_str16(p, m.path);
    put_str16(p, m.backend);
    put_u16(p, static_cast<uint16_t>(m.keys.size()));
    for (size_t i = 0; i < m.keys.size(); i++) {
        put_str16(p, m.keys[i]);
        put_str16(p, i < m.values.size() ? m.values[i] : std::string());
    }
    put_u32(p, static_cast<uint32_t>(m.icon.size()));
    p.append(m.icon);
    return p;
}
inline bool decode_rom_apply(const std::string &p, RomApplyMsg &m)
{
    size_t pos = 0;
    uint16_t count;
    uint32_t len;
    if (!get_str16(p, pos, m.path) || !get_str16(p, pos, m.backend))
        return false;
    if (!get_u16(p, pos, count))
        return false;
    for (uint16_t i = 0; i < count; i++) {
        std::string key, value;
        if (!get_str16(p, pos, key) || !get_str16(p, pos, value))
            return false;
        m.keys.push_back(key);
        m.values.push_back(value);
    }
    if (!get_u32(p, pos, len))
        return false;
    if (p.size() - pos < len)
        return false;
    m.icon.assign(p, pos, len);
    return true;
}

struct RomBackendMsg {
    std::string path;
    std::string backend;
};
inline std::string encode(const RomBackendMsg &m)
{
    std::string p;
    put_str16(p, m.path);
    put_str16(p, m.backend);
    return p;
}
inline bool decode_rom_backend(const std::string &p, RomBackendMsg &m)
{
    size_t pos = 0;
    if (!get_str16(p, pos, m.path))
        return false;
    return get_str16(p, pos, m.backend);
}

struct BackendListAckMsg {
    std::string records;
};
inline std::string encode(const BackendListAckMsg &m)
{
    std::string p;
    put_u32(p, static_cast<uint32_t>(m.records.size()));
    p.append(m.records);
    return p;
}
inline bool decode_backend_list_ack(const std::string &p, BackendListAckMsg &m)
{
    size_t pos = 0;
    uint32_t len;
    if (!get_u32(p, pos, len))
        return false;
    if (p.size() - pos < len)
        return false;
    m.records.assign(p, pos, len);
    return true;
}

struct RomIconsMsg {
    std::vector<std::string> paths;
};
inline std::string encode(const RomIconsMsg &m)
{
    std::string p;
    put_u16(p, static_cast<uint16_t>(m.paths.size()));
    for (size_t i = 0; i < m.paths.size(); i++)
        put_str16(p, m.paths[i]);
    return p;
}
inline bool decode_rom_icons(const std::string &p, RomIconsMsg &m)
{
    size_t pos = 0;
    uint16_t count;
    if (!get_u16(p, pos, count))
        return false;
    for (uint16_t i = 0; i < count; i++) {
        std::string path;
        if (!get_str16(p, pos, path))
            return false;
        m.paths.push_back(path);
    }
    return true;
}

struct RomIconsInfoMsg {
    bool ok;
    uint32_t entry_count;
    uint32_t byte_count;
    std::string reason;
    RomIconsInfoMsg() : ok(false), entry_count(0), byte_count(0) {}
};
inline std::string encode(const RomIconsInfoMsg &m)
{
    std::string p;
    p.append(1, m.ok ? 1 : 0);
    put_u32(p, m.entry_count);
    put_u32(p, m.byte_count);
    put_str16(p, m.reason);
    return p;
}
inline bool decode_rom_icons_info(const std::string &p, RomIconsInfoMsg &m)
{
    if (p.empty())
        return false;
    m.ok = p[0] != 0;
    size_t pos = 1;
    if (!get_u32(p, pos, m.entry_count) || !get_u32(p, pos, m.byte_count))
        return false;
    return get_str16(p, pos, m.reason);
}

struct BezelManyMsg {
    std::vector<std::string> names;
};
inline std::string encode(const BezelManyMsg &m)
{
    std::string p;
    put_u16(p, static_cast<uint16_t>(m.names.size()));
    for (size_t i = 0; i < m.names.size(); i++)
        put_str16(p, m.names[i]);
    return p;
}
inline bool decode_bezel_many(const std::string &p, BezelManyMsg &m)
{
    size_t pos = 0;
    uint16_t count;
    if (!get_u16(p, pos, count))
        return false;
    for (uint16_t i = 0; i < count; i++) {
        std::string name;
        if (!get_str16(p, pos, name))
            return false;
        m.names.push_back(name);
    }
    return true;
}

struct BezelManyInfoMsg {
    bool ok;
    uint32_t entry_count;
    uint32_t byte_count;
    std::string reason;
    BezelManyInfoMsg() : ok(false), entry_count(0), byte_count(0) {}
};
inline std::string encode(const BezelManyInfoMsg &m)
{
    std::string p;
    p.append(1, m.ok ? 1 : 0);
    put_u32(p, m.entry_count);
    put_u32(p, m.byte_count);
    put_str16(p, m.reason);
    return p;
}
inline bool decode_bezel_many_info(const std::string &p, BezelManyInfoMsg &m)
{
    if (p.empty())
        return false;
    m.ok = p[0] != 0;
    size_t pos = 1;
    if (!get_u32(p, pos, m.entry_count) || !get_u32(p, pos, m.byte_count))
        return false;
    return get_str16(p, pos, m.reason);
}

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


struct DeviceMount {
    std::string mount_point;
    std::string device;
    uint64_t total_bytes;
    uint64_t free_bytes;
    DeviceMount() : total_bytes(0), free_bytes(0) {}
};

struct DeviceInfoAckMsg {
    std::string hostname;
    std::string model;
    uint64_t memory_total;
    std::vector<DeviceMount> mounts;
    DeviceInfoAckMsg() : memory_total(0) {}
};
inline std::string encode(const DeviceInfoAckMsg &m)
{
    std::string p;
    put_str16(p, m.hostname);
    put_str16(p, m.model);
    put_u64(p, m.memory_total);
    put_u32(p, static_cast<uint32_t>(m.mounts.size()));
    for (size_t i = 0; i < m.mounts.size(); i++) {
        put_str16(p, m.mounts[i].mount_point);
        put_str16(p, m.mounts[i].device);
        put_u64(p, m.mounts[i].total_bytes);
        put_u64(p, m.mounts[i].free_bytes);
    }
    return p;
}
inline bool decode_device_info_ack(const std::string &p, DeviceInfoAckMsg &m)
{
    size_t pos = 0;
    uint32_t count = 0;
    if (!get_str16(p, pos, m.hostname) || !get_str16(p, pos, m.model))
        return false;
    if (!get_u64(p, pos, m.memory_total) || !get_u32(p, pos, count))
        return false;
    m.mounts.clear();
    for (uint32_t i = 0; i < count; i++) {
        DeviceMount mount;
        if (!get_str16(p, pos, mount.mount_point) || !get_str16(p, pos, mount.device))
            return false;
        if (!get_u64(p, pos, mount.total_bytes) || !get_u64(p, pos, mount.free_bytes))
            return false;
        m.mounts.push_back(mount);
    }
    return true;
}

struct DeviceStatsAckMsg {
    uint32_t cpu_percent;
    uint64_t memory_total;
    uint64_t memory_available;
    DeviceStatsAckMsg() : cpu_percent(0), memory_total(0), memory_available(0) {}
};
inline std::string encode(const DeviceStatsAckMsg &m)
{
    std::string p;
    put_u32(p, m.cpu_percent);
    put_u64(p, m.memory_total);
    put_u64(p, m.memory_available);
    return p;
}
inline bool decode_device_stats_ack(const std::string &p, DeviceStatsAckMsg &m)
{
    size_t pos = 0;
    return get_u32(p, pos, m.cpu_percent) && get_u64(p, pos, m.memory_total)
        && get_u64(p, pos, m.memory_available);
}

struct WallpaperMsg {
    uint32_t known_checksum;
    WallpaperMsg() : known_checksum(0) {}
};
inline std::string encode(const WallpaperMsg &m)
{
    std::string p;
    put_u32(p, m.known_checksum);
    return p;
}
inline bool decode_wallpaper(const std::string &p, WallpaperMsg &m)
{
    size_t pos = 0;
    return get_u32(p, pos, m.known_checksum);
}

struct WallpaperInfoMsg {
    bool ok;
    bool unchanged;
    uint32_t checksum;
    uint32_t byte_count;
    std::string path;
    std::string reason;
    WallpaperInfoMsg() : ok(false), unchanged(false), checksum(0), byte_count(0) {}
};
inline std::string encode(const WallpaperInfoMsg &m)
{
    std::string p;
    p.append(1, m.ok ? 1 : 0);
    p.append(1, m.unchanged ? 1 : 0);
    put_u32(p, m.checksum);
    put_u32(p, m.byte_count);
    put_str16(p, m.path);
    put_str16(p, m.reason);
    return p;
}
inline bool decode_wallpaper_info(const std::string &p, WallpaperInfoMsg &m)
{
    if (p.size() < 2)
        return false;
    m.ok = p[0] != 0;
    m.unchanged = p[1] != 0;
    size_t pos = 2;
    if (!get_u32(p, pos, m.checksum) || !get_u32(p, pos, m.byte_count))
        return false;
    return get_str16(p, pos, m.path) && get_str16(p, pos, m.reason);
}

}

#endif
