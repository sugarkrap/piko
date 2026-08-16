
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

#include "../piko-sync/protocol.h"
#include "../piko-sync/net_io.h"
#include "manifest.h"

using namespace piko_sync;
using namespace piko_sync::deploy;

static std::string g_adapter;
static std::string g_host = "10.208.47.2";
static uint32_t g_staging = STAGE_SD;
static int g_port = static_cast<int>(DEFAULT_PORT);

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

    if (!g_adapter.empty()) {
        struct in_addr local_addr;
        if (get_interface_address(g_adapter, local_addr)) {
            struct sockaddr_in local;
            memset(&local, 0, sizeof(local));
            local.sin_family = AF_INET;
            local.sin_addr = local_addr;
            bind(fd, reinterpret_cast<struct sockaddr *>(&local), sizeof(local));
        } else {
            fprintf(stderr, "piko-sync-deploy: warning: no IPv4 address found on interface %s\n",
                    g_adapter.c_str());
        }
    }

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
                + " (is piko-sync-server open on the device?)";
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

static const int MAX_SEND_ATTEMPTS = 30;

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
            error = em.message; close(fd); return false;
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
            continue;
        error = fca.reason;
        return false;
    }

    error = "gave up after " + std::string(error.empty() ? "repeated connection failures" : error);
    return false;
}

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
        char tmpl[] = "/tmp/piko-sync-deploy-x11.XXXXXX";
        if (!mkdtemp(tmpl)) { error = "mkdtemp failed"; return false; }
        std::string tmpdir = tmpl;
        if (!run_tar(s.local_tar_path, tmpdir)) {
            error = "tar extraction failed";
            std::string rm = "rm -rf '" + tmpdir + "'";
            if (system(rm.c_str()) != 0) {   }
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
            link.local_path = found;
            link.remote_path = "/usr/lib/libSDL-1.2.so.0";
            out.push_back(link);
        }
    }
}

static bool g_dry_run = false;
static bool g_probe = false;

struct AdHocPut {
    std::string local_path, remote_path;
    uint32_t mode;
};
static std::vector<AdHocPut> g_ad_hoc_puts;

static void usage()
{
    fprintf(stderr,
        "Usage: piko-sync-deploy [--adapter IFACE] [--kernel-only] [--no-userspace]\n"
        "                   [--create-backup-files] [--replace-dropbear]\n"
        "                   [--staging nand|sd|cf] [--dry-run] [--probe]\n"
        "                   [--put LOCAL:REMOTE[:MODE] ...] [user@host]\n"
        "\n"
        "--put LOCAL:REMOTE[:MODE] ships exactly that one file to that one\n"
        "absolute path, bypassing manifest.yaml entirely -- repeat --put for more\n"
        "than one file in the same run. MODE is an octal permission string, e.g.\n"
        "0755; omit it to keep LOCAL's own permission bits. Still goes through\n"
        "the resilient PUT_OFFER protocol (resume, CRC verify, --staging,\n"
        "--create-backup-files all apply), just without needing a manifest entry\n"
        "or a full deploy plan for a one-off file.\n"
        "\n"
        "--staging chooses where piko-sync-server stages each file's .part while\n"
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
        "preview what a run would do, and needs no piko-sync-server open anywhere.\n"
        "\n"
        "--probe just connects and does the HELLO handshake, then exits 0 or 1 --\n"
        "no manifest, no repo tree needed. build-and-deploy.sh uses this to fail\n"
        "fast, before spending time building, if piko-sync-server isn't open on\n"
        "the device.\n");
}

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
    printf("%s:%d is reachable and speaking the piko-sync protocol.\n", g_host.c_str(), g_port);
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    DeployContext ctx;
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)))
        ctx.repo = cwd;
    if (const char *root = getenv("PIKO_SYNC_REPO_ROOT"))
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
                fprintf(stderr, "piko-sync-deploy: --staging must be nand, sd, or cf (got \"%s\")\n", v.c_str());
                return 1;
            }
        }
        else if (a == "--dry-run") { g_dry_run = true; }
        else if (a == "--probe") { g_probe = true; }
        else if (a == "--put" && i + 1 < argc) {
            std::string v = argv[++i];
            std::string::size_type c1 = v.find(':');
            std::string::size_type c2 = (c1 == std::string::npos) ? std::string::npos : v.find(':', c1 + 1);
            if (c1 == std::string::npos) {
                fprintf(stderr, "piko-sync-deploy: --put needs LOCAL:REMOTE[:MODE] (got \"%s\")\n", v.c_str());
                return 1;
            }
            AdHocPut p;
            p.local_path = v.substr(0, c1);
            p.remote_path = (c2 == std::string::npos) ? v.substr(c1 + 1) : v.substr(c1 + 1, c2 - c1 - 1);
            if (c2 != std::string::npos) {
                p.mode = parse_mode(v.substr(c2 + 1));
            } else {
                struct stat st;
                p.mode = (stat(p.local_path.c_str(), &st) == 0) ? (st.st_mode & 0777) : 0644;
            }
            g_ad_hoc_puts.push_back(p);
        }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (!a.empty() && a[0] != '-') {
            std::string::size_type at = a.find('@');
            g_host = (at == std::string::npos) ? a : a.substr(at + 1);
        } else {
            fprintf(stderr, "piko-sync-deploy: unknown option %s\n", a.c_str());
            usage();
            return 1;
        }
    }

    if (g_probe)
        return run_probe();

    if (!g_ad_hoc_puts.empty()) {
        const char *staging_name = g_staging == STAGE_SD ? "sd" : g_staging == STAGE_CF ? "cf" : "nand";
        std::vector<Step> plan;
        uint64_t total_bytes = 0;
        for (size_t i = 0; i < g_ad_hoc_puts.size(); i++) {
            Step s;
            s.type = STEP_PUT_FILE;
            s.local_path = g_ad_hoc_puts[i].local_path;
            s.remote_path = g_ad_hoc_puts[i].remote_path;
            s.mode = g_ad_hoc_puts[i].mode;
            s.policy = PUT_ALWAYS;
            s.backup = ctx.flags.create_backup_files;
            plan.push_back(s);

            struct stat st;
            if (stat(s.local_path.c_str(), &st) == 0)
                total_bytes += static_cast<uint64_t>(st.st_size);
        }

        if (g_dry_run) {
            printf("%zu step(s) (dry run -- not connecting to %s:%d, staging=%s)\n",
                   plan.size(), g_host.c_str(), g_port, staging_name);
            for (size_t i = 0; i < plan.size(); i++)
                printf("%s\n", describe_step(plan[i]).c_str());
            return 0;
        }

        printf("Target: %s:%d (staging=%s)\n", g_host.c_str(), g_port, staging_name);
        printf("%zu step(s) to run\n", plan.size());

        {
            DeployBeginMsg begin;
            begin.total_bytes = total_bytes;
            std::string resp, begin_error;
            if (!simple_request(MSG_DEPLOY_BEGIN, encode(begin), MSG_DEPLOY_BEGIN_ACK, resp, begin_error))
                fprintf(stderr, "  (note: could not announce deploy size to the server: %s)\n",
                        begin_error.c_str());
        }

        std::string error;
        for (size_t i = 0; i < plan.size(); i++) {
            if (!execute_step(plan[i], error)) {
                fprintf(stderr, "FAILED: %s\n", error.c_str());
                return 1;
            }
        }

        printf("\nAll file(s) deployed and verified.\n");
        return 0;
    }

    ctx.kernel_dir = ctx.repo + "/kernel-src/linux-7.1.4";
    ctx.ssh_stage = ctx.repo + "/userspace/stage-ssh";
    ctx.alsa_stage = ctx.repo + "/userspace/stage-alsa-runtime";
    ctx.mplayer_stage = ctx.repo + "/userspace/stage-mplayer";
    ctx.sdl_stage = ctx.repo + "/userspace/stage-sdl-runtime";
    ctx.tcroot = ctx.repo + "/toolchain/x-tools/arm-unknown-linux-uclibcgnueabi/arm-unknown-linux-uclibcgnueabi/sysroot";
    ctx.x11_payload = "/tmp/matchbox-payload.tar";
    if (const char *xp = getenv("X11_PAYLOAD"))
        if (*xp) ctx.x11_payload = xp;

    ctx.timidity_stage = ctx.repo + "/userspace/stage-timidity";
    if (const char *ts = getenv("TIMIDITY_STAGE"))
        if (*ts) ctx.timidity_stage = ts;
    ctx.timidity_dir = "/mnt/card/.zaurus/usr/share/timidity";
    if (const char *td = getenv("TIMIDITY_DIR"))
        if (*td) ctx.timidity_dir = td;

    std::string kver_path = ctx.kernel_dir + "/include/config/kernel.release";
    std::string kver_text;
    if (read_whole_file(kver_path, kver_text)) {
        std::string::size_type nl = kver_text.find('\n');
        ctx.kver = (nl == std::string::npos) ? kver_text : kver_text.substr(0, nl);
    }

    if (!path_exists(ctx.kernel_dir + "/arch/arm/boot/zImage")) {
        fprintf(stderr, "piko-sync-deploy: expected built kernel image missing under %s\n", ctx.kernel_dir.c_str());
        fprintf(stderr, "Run tools/build-and-deploy.sh (which builds first), or set PIKO_SYNC_REPO_ROOT.\n");
        return 1;
    }

    std::string manifest_path = ctx.repo + "/userspace/src/piko-sync-deploy/manifest.yaml";
    std::string manifest_text;
    if (!read_whole_file(manifest_path, manifest_text)) {
        fprintf(stderr, "piko-sync-deploy: cannot read %s\n", manifest_path.c_str());
        return 1;
    }
    std::vector<yaml::Section> sections;
    std::string error;
    if (!yaml::parse(manifest_text, sections, error)) {
        fprintf(stderr, "piko-sync-deploy: %s\n", error.c_str());
        return 1;
    }

    std::vector<std::string> which = select_sections(ctx.flags);
    std::vector<Step> plan;
    if (!build_plan(sections, which, ctx, plan, error)) {
        fprintf(stderr, "piko-sync-deploy: %s\n", error.c_str());
        return 1;
    }

    if (!ctx.flags.kernel_only && !path_exists(ctx.x11_payload)) {
        printf("==> no X11 payload at %s -- skipping\n", ctx.x11_payload.c_str());
        printf("    (build it with tools/userspace/build-matchbox-payload.sh)\n");
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
