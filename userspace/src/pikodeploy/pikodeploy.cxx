/*
 * pikodeploy -- replaces tools/chunked-deploy.sh's "ship it over SSH"
 * job. Plain headless CLI (no FLTK, no GUI event loop -- it's a
 * synchronous, one-thing-at-a-time program, so plain blocking sockets are
 * simpler and sufficient here, unlike pikoxfer-client's FLTK-driven
 * FileSend). Talks to pikoxfer-server (which must already be open on the
 * device) over the same wire protocol pikoxfer's file transfer uses, just
 * the deploy message set (MSG_PUT_OFFER etc, see protocol.h).
 *
 * Same flag surface chunked-deploy.sh accepts today: --adapter IFACE,
 * --target/positional user@host, --kernel-only, --no-userspace,
 * --create-backup-files, --replace-dropbear. build-and-deploy.sh's final
 * line execs this instead of chunked-deploy.sh -- see that script's own
 * comment at the exec call.
 *
 * WHY A CLEAR "pikoxfer-server isn't open" MESSAGE MATTERS: unlike SSH,
 * which chunked-deploy.sh could always reach as long as dropbear was
 * running, deploy now needs the GUI app open on the device (see
 * manifest.h and the pikodeploy plan) -- a bare connection-refused here
 * would be a confusing regression from "just works over SSH" without an
 * explicit nudge toward the actual fix.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "../pikoxfer/protocol.h"
#include "../pikoxfer/net_io.h"
#include "manifest.h"

using namespace pikoxfer;
using namespace pikoxfer::deploy;

static std::string g_adapter;
static std::string g_host = "10.208.47.2"; /* same default as pikoxfer-client's Transfer tab */
/* SD, not NAND: the whole point of this option is that most deploy
 * destinations live on the ~68 MiB NAND root, and staging a transfer's
 * .part there defeats the purpose of having a separate staging area at
 * all whenever a card is available -- see resolve_staging_dir() in
 * pikoxfer-server.cxx. Falls back to whatever the user picks explicitly
 * (--staging nand) when no card is in. */
static uint32_t g_staging = STAGE_SD;
static int g_port = static_cast<int>(DEFAULT_PORT);

/* ---------------------------------------------------------------------- *
 * Low-level networking: blocking connect (with optional --adapter        *
 * source-interface binding) and a FrameReader-backed blocking recv.       *
 * ---------------------------------------------------------------------- */

static bool get_interface_address(const std::string &iface, struct in_addr &out)
{
    struct ifaddrs *ifap = 0;
    if (getifaddrs(&ifap) != 0)
        return false;
    bool found = false;
    for (struct ifaddrs *p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_name || !p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
            continue;
        if (iface != p->ifa_name)
            continue;
        struct sockaddr_in *sin = reinterpret_cast<struct sockaddr_in *>(p->ifa_addr);
        out = sin->sin_addr;
        found = true;
        break;
    }
    freeifaddrs(ifap);
    return found;
}

static int connect_blocking(std::string &error)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { error = "socket() failed"; return -1; }

    /* --adapter's non-privileged equivalent of `ssh -B`: bind the LOCAL
     * side of the socket to that interface's own address before
     * connect(), rather than SO_BINDTODEVICE (which needs CAP_NET_RAW on
     * Linux -- not available to a normal host user running this CLI). */
    if (!g_adapter.empty()) {
        struct in_addr local_addr;
        if (get_interface_address(g_adapter, local_addr)) {
            struct sockaddr_in local;
            memset(&local, 0, sizeof(local));
            local.sin_family = AF_INET;
            local.sin_addr = local_addr;
            bind(fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local)); /* best effort */
        } else {
            fprintf(stderr, "pikodeploy: warning: no IPv4 address found on interface %s\n",
                    g_adapter.c_str());
        }
    }

    /* 30s, matching the ConnectTimeout=30 this project's SSH-based
     * scripts (chunked-deploy.sh, build-and-deploy.sh) already settled
     * on for this exact link -- 10s turned out too short live: a
     * blocking connect() with SO_SNDTIMEO set returns EINPROGRESS (not
     * ETIMEDOUT) on timeout, a Linux-specific quirk, and this link's
     * RTT is routinely 400-1700ms and occasionally much worse. */
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(g_port));
    if (inet_pton(AF_INET, g_host.c_str(), &addr.sin_addr) != 1) {
        error = "invalid device address: " + g_host;
        close(fd);
        return -1;
    }

    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        error = std::string("connect to ") + g_host + " failed: " + strerror(errno)
                + " (is pikoxfer-server open on the device?)";
        close(fd);
        return -1;
    }
    return fd;
}

static bool recv_frame_blocking(int fd, FrameReader &reader, uint32_t &type, std::string &payload)
{
    for (;;) {
        FrameReader::Result r = reader.next(type, payload);
        if (r == FrameReader::GOT_FRAME)
            return true;
        if (r == FrameReader::DESYNC)
            return false;
        char buf[16384];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            return false;
        reader.feed(buf, static_cast<size_t>(n));
    }
}

static bool do_hello(int fd, FrameReader &reader, std::string &error)
{
    HelloMsg h;
    h.version = PROTO_VERSION;
    if (!send_frame_blocking(fd, MSG_HELLO, encode(h))) { error = "send HELLO failed"; return false; }

    uint32_t type;
    std::string payload;
    if (!recv_frame_blocking(fd, reader, type, payload)) { error = "no HELLO_ACK"; return false; }
    if (type == MSG_ERROR) {
        ErrorMsg em;
        decode_error(payload, em);
        error = em.message;
        return false;
    }
    if (type != MSG_HELLO_ACK) { error = "unexpected reply to HELLO"; return false; }
    HelloMsg ack;
    if (!decode_hello(payload, ack) || ack.version != PROTO_VERSION) {
        error = "protocol version mismatch";
        return false;
    }
    return true;
}

/* One connection, one request/response -- used by mkdir/symlink/run/
 * query_existing/free_space, all cheap enough that a fresh connection per
 * call costs nothing that matters (see pikoxfer-server.cxx's Connection
 * class header comment for why one-op-per-connection was kept simple
 * rather than a request loop). */
static bool simple_request(uint32_t req_type, const std::string &req_payload,
                            uint32_t expect_ack_type, std::string &resp_payload, std::string &error)
{
    int fd = connect_blocking(error);
    if (fd < 0)
        return false;

    FrameReader reader;
    if (!do_hello(fd, reader, error)) { close(fd); return false; }
    if (!send_frame_blocking(fd, req_type, req_payload)) { close(fd); error = "send failed"; return false; }

    uint32_t type;
    if (!recv_frame_blocking(fd, reader, type, resp_payload)) { close(fd); error = "no response"; return false; }
    close(fd);

    if (type == MSG_ERROR) {
        ErrorMsg em;
        decode_error(resp_payload, em);
        error = em.message;
        return false;
    }
    if (type != expect_ack_type) { error = "unexpected reply type"; return false; }
    return true;
}

/* ---------------------------------------------------------------------- *
 * Chunked, resumable file transfer -- the client side of MSG_PUT_OFFER.   *
 * Retries with backoff across a dropped connection (reconnect + re-offer *
 * -> server reports how far it already got), same resilience pikoxfer's  *
 * plain file transfer already proved live.                                *
 * ---------------------------------------------------------------------- */

static bool compute_file_crc32_and_size(const std::string &path, uint32_t &crc_out, uint64_t &size_out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    Crc32 crc;
    char buf[65536];
    size_t n;
    uint64_t total = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc.update(buf, n);
        total += n;
    }
    fclose(f);
    crc_out = crc.final_value();
    size_out = total;
    return true;
}

static const int MAX_SEND_ATTEMPTS = 30; /* generous: this is the whole point of resume */

static bool send_put_file(const Step &s, std::string &error)
{
    uint32_t crc;
    uint64_t size;
    if (!compute_file_crc32_and_size(s.local_path, crc, size)) {
        error = "cannot read " + s.local_path;
        return false;
    }

    for (int attempt = 0; attempt < MAX_SEND_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            int delay = attempt < 5 ? attempt * 2 : 10;
            fprintf(stderr, "  (reconnecting in %ds, attempt %d/%d)\n", delay, attempt + 1, MAX_SEND_ATTEMPTS);
            sleep(static_cast<unsigned>(delay));
        }

        int fd = connect_blocking(error);
        if (fd < 0) continue;
        FrameReader reader;
        if (!do_hello(fd, reader, error)) { close(fd); continue; }

        PutOfferMsg po;
        po.path = s.remote_path;
        po.total_size = size;
        po.mode = s.mode;
        po.policy = s.policy;
        po.crc32 = crc;
        po.backup = s.backup;
        po.staging = g_staging;
        if (!send_frame_blocking(fd, MSG_PUT_OFFER, encode(po))) { close(fd); continue; }

        uint32_t type;
        std::string payload;
        if (!recv_frame_blocking(fd, reader, type, payload)) { close(fd); continue; }
        if (type == MSG_ERROR) {
            ErrorMsg em; decode_error(payload, em);
            error = em.message; close(fd); return false; /* not a transport drop -- don't retry blindly */
        }
        if (type != MSG_PUT_OFFER_ACK) { close(fd); continue; }
        PutOfferAckMsg ack;
        if (!decode_put_offer_ack(payload, ack)) { close(fd); continue; }

        if (ack.outcome == PUT_REJECTED) { error = ack.reason; close(fd); return false; }
        if (ack.outcome == PUT_ALREADY_SATISFIED) { close(fd); return true; }

        FILE *f = fopen(s.local_path.c_str(), "rb");
        if (!f) { error = "cannot reopen " + s.local_path; close(fd); return false; }
        if (fseek(f, static_cast<long>(ack.resume_offset), SEEK_SET) != 0) {
            fclose(f); close(fd); continue;
        }

        uint64_t offset = ack.resume_offset;
        bool dropped = false;
        char buf[MAX_CHUNK];
        while (offset < size) {
            size_t n = fread(buf, 1, sizeof(buf), f);
            if (n == 0) { dropped = true; break; }

            DataChunkMsg dc;
            dc.offset = offset;
            dc.data.assign(buf, n);
            if (!send_frame_blocking(fd, MSG_DATA_CHUNK, encode(dc))) { dropped = true; break; }

            if (!recv_frame_blocking(fd, reader, type, payload) || type != MSG_CHUNK_ACK) {
                dropped = true;
                break;
            }
            ChunkAckMsg ca;
            if (!decode_chunk_ack(payload, ca)) { dropped = true; break; }
            offset = ca.bytes_written;
        }
        fclose(f);

        if (dropped) { close(fd); continue; }

        FileCompleteMsg fc;
        fc.crc32 = crc;
        if (!send_frame_blocking(fd, MSG_FILE_COMPLETE, encode(fc))) { close(fd); continue; }
        if (!recv_frame_blocking(fd, reader, type, payload) || type != MSG_FILE_COMPLETE_ACK) {
            close(fd); continue;
        }
        FileCompleteAckMsg fca;
        if (!decode_file_complete_ack(payload, fca)) { close(fd); continue; }
        close(fd);

        if (fca.ok)
            return true;
        if (fca.reason.find("checksum") != std::string::npos)
            continue; /* server already reset itself to 0; a fresh full resend will succeed */
        error = fca.reason;
        return false;
    }

    error = "gave up after " + std::string(error.empty() ? "repeated connection failures" : error);
    return false;
}

/* ---------------------------------------------------------------------- *
 * Step execution                                                          *
 * ---------------------------------------------------------------------- */

static bool run_tar(const std::string &tar_path, const std::string &dest_dir)
{
    mkdir(dest_dir.c_str(), 0755);
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execlp("tar", "tar", "xf", tar_path.c_str(), "-C", dest_dir.c_str(), static_cast<char *>(0));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Sum of every step's bytes, for MSG_DEPLOY_BEGIN -- see its comment in
 * protocol.h for why the server needs this up front rather than learning
 * about one file at a time. put_tar_tree counts the tar's own archive
 * size as a stand-in for its expanded contents (the real size isn't known
 * until execute_step() actually extracts it); everything else is an exact
 * stat(). A step whose local file has since vanished contributes 0 rather
 * than aborting here -- execute_step() will report that failure properly,
 * with a useful message, when it actually gets there. */
static uint64_t compute_plan_total_bytes(const std::vector<Step> &plan)
{
    uint64_t total = 0;
    struct stat st;
    for (size_t i = 0; i < plan.size(); i++) {
        const Step &s = plan[i];
        const std::string *path = 0;
        if (s.type == STEP_PUT_FILE) path = &s.local_path;
        else if (s.type == STEP_EXTRACT_TAR_TREE) path = &s.local_tar_path;
        else continue;
        if (stat(path->c_str(), &st) == 0)
            total += static_cast<uint64_t>(st.st_size);
    }
    return total;
}

/* --dry-run's listing -- no network, no filesystem access beyond what
 * build_plan() already did. */
static std::string describe_step(const Step &s)
{
    char mode[8];
    snprintf(mode, sizeof(mode), "%04o", s.mode);
    switch (s.type) {
    case STEP_PUT_FILE:
        return "put_file  " + s.local_path + " -> " + s.remote_path + "  (mode " + mode
             + (s.policy == PUT_IF_MISSING ? ", if_missing" : "")
             + (s.backup ? ", backup" : "") + ")";
    case STEP_MKDIR:
        return "mkdir     " + s.remote_path;
    case STEP_SYMLINK:
        return "symlink   " + s.remote_path + " -> " + s.local_path;
    case STEP_RUN:
        return "run       op " + std::string(s.run_op == RUN_MOUNT_SD_CARD ? "mount_sd_card" : "?");
    case STEP_EXTRACT_TAR_TREE:
        return "put_tar_tree " + s.local_tar_path + " -> " + s.tar_remote_base
             + "  (expanded to individual files at run time)";
    }
    return "?";
}

static bool execute_step(const Step &s, std::string &error)
{
    switch (s.type) {
    case STEP_PUT_FILE:
        printf("==> %s -> %s\n", s.local_path.c_str(), s.remote_path.c_str());
        return send_put_file(s, error);

    case STEP_MKDIR: {
        printf("==> mkdir %s\n", s.remote_path.c_str());
        PathMsg m; m.path = s.remote_path;
        std::string resp;
        if (!simple_request(MSG_MKDIR, encode(m), MSG_MKDIR_ACK, resp, error)) return false;
        OkReasonMsg ack;
        if (!decode_ok_reason(resp, ack)) { error = "malformed MKDIR_ACK"; return false; }
        if (!ack.ok) { error = ack.reason; return false; }
        return true;
    }

    case STEP_SYMLINK: {
        printf("==> symlink %s -> %s\n", s.remote_path.c_str(), s.local_path.c_str());
        SymlinkMsg m; m.target = s.local_path; m.linkname = s.remote_path;
        std::string resp;
        if (!simple_request(MSG_SYMLINK, encode(m), MSG_SYMLINK_ACK, resp, error)) return false;
        OkReasonMsg ack;
        if (!decode_ok_reason(resp, ack)) { error = "malformed SYMLINK_ACK"; return false; }
        if (!ack.ok) { error = ack.reason; return false; }
        return true;
    }

    case STEP_RUN: {
        printf("==> run op %u\n", s.run_op);
        RunMsg m; m.op = s.run_op;
        std::string resp;
        if (!simple_request(MSG_RUN, encode(m), MSG_RUN_ACK, resp, error)) return false;
        OkReasonMsg ack;
        if (!decode_ok_reason(resp, ack)) { error = "malformed RUN_ACK"; return false; }
        if (!ack.ok) { error = ack.reason; return false; }
        return true;
    }

    case STEP_EXTRACT_TAR_TREE: {
        printf("==> X11/Matchbox payload (%s)\n", s.local_tar_path.c_str());
        char tmpl[] = "/tmp/pikodeploy-x11.XXXXXX";
        if (!mkdtemp(tmpl)) { error = "mkdtemp failed"; return false; }
        std::string tmpdir = tmpl;
        if (!run_tar(s.local_tar_path, tmpdir)) {
            error = "tar extraction failed";
            std::string rm = "rm -rf '" + tmpdir + "'";
            if (system(rm.c_str()) != 0) { /* best effort cleanup */ }
            return false;
        }

        std::vector<Step> files;
        bool ok = put_files_from_tree(tmpdir, s.tar_remote_base, files, error);
        std::string rm = "rm -rf '" + tmpdir + "'";
        if (!ok) { if (system(rm.c_str()) != 0) { } return false; }

        printf("    %zu files to send\n", files.size());
        for (size_t i = 0; i < files.size(); i++) {
            if (!execute_step(files[i], error)) {
                if (system(rm.c_str()) != 0) { }
                return false;
            }
        }
        if (system(rm.c_str()) != 0) { }
        return true;
    }
    }
    error = "unknown step type";
    return false;
}

/* ---------------------------------------------------------------------- *
 * userspace_media's dynamic parts: MPlayer's card-verified destination,   *
 * SDL's version-suffixed .so + symlink. Not in manifest.yaml -- see its   *
 * header and manifest.h's for why.                                        *
 * ---------------------------------------------------------------------- */

static void append_mplayer_and_sdl_steps(const DeployContext &ctx, std::vector<Step> &out)
{
    std::string mplayer_bin = ctx.mplayer_stage + "/usr/bin/mplayer";
    if (path_exists(mplayer_bin)) {
        std::string card_dest = "/mnt/card/.zaurus/usr/bin/mplayer";
        std::string error;
        RunMsg m; m.op = RUN_MOUNT_SD_CARD;
        std::string resp;
        if (simple_request(MSG_RUN, encode(m), MSG_RUN_ACK, resp, error)) {
            OkReasonMsg ack;
            if (decode_ok_reason(resp, ack) && ack.ok) {
                Step s;
                s.type = STEP_PUT_FILE;
                s.local_path = mplayer_bin;
                s.remote_path = card_dest;
                s.mode = 0755;
                s.policy = PUT_ALWAYS;
                out.push_back(s);
            } else {
                printf("==> no SD card mounted -- skipping MPlayer (card-only, see manifest.h)\n");
            }
        } else {
            printf("==> could not verify SD card mount (%s) -- skipping MPlayer\n", error.c_str());
        }
    }

    /* SDL's .so carries a version suffix (libSDL-1.2.so.0.<patch>) --
     * find it rather than assume one, same as chunked-deploy.sh's own
     * `ls libSDL-1.2.so.0.* | head -1`. */
    std::string sdl_lib_dir = ctx.sdl_stage + "/usr/lib";
    DIR *d = opendir(sdl_lib_dir.c_str());
    if (d) {
        struct dirent *e;
        std::string found;
        while ((e = readdir(d)) != 0) {
            std::string name(e->d_name);
            if (name.compare(0, 15, "libSDL-1.2.so.0") == 0 && name.size() > 15) {
                found = name;
                break;
            }
        }
        closedir(d);
        if (!found.empty()) {
            Step put;
            put.type = STEP_PUT_FILE;
            put.local_path = sdl_lib_dir + "/" + found;
            put.remote_path = "/usr/lib/" + found;
            put.mode = 0755;
            put.policy = PUT_ALWAYS;
            out.push_back(put);

            Step link;
            link.type = STEP_SYMLINK;
            link.local_path = found; /* symlink target */
            link.remote_path = "/usr/lib/libSDL-1.2.so.0"; /* linkname */
            out.push_back(link);
        }
    }
}

/* ---------------------------------------------------------------------- *
 * main                                                                     *
 * ---------------------------------------------------------------------- */

static bool g_dry_run = false;
static bool g_probe = false;

static void usage()
{
    fprintf(stderr,
        "Usage: pikodeploy [--adapter IFACE] [--kernel-only] [--no-userspace]\n"
        "                   [--create-backup-files] [--replace-dropbear]\n"
        "                   [--staging nand|sd|cf] [--dry-run] [--probe] [user@host]\n"
        "\n"
        "--staging chooses where pikoxfer-server stages each file's .part while\n"
        "it's being received -- NOT where it ends up (that's each file's own\n"
        "destination, from manifest.yaml). Default sd: most deploy destinations\n"
        "live on the ~68 MiB NAND root, so staging there too would risk the exact\n"
        "ENOSPC problem chunked-deploy.sh's REMOTE_STAGE fix solved, just for\n"
        "deploy instead of plain transfer. nand always works (/tmp); sd needs a\n"
        "mounted card (/mnt/card/.zaurus/tmp, autocleaned by the ROM on boot); cf\n"
        "is refused today -- real CF mounting does not exist in this ROM yet.\n"
        "\n"
        "--dry-run builds and prints the deploy plan (every put_file/mkdir/symlink/\n"
        "run step, in order) without connecting to the device at all -- useful to\n"
        "preview what a run would do, and needs no pikoxfer-server open anywhere.\n"
        "\n"
        "--probe just connects and does the HELLO handshake, then exits 0 or 1 --\n"
        "no manifest, no repo tree needed. build-and-deploy.sh uses this to fail\n"
        "fast, before spending time building, if pikoxfer-server isn't open on\n"
        "the device.\n");
}

/* A handful of retries, not the ~30 send_put_file allows itself: this is
 * "fail fast before a long build", so it should still tolerate one bad
 * moment on a known-flaky link rather than a single blip wasting the
 * whole build -- but it should not spend minutes retrying either. */
static const int PROBE_ATTEMPTS = 4;

static int run_probe()
{
    std::string error;
    int fd = -1;
    for (int attempt = 0; attempt < PROBE_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            fprintf(stderr, "  (retrying, attempt %d/%d)\n", attempt + 1, PROBE_ATTEMPTS);
            sleep(2);
        }
        fd = connect_blocking(error);
        if (fd >= 0)
            break;
    }
    if (fd < 0) {
        fprintf(stderr, "FAILED: %s\n", error.c_str());
        return 1;
    }
    FrameReader reader;
    if (!do_hello(fd, reader, error)) {
        fprintf(stderr, "FAILED: %s\n", error.c_str());
        close(fd);
        return 1;
    }
    close(fd);
    printf("%s:%d is reachable and speaking the pikoxfer protocol.\n", g_host.c_str(), g_port);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    DeployContext ctx;
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)))
        ctx.repo = cwd;
    if (const char *root = getenv("PIKOXFER_REPO_ROOT"))
        if (*root) ctx.repo = root;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--adapter" && i + 1 < argc) { g_adapter = argv[++i]; }
        else if (a == "--kernel-only") { ctx.flags.kernel_only = true; }
        else if (a == "--no-userspace") { ctx.flags.no_userspace = true; }
        else if (a == "--create-backup-files") { ctx.flags.create_backup_files = true; }
        else if (a == "--replace-dropbear") { ctx.flags.replace_dropbear = true; }
        else if (a == "--staging" && i + 1 < argc) {
            std::string v = argv[++i];
            if (v == "nand") g_staging = STAGE_NAND;
            else if (v == "sd") g_staging = STAGE_SD;
            else if (v == "cf") g_staging = STAGE_CF;
            else {
                fprintf(stderr, "pikodeploy: --staging must be nand, sd, or cf (got \"%s\")\n", v.c_str());
                return 1;
            }
        }
        else if (a == "--dry-run") { g_dry_run = true; }
        else if (a == "--probe") { g_probe = true; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (!a.empty() && a[0] != '-') {
            std::string::size_type at = a.find('@');
            g_host = (at == std::string::npos) ? a : a.substr(at + 1);
        } else {
            fprintf(stderr, "pikodeploy: unknown option %s\n", a.c_str());
            usage();
            return 1;
        }
    }

    if (g_probe)
        return run_probe();

    ctx.kernel_dir = ctx.repo + "/kernel-src/linux-7.1.4";
    ctx.ssh_stage = ctx.repo + "/userspace/stage-ssh";
    ctx.alsa_stage = ctx.repo + "/userspace/stage-alsa-runtime";
    ctx.mplayer_stage = ctx.repo + "/userspace/stage-mplayer";
    ctx.sdl_stage = ctx.repo + "/userspace/stage-sdl-runtime";
    ctx.tcroot = ctx.repo + "/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/arm-unknown-linux-uclibcgnueabi/sysroot";
    /* Same default and same env var name build-and-deploy.sh already
     * exports before invoking this (see its X11_PAYLOAD/X11_PAYLOAD_TAR
     * comment) -- /tmp, not $REPO, matching tools/build-matchbox-payload.sh
     * and chunked-deploy.sh's existing convention. */
    ctx.x11_payload = "/tmp/matchbox-payload.tar";
    if (const char *xp = getenv("X11_PAYLOAD"))
        if (*xp) ctx.x11_payload = xp;

    std::string kver_path = ctx.kernel_dir + "/include/config/kernel.release";
    std::string kver_text;
    if (read_whole_file(kver_path, kver_text)) {
        std::string::size_type nl = kver_text.find('\n');
        ctx.kver = (nl == std::string::npos) ? kver_text : kver_text.substr(0, nl);
    }

    if (!path_exists(ctx.kernel_dir + "/arch/arm/boot/zImage")) {
        fprintf(stderr, "pikodeploy: expected built kernel image missing under %s\n", ctx.kernel_dir.c_str());
        fprintf(stderr, "Run tools/build-and-deploy.sh (which builds first), or set PIKOXFER_REPO_ROOT.\n");
        return 1;
    }

    std::string manifest_path = ctx.repo + "/userspace/src/pikodeploy/manifest.yaml";
    std::string manifest_text;
    if (!read_whole_file(manifest_path, manifest_text)) {
        fprintf(stderr, "pikodeploy: cannot read %s\n", manifest_path.c_str());
        return 1;
    }
    std::vector<yaml::Section> sections;
    std::string error;
    if (!yaml::parse(manifest_text, sections, error)) {
        fprintf(stderr, "pikodeploy: %s\n", error.c_str());
        return 1;
    }

    std::vector<std::string> which = select_sections(ctx.flags);
    std::vector<Step> plan;
    if (!build_plan(sections, which, ctx, plan, error)) {
        fprintf(stderr, "pikodeploy: %s\n", error.c_str());
        return 1;
    }

    /* manifest.yaml's x11_matchbox entry is if_exists-gated on
     * X11_PAYLOAD, so build_plan() silently omits it when the tar isn't
     * there -- correct for every other if_exists case (an optional,
     * "only if built" artifact), but this specific one is exactly the
     * failure mode chunked-deploy.sh used to print an explicit note
     * about, not skip silently: a machine without the X11 toolchain
     * provisioned still gets a kernel/modules deploy, and the missing
     * desktop should be obvious, not discovered later by hand. */
    if (!ctx.flags.kernel_only && !path_exists(ctx.x11_payload)) {
        printf("==> no X11 payload at %s -- skipping\n", ctx.x11_payload.c_str());
        printf("    (build it with tools/build-matchbox-payload.sh)\n");
    }

    if (!ctx.flags.kernel_only && !ctx.flags.no_userspace && !g_dry_run)
        append_mplayer_and_sdl_steps(ctx, plan);

    const char *staging_name = g_staging == STAGE_SD ? "sd" : g_staging == STAGE_CF ? "cf" : "nand";

    if (g_dry_run) {
        printf("%zu steps (dry run -- not connecting to %s:%d, staging=%s)\n",
               plan.size(), g_host.c_str(), g_port, staging_name);
        printf("(MPlayer/SDL's card-mount-verified steps need a live connection to\n"
               " compute and are not shown here -- see append_mplayer_and_sdl_steps)\n\n");
        for (size_t i = 0; i < plan.size(); i++)
            printf("%s\n", describe_step(plan[i]).c_str());
        return 0;
    }

    printf("Target: %s:%d (staging=%s)\n", g_host.c_str(), g_port, staging_name);
    printf("%zu steps to run\n", plan.size());

    /* Best-effort: a deploy-wide progress bar is a nicety, not something
     * worth failing an otherwise-working deploy over if this one extra
     * connection has a problem. */
    {
        DeployBeginMsg begin;
        begin.total_bytes = compute_plan_total_bytes(plan);
        std::string resp, begin_error;
        if (!simple_request(MSG_DEPLOY_BEGIN, encode(begin), MSG_DEPLOY_BEGIN_ACK, resp, begin_error))
            fprintf(stderr, "  (note: could not announce deploy size to the server: %s)\n",
                    begin_error.c_str());
    }

    for (size_t i = 0; i < plan.size(); i++) {
        if (!execute_step(plan[i], error)) {
            fprintf(stderr, "FAILED: %s\n", error.c_str());
            return 1;
        }
    }

    printf("\nAll steps deployed and verified. Reboot the device when ready.\n");
    return 0;
}
