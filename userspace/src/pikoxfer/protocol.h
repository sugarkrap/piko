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
    MSG_ERROR             = 9, /* either direction */

    /* pikodeploy's message set, sharing this same connection/frame format.
     * MSG_DATA_CHUNK/MSG_CHUNK_ACK/MSG_FILE_COMPLETE/MSG_FILE_COMPLETE_ACK
     * above are reused as-is for the actual bytes of a MSG_PUT_OFFER --
     * only the offer/finalize semantics differ from a plain MSG_FILE_OFFER
     * (an absolute destination path instead of a collision-resolved name,
     * an explicit mode, and an ALWAYS/IF_MISSING policy). See
     * pikodeploy/README.md. */
    MSG_PUT_OFFER          = 10, /* client -> server */
    MSG_PUT_OFFER_ACK      = 11, /* server -> client */
    MSG_MKDIR              = 12, /* client -> server */
    MSG_MKDIR_ACK          = 13, /* server -> client */
    MSG_SYMLINK            = 14, /* client -> server */
    MSG_SYMLINK_ACK        = 15, /* server -> client */
    MSG_RUN                = 16, /* client -> server */
    MSG_RUN_ACK            = 17, /* server -> client */
    MSG_QUERY_EXISTING     = 18, /* client -> server */
    MSG_QUERY_EXISTING_ACK = 19, /* server -> client */
    MSG_FREE_SPACE         = 20, /* client -> server */
    MSG_FREE_SPACE_ACK     = 21, /* server -> client */

    /* Sent once, on its own short-lived connection, before any PUT_OFFER
     * -- see DeployBeginMsg below for why. */
    MSG_DEPLOY_BEGIN       = 22, /* client -> server */
    MSG_DEPLOY_BEGIN_ACK   = 23  /* server -> client */
};

enum PutPolicy {
    PUT_ALWAYS     = 0, /* overwrite unconditionally (after the usual
                          * already-matches-so-skip check) */
    PUT_IF_MISSING = 1  /* leave alone if the destination already exists,
                          * regardless of its content -- chunked-deploy.sh's
                          * current /etc/TZ and touchscreen.cfg handling */
};

/* Where the server stages a PUT_FILE's ".part" while it's being received,
 * chosen by the client (the Build & Deploy tab's Destination dropdown /
 * pikodeploy's --staging flag), not decided unilaterally by the server.
 * Matters because most deploy DESTINATIONS live on NAND (/boot, /etc,
 * /usr/sbin, ...) -- staging there too would reintroduce the exact
 * ENOSPC problem chunked-deploy.sh's REMOTE_STAGE fix solved, just for
 * deploy instead of plain transfer. See PutOfferAckMsg's finalize
 * comment in pikoxfer-server.cxx for how staging on a different
 * filesystem than the destination is reconciled (copy, then the same
 * same-directory rename() every other case uses). */
enum StagingKind {
    STAGE_NAND = 0, /* /tmp -- always available, the safe zero-config default */
    STAGE_SD   = 1, /* /mnt/card/.zaurus/tmp -- preferred when a card is in */
    STAGE_CF   = 2  /* /mnt/cf/.zaurus/tmp -- not yet supported on real
                      * hardware; the server rejects this today rather
                      * than silently falling back, see AGENTS.md */
};

enum PutOutcome {
    PUT_RESUME            = 0, /* resume_offset valid, may be 0 (fresh) */
    PUT_ALREADY_SATISFIED = 1, /* nothing to send: content already matches
                                 * (ALWAYS) or destination already exists
                                 * (IF_MISSING) */
    PUT_REJECTED          = 2  /* reason valid */
};

/* Deliberately just one op: replacing chunked-deploy.sh's "stop the
 * graphical session before unpacking the X11 payload" step turned out to
 * be unnecessary, not merely unported -- every PUT_FILE finalizes via a
 * same-directory rename() (see MSG_PUT_OFFER above), which is exactly
 * how send_file's rename-aside already replaces running binaries like
 * brightd/flipd today without stopping them. A running Xfbdev/matchbox
 * keeps executing from its old, unlinked-but-open inode the same way. */
enum RunOp {
    RUN_MOUNT_SD_CARD = 1
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

/* ---------------------------------------------------------------------- *
 * pikodeploy messages                                                     *
 * ---------------------------------------------------------------------- */

struct PutOfferMsg {
    std::string path;    /* absolute destination path, not collision-resolved */
    uint64_t total_size;
    uint32_t mode;        /* permission bits, e.g. 0755 as the integer 493 */
    uint32_t policy;      /* PutPolicy */
    uint32_t crc32;        /* whole-file CRC, sent up front so the server can
                            * answer ALREADY_SATISFIED without a transfer */
    bool backup;          /* copy the existing file to path+".bak" first */
    uint32_t staging;      /* StagingKind */
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
    uint32_t outcome;      /* PutOutcome */
    uint64_t resume_offset; /* valid when outcome == PUT_RESUME */
    std::string reason;    /* valid when outcome == PUT_REJECTED */
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

/* Sent once, before the first PUT_OFFER of a deploy run, so the server can
 * show progress across the WHOLE run instead of only what it has heard
 * about so far. Without this, the server only learns about one file at a
 * time (each on its own short-lived connection) -- a file that is
 * PUT_ALREADY_SATISFIED never even gets a row, and one that IS sent only
 * contributes its own size to the aggregate once its PUT_OFFER arrives, so
 * the aggregate bar could read 100% between files while 100 more were
 * still queued behind it (seen live 2026-08-03: the bar sat at 100% for
 * most of a 119-step deploy). total_bytes is the sum of every step's
 * bytes as pikodeploy's own plan sees it -- put_tar_tree steps (the
 * X11/Matchbox payload) count the tar's own archive size as a stand-in
 * for its expanded contents, since the tar is not extracted (and its
 * true expanded size not known) until execute_step() actually reaches
 * that step; close enough for a progress indicator, not used for
 * anything that needs to be exact. */
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

struct PathMsg { /* shared shape for MKDIR and QUERY_EXISTING/FREE_SPACE requests */
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

struct OkReasonMsg { /* shared shape for MKDIR_ACK/SYMLINK_ACK/RUN_ACK/DEPLOY_BEGIN_ACK */
    bool ok;
    std::string reason; /* valid when !ok */
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
    uint32_t op; /* RunOp */
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
    uint64_t size; /* valid when exists */
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

} /* namespace pikoxfer */

#endif /* PIKOXFER_PROTOCOL_H */
