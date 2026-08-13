
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Pixmap.H>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "protocol.h"
#include "transfer_state.h"
#include "transfer_queue.h"
#include "transfer_table.h"
#include "net_io.h"
#include "icon_xpm.h"

using namespace piko_sync;

static const char *TRANSFERS_DIR = "/mnt/card/Transfers";
static const char *PART_SUFFIX = ".piko-sync-part";

static const int JFFS2_EAGAIN_RETRIES = 20;
static const int JFFS2_EAGAIN_DELAY_US = 100000;

static ssize_t write_retry(int fd, const void *buf, size_t count)
{
    for (int attempt = 0; ; attempt++) {
        ssize_t n = write(fd, buf, count);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static ssize_t pwrite_retry(int fd, const void *buf, size_t count, off_t offset)
{
    for (int attempt = 0; ; attempt++) {
        ssize_t n = pwrite(fd, buf, count, offset);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static int rename_retry(const char *oldpath, const char *newpath)
{
    for (int attempt = 0; ; attempt++) {
        if (rename(oldpath, newpath) == 0)
            return 0;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static int mkdir_retry(const char *path, mode_t mode)
{
    for (int attempt = 0; ; attempt++) {
        if (mkdir(path, mode) == 0)
            return 0;
        if (errno == EEXIST)
            return 0;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static int open_retry(const char *path, int flags, mode_t mode)
{
    for (int attempt = 0; ; attempt++) {
        int fd = open(path, flags, mode);
        if (fd >= 0)
            return fd;
        if (errno == EAGAIN && attempt < JFFS2_EAGAIN_RETRIES) {
            usleep(JFFS2_EAGAIN_DELAY_US);
            continue;
        }
        return -1;
    }
}

static std::string dirname_of(const std::string &path)
{
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return "/";
    return path.substr(0, slash);
}

static bool free_bytes_on(const std::string &path, uint64_t &out)
{
    std::string probe = path;
    struct stat st;
    while (!probe.empty() && stat(probe.c_str(), &st) != 0) {
        std::string parent = dirname_of(probe);
        if (parent == probe)
            break;
        probe = parent;
    }
    if (probe.empty())
        probe = "/";

    struct statvfs sv;
    if (statvfs(probe.c_str(), &sv) != 0)
        return false;
    out = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
    return true;
}

static void names_in_dir(const std::string &dir, std::vector<std::string> &out)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    std::string suffix(PART_SUFFIX);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        std::string name(e->d_name);
        if (name == "." || name == "..")
            continue;
        if (name.size() >= suffix.size()
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            continue;
        struct stat st;
        if (stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        out.push_back(name);
    }
    closedir(d);
}

static bool mkdir_p(const std::string &path)
{
    if (path.empty() || path == "/")
        return true;
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    if (!mkdir_p(dirname_of(path)))
        return false;
    return mkdir_retry(path.c_str(), 0755) == 0;
}

static bool is_sd_card_mounted()
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, " /mnt/card ")) { found = true; break; }
    }
    fclose(f);
    return found;
}

static std::string staging_filename_for(const std::string &dest)
{
    std::string s = dest;
    if (!s.empty() && s[0] == '/')
        s.erase(0, 1);
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] == '/')
            s[i] = '_';
    return s;
}

static bool resolve_staging_dir(uint32_t staging, std::string &dir, std::string &error)
{
    switch (staging) {
    case STAGE_NAND:
        dir = "/tmp";
        return true;
    case STAGE_SD:
        system("mount /mnt/card >/dev/null 2>&1");
        if (!is_sd_card_mounted()) { error = "SD card is not mounted"; return false; }
        dir = "/mnt/card/.zaurus/tmp";
        return mkdir_p(dir);
    case STAGE_CF:
        error = "CF staging is not yet supported on this device";
        return false;
    default:
        error = "unknown staging kind";
        return false;
    }
}

static bool copy_file(const std::string &src, const std::string &dst)
{
    int in = open(src.c_str(), O_RDONLY);
    if (in < 0)
        return false;
    int out = open_retry(dst.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out < 0) { close(in); return false; }

    char buf[65536];
    ssize_t n;
    bool ok = true;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t w = write_retry(out, buf, static_cast<size_t>(n));
        if (w != n) { ok = false; break; }
    }
    if (n < 0)
        ok = false;

    close(in);
    close(out);
    return ok;
}

class ServerApp;

class Connection {
public:
    Connection(int fd, ServerApp *app);
    ~Connection();

    void close_connection();

private:
    enum Phase { WAIT_HELLO, WAIT_OFFER, RECEIVING, CLOSED };

    static void read_cb(int, void *v) { static_cast<Connection *>(v)->on_read(); }
    void on_read();
    void handle_frame(uint32_t type, const std::string &payload);
    void handle_hello(const std::string &payload);
    void handle_offer(const std::string &payload);
    void handle_chunk(const std::string &payload);
    void handle_complete(const std::string &payload);
    void handle_deploy_complete(const FileCompleteMsg &fc);

    void handle_put_offer(const std::string &payload);
    void handle_mkdir(const std::string &payload);
    void handle_symlink(const std::string &payload);
    void handle_run(const std::string &payload);
    void handle_query_existing(const std::string &payload);
    void handle_free_space(const std::string &payload);
    void handle_deploy_begin(const std::string &payload);
    void handle_screenshot(const std::string &payload);

    std::string dest_dir_;

    bool send(uint32_t type, const std::string &payload);
    void fail(const std::string &reason);
    static void deferred_delete_cb(void *v);

    ServerApp *app_;
    int fd_;
    FrameReader reader_;
    Phase phase_;

    std::string original_name_;
    std::string final_name_;
    uint64_t total_size_;
    uint64_t next_offset_;
    bool already_fully_done_;
    int row_;
    int part_fd_;

    bool is_deploy_;
    uint32_t deploy_mode_;
    bool deploy_backup_;
    std::string staging_part_path_;
};

class ServerApp {
public:
    ServerApp(int X, int Y, int W, int H);
    ~ServerApp();

    Fl_Widget *resizable_widget() { return table_; }

    TransferQueue &queue() { return queue_; }
    TransferMap &transfer_map() { return transfer_map_; }
    const std::vector<std::string> &complete_names() const { return complete_names_; }
    void note_complete_name(const std::string &name) { complete_names_.push_back(name); }
    DeploySession &deploy_session() { return deploy_session_; }

    void set_status(const std::string &text)
    {
        status_label_->copy_label(text.c_str());
        status_label_->redraw();
    }

    void sync_table()
    {
        table_->sync();
        double pct = deploy_session_.active() ? deploy_session_.percent() : queue_.aggregate_percent();
        aggregate_bar_->value(static_cast<float>(pct));
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%d%%", static_cast<int>(pct + 0.5));
        aggregate_bar_->copy_label(lbl);
        aggregate_bar_->redraw();
    }

    void forget_connection(Connection *c)
    {
        for (size_t i = 0; i < connections_.size(); i++) {
            if (connections_[i] == c) {
                connections_.erase(connections_.begin() + i);
                break;
            }
        }
    }

private:
    static void accept_cb(int, void *v) { static_cast<ServerApp *>(v)->on_accept(); }
    void on_accept();

    static void refresh_address_cb(void *v)
    {
        static_cast<ServerApp *>(v)->refresh_address();
        Fl::repeat_timeout(3.0, refresh_address_cb, v);
    }
    void refresh_address();

    void scan_existing();

    Fl_Box *address_box_;
    Fl_Box *status_label_;
    Fl_Progress *aggregate_bar_;
    TransferTable *table_;
    TransferQueue queue_;
    DeploySession deploy_session_;
    TransferMap transfer_map_;
    std::vector<std::string> complete_names_;
    int listen_fd_;
    std::vector<Connection *> connections_;
};

Connection::Connection(int fd, ServerApp *app)
    : app_(app), fd_(fd), phase_(WAIT_HELLO),
      total_size_(0), next_offset_(0), already_fully_done_(false),
      row_(-1), part_fd_(-1),
      is_deploy_(false), deploy_mode_(0644), deploy_backup_(false)
{
    set_nonblock(fd_);
    Fl::add_fd(fd_, FL_READ, read_cb, this);
}

Connection::~Connection()
{
    if (fd_ >= 0) { Fl::remove_fd(fd_); close(fd_); }
    if (part_fd_ >= 0) close(part_fd_);
}

bool Connection::send(uint32_t type, const std::string &payload)
{
    if (!send_frame_blocking(fd_, type, payload)) {
        close_connection();
        return false;
    }
    return true;
}

void Connection::fail(const std::string &reason)
{
    ErrorMsg em;
    em.message = reason;
    send_frame_blocking(fd_, MSG_ERROR, encode(em));
    close_connection();
}

void Connection::close_connection()
{
    if (phase_ == CLOSED)
        return;
    phase_ = CLOSED;

    if (row_ >= 0) {
        const TransferRow &r = app_->queue().row(row_);
        if (r.status == XFER_TRANSFERRING)
            app_->queue().set_status(row_, XFER_RECONNECTING);
        app_->sync_table();
    }

    if (fd_ >= 0)      { Fl::remove_fd(fd_); close(fd_); fd_ = -1; }
    if (part_fd_ >= 0) { close(part_fd_); part_fd_ = -1; }

    app_->forget_connection(this);

    Fl::add_timeout(0.0, deferred_delete_cb, this);
}

void Connection::deferred_delete_cb(void *v)
{
    delete static_cast<Connection *>(v);
}

void Connection::on_read()
{
    char buf[16384];
    ssize_t n = read(fd_, buf, sizeof(buf));

    if (n == 0) { close_connection(); return; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return;
        close_connection();
        return;
    }

    reader_.feed(buf, static_cast<size_t>(n));

    for (;;) {
        uint32_t type;
        std::string payload;
        FrameReader::Result r = reader_.next(type, payload);
        if (r == FrameReader::NEED_MORE)
            return;
        if (r == FrameReader::DESYNC) {
            close_connection();
            return;
        }
        handle_frame(type, payload);
        if (phase_ == CLOSED)
            return;
    }
}

void Connection::handle_frame(uint32_t type, const std::string &payload)
{
    switch (phase_) {
    case WAIT_HELLO:
        if (type != MSG_HELLO) { fail("expected HELLO"); return; }
        handle_hello(payload);
        return;
    case WAIT_OFFER:
        switch (type) {
        case MSG_FILE_OFFER:       handle_offer(payload); return;
        case MSG_PUT_OFFER:        handle_put_offer(payload); return;
        case MSG_MKDIR:            handle_mkdir(payload); return;
        case MSG_SYMLINK:          handle_symlink(payload); return;
        case MSG_RUN:              handle_run(payload); return;
        case MSG_QUERY_EXISTING:   handle_query_existing(payload); return;
        case MSG_FREE_SPACE:       handle_free_space(payload); return;
        case MSG_DEPLOY_BEGIN:     handle_deploy_begin(payload); return;
        case MSG_SCREENSHOT:       handle_screenshot(payload); return;
        default:
            fail("expected an offer or a deploy request");
            return;
        }
    case RECEIVING:
        if (type == MSG_DATA_CHUNK) { handle_chunk(payload); return; }
        if (type == MSG_FILE_COMPLETE) { handle_complete(payload); return; }
        fail("expected DATA_CHUNK or FILE_COMPLETE");
        return;
    case CLOSED:
        return;
    }
}

void Connection::handle_hello(const std::string &payload)
{
    HelloMsg h;
    if (!decode_hello(payload, h)) { fail("malformed HELLO"); return; }
    if (h.version != PROTO_VERSION) {
        char msg[64];
        snprintf(msg, sizeof(msg), "unsupported protocol version %u", h.version);
        fail(msg);
        return;
    }

    HelloMsg ack; ack.version = PROTO_VERSION;
    if (!send(MSG_HELLO_ACK, encode(ack)))
        return;
    phase_ = WAIT_OFFER;
}

void Connection::handle_offer(const std::string &payload)
{
    FileOfferMsg fo;
    if (!decode_file_offer(payload, fo)) { fail("malformed FILE_OFFER"); return; }
    if (fo.name.empty() || fo.name.find('/') != std::string::npos) {
        fail("invalid file name");
        return;
    }
    if (fo.total_size == 0) {
        FileOfferAckMsg ack;
        ack.accepted = false;
        ack.reason = "empty files are not supported";
        send(MSG_FILE_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    dest_dir_ = TRANSFERS_DIR;
    if (!fo.dest_dir.empty()) {
        if (fo.dest_dir[0] != '/') {
            FileOfferAckMsg ack;
            ack.accepted = false;
            ack.reason = "destination must be an absolute path";
            send(MSG_FILE_OFFER_ACK, encode(ack));
            close_connection();
            return;
        }
        std::string want = fo.dest_dir;
        while (want.size() > 1 && want[want.size() - 1] == '/')
            want.erase(want.size() - 1);
        if (!mkdir_p(want) || access(want.c_str(), W_OK) != 0) {
            char msg[320];
            snprintf(msg, sizeof(msg), "cannot write to %s: %s",
                     want.c_str(), strerror(errno));
            FileOfferAckMsg ack;
            ack.accepted = false;
            ack.reason = msg;
            send(MSG_FILE_OFFER_ACK, encode(ack));
            close_connection();
            return;
        }
        dest_dir_ = want;
    }

    original_name_ = fo.name;
    total_size_ = fo.total_size;

    TransferKey key(fo.name, fo.total_size);
    TransferMap::const_iterator it = app_->transfer_map().find(key);
    already_fully_done_ = (it != app_->transfer_map().end() && it->second.complete);

    std::vector<std::string> other_names;
    const std::vector<std::string> *names = &app_->complete_names();
    if (dest_dir_ != TRANSFERS_DIR) {
        names_in_dir(dest_dir_, other_names);
        names = &other_names;
    }

    OfferDecision d = decide_offer(app_->transfer_map(), fo.name, fo.total_size,
                                    *names);
    final_name_ = d.final_name;
    next_offset_ = d.resume_offset;

    if (next_offset_ < total_size_) {
        std::string part_path = dest_dir_ + "/" + final_name_ + PART_SUFFIX;
        part_fd_ = open(part_path.c_str(), O_CREAT | O_RDWR, 0644);
        if (part_fd_ < 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "cannot open %s: %s", part_path.c_str(), strerror(errno));
            FileOfferAckMsg ack;
            ack.accepted = false;
            ack.reason = msg;
            send(MSG_FILE_OFFER_ACK, encode(ack));
            close_connection();
            return;
        }
    }

    row_ = app_->queue().find_or_add(final_name_, total_size_);
    app_->queue().set_status(row_, XFER_TRANSFERRING);
    app_->queue().set_progress(row_, next_offset_);
    app_->set_status("Receiving " + final_name_ + "...");
    app_->sync_table();

    FileOfferAckMsg ack;
    ack.accepted = true;
    ack.final_name = final_name_;
    ack.resume_offset = next_offset_;
    if (!send(MSG_FILE_OFFER_ACK, encode(ack)))
        return;
    phase_ = RECEIVING;
}

void Connection::handle_chunk(const std::string &payload)
{
    DataChunkMsg dc;
    if (!decode_data_chunk(payload, dc)) { fail("malformed DATA_CHUNK"); return; }

    if (dc.offset != next_offset_) { fail("unexpected chunk offset"); return; }
    if (next_offset_ + dc.data.size() > total_size_) { fail("chunk exceeds file size"); return; }
    if (part_fd_ < 0) { fail("no data expected for this file"); return; }

    ssize_t w = pwrite_retry(part_fd_, dc.data.data(), dc.data.size(),
                              static_cast<off_t>(dc.offset));
    if (w < 0 || static_cast<size_t>(w) != dc.data.size()) {
        char msg[128];
        snprintf(msg, sizeof(msg), "write failed: %s", strerror(errno));
        fail(msg);
        return;
    }

    next_offset_ += dc.data.size();
    if (!is_deploy_)
        app_->transfer_map()[TransferKey(original_name_, total_size_)].bytes_on_disk = next_offset_;
    else
        app_->deploy_session().add_bytes(dc.data.size());

    if (row_ >= 0) {
        app_->queue().set_progress(row_, next_offset_);
        app_->sync_table();
    }

    ChunkAckMsg ack; ack.bytes_written = next_offset_;
    send(MSG_CHUNK_ACK, encode(ack));
}

void Connection::handle_complete(const std::string &payload)
{
    FileCompleteMsg fc;
    if (!decode_file_complete(payload, fc)) { fail("malformed FILE_COMPLETE"); return; }

    if (is_deploy_) {
        handle_deploy_complete(fc);
        return;
    }

    TransferKey key(original_name_, total_size_);

    if (already_fully_done_) {
        FileCompleteAckMsg ack; ack.ok = true;
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        app_->queue().set_status(row_, XFER_DONE);
        app_->queue().set_progress(row_, total_size_);
        app_->sync_table();
        close_connection();
        return;
    }

    if (next_offset_ != total_size_) {
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = "incomplete: not all bytes were received";
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        close_connection();
        return;
    }

    app_->set_status("Verifying " + final_name_ + "...");
    Crc32 crc;
    if (lseek(part_fd_, 0, SEEK_SET) == 0) {
        char buf[65536];
        for (;;) {
            ssize_t n = read(part_fd_, buf, sizeof(buf));
            if (n <= 0) break;
            crc.update(buf, static_cast<size_t>(n));
        }
    }

    if (crc.final_value() != fc.crc32) {
        ftruncate(part_fd_, 0);
        app_->transfer_map()[key].bytes_on_disk = 0;

        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = "checksum mismatch -- please retry";
        send(MSG_FILE_COMPLETE_ACK, encode(ack));

        app_->queue().set_status(row_, XFER_ERROR, "checksum mismatch");
        app_->sync_table();
        close_connection();
        return;
    }

    std::string part_path = dest_dir_ + "/" + final_name_ + PART_SUFFIX;
    std::string final_path = dest_dir_ + "/" + final_name_;
    close(part_fd_);
    part_fd_ = -1;

    if (rename_retry(part_path.c_str(), final_path.c_str()) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "could not finalize: %s", strerror(errno));
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = msg;
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        app_->queue().set_status(row_, XFER_ERROR, msg);
        app_->sync_table();
        close_connection();
        return;
    }

    app_->transfer_map()[key].complete = true;
    app_->note_complete_name(final_name_);

    FileCompleteAckMsg ack; ack.ok = true;
    send(MSG_FILE_COMPLETE_ACK, encode(ack));
    app_->queue().set_status(row_, XFER_DONE);
    app_->sync_table();
    close_connection();
}

void Connection::handle_put_offer(const std::string &payload)
{
    PutOfferMsg po;
    if (!decode_put_offer(payload, po)) { fail("malformed PUT_OFFER"); return; }
    if (po.path.empty() || po.path[0] != '/') { fail("PUT_OFFER path must be absolute"); return; }
    if (po.total_size == 0) {
        PutOfferAckMsg ack;
        ack.outcome = PUT_REJECTED;
        ack.reason = "empty files are not supported";
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    is_deploy_ = true;
    original_name_ = po.path;
    total_size_ = po.total_size;
    deploy_mode_ = po.mode;
    deploy_backup_ = po.backup;

    struct stat st;
    bool exists = (stat(po.path.c_str(), &st) == 0);

    if (po.policy == PUT_IF_MISSING && exists) {
        app_->deploy_session().add_bytes(po.total_size);
        app_->sync_table();
        PutOfferAckMsg ack;
        ack.outcome = PUT_ALREADY_SATISFIED;
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    if (exists && static_cast<uint64_t>(st.st_size) == po.total_size) {
        int fd = open(po.path.c_str(), O_RDONLY);
        if (fd >= 0) {
            Crc32 crc;
            char buf[65536];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                crc.update(buf, static_cast<size_t>(n));
            close(fd);
            if (crc.final_value() == po.crc32) {
                app_->deploy_session().add_bytes(po.total_size);
                app_->sync_table();
                PutOfferAckMsg ack;
                ack.outcome = PUT_ALREADY_SATISFIED;
                send(MSG_PUT_OFFER_ACK, encode(ack));
                close_connection();
                return;
            }
        }
    }

    uint64_t dest_free = 0;
    if (free_bytes_on(po.path, dest_free) && dest_free < po.total_size) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "not enough free space on the destination filesystem "
                 "(need %llu bytes, %llu available)",
                 static_cast<unsigned long long>(po.total_size),
                 static_cast<unsigned long long>(dest_free));
        PutOfferAckMsg ack;
        ack.outcome = PUT_REJECTED;
        ack.reason = msg;
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    std::string staging_dir;
    std::string staging_error;
    if (!resolve_staging_dir(po.staging, staging_dir, staging_error)) {
        PutOfferAckMsg ack;
        ack.outcome = PUT_REJECTED;
        ack.reason = staging_error;
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    staging_part_path_ = staging_dir + "/" + staging_filename_for(po.path) + PART_SUFFIX;
    uint64_t resume_offset = 0;
    struct stat pst;
    if (stat(staging_part_path_.c_str(), &pst) == 0) {
        resume_offset = static_cast<uint64_t>(pst.st_size);
        if (resume_offset > po.total_size)
            resume_offset = 0;
    }

    part_fd_ = open(staging_part_path_.c_str(), O_CREAT | O_RDWR, 0644);
    if (part_fd_ < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot open %s: %s", staging_part_path_.c_str(), strerror(errno));
        PutOfferAckMsg ack;
        ack.outcome = PUT_REJECTED;
        ack.reason = msg;
        send(MSG_PUT_OFFER_ACK, encode(ack));
        close_connection();
        return;
    }

    next_offset_ = resume_offset;
    row_ = app_->queue().find_or_add(po.path, po.total_size);
    app_->queue().set_status(row_, XFER_TRANSFERRING);
    app_->queue().set_progress(row_, next_offset_);
    app_->set_status("Receiving " + po.path + "...");
    app_->sync_table();

    PutOfferAckMsg ack;
    ack.outcome = PUT_RESUME;
    ack.resume_offset = next_offset_;
    if (!send(MSG_PUT_OFFER_ACK, encode(ack)))
        return;
    phase_ = RECEIVING;
}

void Connection::handle_deploy_complete(const FileCompleteMsg &fc)
{
    if (next_offset_ != total_size_) {
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = "incomplete: not all bytes were received";
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        close_connection();
        return;
    }

    app_->set_status("Verifying " + original_name_ + "...");
    Crc32 crc;
    if (lseek(part_fd_, 0, SEEK_SET) == 0) {
        char buf[65536];
        for (;;) {
            ssize_t n = read(part_fd_, buf, sizeof(buf));
            if (n <= 0) break;
            crc.update(buf, static_cast<size_t>(n));
        }
    }

    if (crc.final_value() != fc.crc32) {
        ftruncate(part_fd_, 0);

        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = "checksum mismatch -- please retry";
        send(MSG_FILE_COMPLETE_ACK, encode(ack));

        if (row_ >= 0) {
            app_->queue().set_status(row_, XFER_ERROR, "checksum mismatch");
            app_->sync_table();
        }
        close_connection();
        return;
    }

    const std::string &dest = original_name_;
    close(part_fd_);
    part_fd_ = -1;

    if (!mkdir_p(dirname_of(dest))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "cannot create directory for %s: %s", dest.c_str(), strerror(errno));
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = msg;
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        if (row_ >= 0) {
            app_->queue().set_status(row_, XFER_ERROR, msg);
            app_->sync_table();
        }
        close_connection();
        return;
    }

    if (deploy_backup_) {
        struct stat st;
        if (stat(dest.c_str(), &st) == 0)
            rename(dest.c_str(), (dest + ".bak").c_str());
    }

    struct stat staging_dir_st, dest_dir_st;
    std::string staging_dir = dirname_of(staging_part_path_);
    std::string dest_dir = dirname_of(dest);
    bool same_fs = (stat(staging_dir.c_str(), &staging_dir_st) == 0
                     && stat(dest_dir.c_str(), &dest_dir_st) == 0
                     && staging_dir_st.st_dev == dest_dir_st.st_dev);

    bool finalized;
    if (same_fs) {
        finalized = (rename_retry(staging_part_path_.c_str(), dest.c_str()) == 0);
    } else {
        std::string local_tmp = dest + PART_SUFFIX;
        finalized = copy_file(staging_part_path_, local_tmp)
                 && rename_retry(local_tmp.c_str(), dest.c_str()) == 0;
        int finalize_errno = errno;
        if (finalized)
            unlink(staging_part_path_.c_str());
        else
            unlink(local_tmp.c_str());
        errno = finalize_errno;
    }

    if (!finalized) {
        char msg[256];
        snprintf(msg, sizeof(msg), "could not finalize: %s", strerror(errno));
        FileCompleteAckMsg ack;
        ack.ok = false;
        ack.reason = msg;
        send(MSG_FILE_COMPLETE_ACK, encode(ack));
        if (row_ >= 0) {
            app_->queue().set_status(row_, XFER_ERROR, msg);
            app_->sync_table();
        }
        close_connection();
        return;
    }
    chmod(dest.c_str(), deploy_mode_);

    FileCompleteAckMsg ack;
    ack.ok = true;
    send(MSG_FILE_COMPLETE_ACK, encode(ack));
    if (row_ >= 0) {
        app_->queue().set_status(row_, XFER_DONE);
        app_->sync_table();
    }
    close_connection();
}

void Connection::handle_mkdir(const std::string &payload)
{
    PathMsg m;
    if (!decode_path(payload, m)) { fail("malformed MKDIR"); return; }

    OkReasonMsg ack;
    ack.ok = mkdir_p(m.path);
    if (!ack.ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "mkdir failed: %s", strerror(errno));
        ack.reason = msg;
    }
    send(MSG_MKDIR_ACK, encode(ack));
    close_connection();
}

void Connection::handle_symlink(const std::string &payload)
{
    SymlinkMsg m;
    if (!decode_symlink(payload, m)) { fail("malformed SYMLINK"); return; }

    unlink(m.linkname.c_str());

    OkReasonMsg ack;
    ack.ok = (symlink(m.target.c_str(), m.linkname.c_str()) == 0);
    if (!ack.ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "symlink failed: %s", strerror(errno));
        ack.reason = msg;
    }
    send(MSG_SYMLINK_ACK, encode(ack));
    close_connection();
}

void Connection::handle_run(const std::string &payload)
{
    RunMsg m;
    if (!decode_run(payload, m)) { fail("malformed RUN"); return; }

    OkReasonMsg ack;
    if (m.op == RUN_MOUNT_SD_CARD) {
        system("mount /mnt/card >/dev/null 2>&1");
        ack.ok = is_sd_card_mounted();
        if (!ack.ok)
            ack.reason = "SD card is not mounted";
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "unknown run op %u", m.op);
        ack.ok = false;
        ack.reason = msg;
    }
    send(MSG_RUN_ACK, encode(ack));
    close_connection();
}

void Connection::handle_query_existing(const std::string &payload)
{
    PathMsg m;
    if (!decode_path(payload, m)) { fail("malformed QUERY_EXISTING"); return; }

    struct stat st;
    QueryExistingAckMsg ack;
    if (stat(m.path.c_str(), &st) == 0) {
        ack.exists = true;
        ack.size = static_cast<uint64_t>(st.st_size);
    } else {
        ack.exists = false;
    }
    send(MSG_QUERY_EXISTING_ACK, encode(ack));
    close_connection();
}

void Connection::handle_free_space(const std::string &payload)
{
    PathMsg m;
    if (!decode_path(payload, m)) { fail("malformed FREE_SPACE"); return; }

    FreeSpaceAckMsg ack;
    free_bytes_on(m.path, ack.free_bytes);
    send(MSG_FREE_SPACE_ACK, encode(ack));
    close_connection();
}

static bool capture_framebuffer(std::string &out, uint32_t &width,
                                uint32_t &height, uint32_t &bpp,
                                std::string &err)
{
    const char *dev = "/dev/fb0";
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        err = std::string("cannot open ") + dev + ": " + strerror(errno);
        return false;
    }

    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0) {
        err = std::string("framebuffer ioctl: ") + strerror(errno);
        close(fd);
        return false;
    }
    if (var.bits_per_pixel != 16) {
        char buf[96];
        snprintf(buf, sizeof(buf), "unsupported depth: %u bpp (expected 16)",
                 var.bits_per_pixel);
        err = buf;
        close(fd);
        return false;
    }

    size_t rowbytes = static_cast<size_t>(var.xres) * var.bits_per_pixel / 8;
    size_t maplen = static_cast<size_t>(fix.line_length) * (var.yres + var.yoffset);
    unsigned char *fb = static_cast<unsigned char *>(
        mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, 0));
    if (fb == MAP_FAILED) {
        err = std::string("mmap framebuffer: ") + strerror(errno);
        close(fd);
        return false;
    }

    out.clear();
    out.reserve(rowbytes * var.yres);
    for (unsigned int y = 0; y < var.yres; y++) {
        const unsigned char *row = fb
            + static_cast<size_t>(y + var.yoffset) * fix.line_length
            + static_cast<size_t>(var.xoffset) * var.bits_per_pixel / 8;
        out.append(reinterpret_cast<const char *>(row), rowbytes);
    }

    munmap(fb, maplen);
    close(fd);

    width = var.xres;
    height = var.yres;
    bpp = var.bits_per_pixel;
    return true;
}

void Connection::handle_screenshot(const std::string &payload)
{
    (void)payload;

    std::string pixels, err;
    ScreenshotInfoMsg info;
    if (!capture_framebuffer(pixels, info.width, info.height, info.bpp, err)) {
        info.ok = false;
        info.reason = err;
        send(MSG_SCREENSHOT_INFO, encode(info));
        close_connection();
        return;
    }

    info.ok = true;
    info.byte_count = static_cast<uint32_t>(pixels.size());
    send(MSG_SCREENSHOT_INFO, encode(info));

    Crc32 crc;
    size_t off = 0;
    while (off < pixels.size()) {
        size_t n = pixels.size() - off;
        if (n > MAX_CHUNK)
            n = MAX_CHUNK;
        DataChunkMsg chunk;
        chunk.offset = off;
        chunk.data.assign(pixels, off, n);
        crc.update(pixels.data() + off, n);
        if (!send_frame_blocking(fd_, MSG_DATA_CHUNK, encode(chunk)))
            return;
        off += n;
    }

    FileCompleteMsg done;
    done.crc32 = crc.final_value();
    send(MSG_FILE_COMPLETE, encode(done));
    close_connection();
}

void Connection::handle_deploy_begin(const std::string &payload)
{
    DeployBeginMsg m;
    if (!decode_deploy_begin(payload, m)) { fail("malformed DEPLOY_BEGIN"); return; }

    app_->deploy_session().begin(m.total_bytes);
    app_->sync_table();

    OkReasonMsg ack; ack.ok = true;
    send(MSG_DEPLOY_BEGIN_ACK, encode(ack));
    close_connection();
}

ServerApp::ServerApp(int X, int Y, int W, int H)
    : listen_fd_(-1)
{
    int m = 10;
    address_box_ = new Fl_Box(X + m, Y + m, W - 2 * m, 40);
    address_box_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    address_box_->label("starting...");

    status_label_ = new Fl_Box(X + m, Y + m + 44, W - 2 * m, 16);
    status_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    status_label_->labelfont(FL_HELVETICA_ITALIC);
    status_label_->labelsize(12);
    status_label_->label("");

    aggregate_bar_ = new Fl_Progress(X + m, Y + m + 62, W - 2 * m, 20);
    aggregate_bar_->minimum(0);
    aggregate_bar_->maximum(100);
    aggregate_bar_->value(0);
    aggregate_bar_->color(FL_BACKGROUND_COLOR);
    aggregate_bar_->selection_color(FL_BLUE);
    aggregate_bar_->label("0%");

    table_ = new TransferTable(X + m, Y + m + 90, W - 2 * m, H - m - 90 - m);
    table_->queue(&queue_);

    mkdir(TRANSFERS_DIR, 0755);

    if (access(TRANSFERS_DIR, W_OK) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "SD card not available: %s is not writable", TRANSFERS_DIR);
        address_box_->copy_label(msg);
        return;
    }

    scan_existing();
    sync_table();

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ >= 0) {
        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(DEFAULT_PORT);

        if (bind(listen_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) == 0
            && listen(listen_fd_, 16) == 0) {
            set_nonblock(listen_fd_);
            Fl::add_fd(listen_fd_, FL_READ, accept_cb, this);
        } else {
            close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    refresh_address();
    Fl::add_timeout(3.0, refresh_address_cb, this);
}

ServerApp::~ServerApp()
{
    while (!connections_.empty())
        connections_.back()->close_connection();
    if (listen_fd_ >= 0) {
        Fl::remove_fd(listen_fd_);
        close(listen_fd_);
    }
    Fl::remove_timeout(refresh_address_cb, this);
}

void ServerApp::scan_existing()
{
    DIR *d = opendir(TRANSFERS_DIR);
    if (!d)
        return;

    std::string suffix(PART_SUFFIX);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        std::string name(e->d_name);
        if (name == "." || name == "..")
            continue;
        if (name.size() >= suffix.size()
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            continue;

        std::string path = std::string(TRANSFERS_DIR) + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        int row = queue_.add(name, static_cast<uint64_t>(st.st_size));
        queue_.set_status(row, XFER_DONE);
        queue_.set_progress(row, static_cast<uint64_t>(st.st_size));
        complete_names_.push_back(name);
    }
    closedir(d);
}

void ServerApp::refresh_address()
{
    std::string ip = wlan0_address();
    char msg[256];
    if (ip.empty()) {
        snprintf(msg, sizeof(msg), "Waiting for WiFi (wlan0)...");
    } else if (listen_fd_ < 0) {
        snprintf(msg, sizeof(msg), "%s -- could not start listening on port %u",
                 ip.c_str(), static_cast<unsigned>(DEFAULT_PORT));
    } else {
        snprintf(msg, sizeof(msg), "Listening on %s:%u", ip.c_str(),
                 static_cast<unsigned>(DEFAULT_PORT));
    }
    address_box_->copy_label(msg);
    address_box_->redraw();
}

void ServerApp::on_accept()
{
    for (;;) {
        int fd = accept(listen_fd_, 0, 0);
        if (fd < 0)
            return;
        connections_.push_back(new Connection(fd, this));
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    Fl_Double_Window win(640, 480, "Piko Sync");
    win.begin();
    ServerApp app(0, 0, 640, 480);
    win.end();
    win.resizable(app.resizable_widget());

    static Fl_Pixmap icon_pixmap(piko_sync_icon_xpm);
    static Fl_RGB_Image icon_img(&icon_pixmap);
    win.icon(&icon_img);

    win.show(argc, argv);

    return Fl::run();
}
