
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Pixmap.H>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "protocol.h"
#include "transfer_queue.h"
#include "transfer_table.h"
#include "net_io.h"
#include "icon_xpm.h"
#include "settings.h"
#include "png_write.h"

using namespace piko_sync;

static const char *DEFAULT_ADDRESS = "10.208.47.2";

static std::string basename_of(const std::string &p)
{
    std::string::size_type s = p.rfind('/');
    return (s == std::string::npos) ? p : p.substr(s + 1);
}

static uint32_t compute_file_crc32(const std::string &path)
{
    Crc32 crc;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return 0;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        crc.update(buf, n);
    fclose(f);
    return crc.final_value();
}

static std::vector<std::string> list_network_interfaces()
{
    std::vector<std::string> names;
    struct ifaddrs *ifap = 0;
    if (getifaddrs(&ifap) != 0)
        return names;

    for (struct ifaddrs *p = ifap; p; p = p->ifa_next) {
        if (!p->ifa_name)
            continue;
        std::string n = p->ifa_name;
        bool seen = false;
        for (size_t i = 0; i < names.size(); i++) {
            if (names[i] == n) { seen = true; break; }
        }
        if (!seen)
            names.push_back(n);
    }
    freeifaddrs(ifap);
    return names;
}

static void select_choice_by_label(Fl_Choice *choice, const std::string &label)
{
    if (label.empty())
        return;
    for (int i = 0; i < choice->size(); i++) {
        const Fl_Menu_Item *item = choice->menu() + i;
        if (item->label() && item->active() && label == item->label()) {
            choice->value(i);
            return;
        }
    }
}

static std::string choice_label(const Fl_Choice *choice)
{
    const char *t = const_cast<Fl_Choice *>(choice)->text();
    return t ? std::string(t) : std::string();
}

class ClientApp;

struct QueuedFile {
    std::string path;
    std::string name;
    uint64_t total_size;
    uint32_t crc32;
    int row;
    double retry_delay;

    QueuedFile() : total_size(0), crc32(0), row(-1), retry_delay(1.0) {}
};

class FileSend {
public:
    FileSend(ClientApp *app, int file_index);
    ~FileSend();

    void abandon();

private:
    enum Phase {
        CONNECTING, WAIT_HELLO_ACK, WAIT_OFFER_ACK,
        WAIT_CHUNK_ACK, WAIT_COMPLETE_ACK, CLOSED
    };

    static void connect_cb(int, void *v) { static_cast<FileSend *>(v)->on_connectable(); }
    void on_connectable();
    static void read_cb(int, void *v) { static_cast<FileSend *>(v)->on_read(); }
    void on_read();
    void handle_frame(uint32_t type, const std::string &payload);

    void send_hello();
    void send_offer();
    void send_next_chunk();
    void send_complete();

    void terminate(TransferStatus status, const std::string &detail, bool retry);
    void close_fd_only();
    static void deferred_delete_cb(void *v) { delete static_cast<FileSend *>(v); }

    ClientApp *app_;
    int file_index_;
    int fd_;
    FrameReader reader_;
    Phase phase_;
    FILE *local_file_;
    uint64_t sent_offset_;
    uint64_t total_size_;
    char *chunk_buf_;
};

struct RetryContext {
    ClientApp *app;
    int file_index;
};

class BuildRunner;

class ClientApp {
public:
    ClientApp(Fl_Group *transfer_tab, int X, int Y, int W, int H, const Settings &cfg);
    ~ClientApp();

    void store_settings(Settings &cfg) const
    {
        cfg.set("transfer.address", address_->value() ? address_->value() : "");
        cfg.set("transfer.last_dir", last_dir_);
        cfg.set("transfer.screenshot_dir", shot_dir_);
    }

    TransferQueue &queue() { return queue_; }
    QueuedFile &file(int i) { return files_[i]; }
    std::string address() const { return address_->value() ? address_->value() : ""; }

    void sync_table()
    {
        table_->sync();
        double pct = queue_.aggregate_percent();
        aggregate_bar_->value(static_cast<float>(pct));
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%d%%", static_cast<int>(pct + 0.5));
        aggregate_bar_->copy_label(lbl);
        aggregate_bar_->redraw();
    }

    void spawn_attempt(int file_index) { active_.push_back(new FileSend(this, file_index)); }

    void forget_attempt(FileSend *fs)
    {
        for (size_t i = 0; i < active_.size(); i++) {
            if (active_[i] == fs) { active_.erase(active_.begin() + i); break; }
        }
    }

    void schedule_retry(int file_index)
    {
        QueuedFile &qf = files_[file_index];
        double delay = qf.retry_delay;
        qf.retry_delay = (qf.retry_delay < 10.0) ? qf.retry_delay * 2.0 : 10.0;
        RetryContext *ctx = new RetryContext();
        ctx->app = this;
        ctx->file_index = file_index;
        Fl::add_timeout(delay, retry_cb, ctx);
    }

private:
    static void retry_cb(void *v)
    {
        RetryContext *ctx = static_cast<RetryContext *>(v);
        ClientApp *app = ctx->app;
        int idx = ctx->file_index;
        delete ctx;
        app->spawn_attempt(idx);
    }

    static void add_files_cb(Fl_Widget *, void *v) { static_cast<ClientApp *>(v)->do_add_files(); }
    void do_add_files();

    static void screenshot_cb(Fl_Widget *, void *v) { static_cast<ClientApp *>(v)->do_screenshot(); }
    void do_screenshot();

    static void retry_failed_cb(Fl_Widget *, void *v) { static_cast<ClientApp *>(v)->do_retry_failed(); }
    void do_retry_failed();

    Fl_Input *address_;
    Fl_Progress *aggregate_bar_;
    TransferTable *table_;
    Fl_Button *shot_btn_;
    std::string shot_dir_;
    TransferQueue queue_;
    std::vector<QueuedFile> files_;
    std::vector<FileSend *> active_;
    std::string last_dir_;
};

FileSend::FileSend(ClientApp *app, int file_index)
    : app_(app), file_index_(file_index), fd_(-1), phase_(CONNECTING),
      local_file_(0), sent_offset_(0), total_size_(0)
{
    chunk_buf_ = new char[MAX_CHUNK];

    QueuedFile &qf = app_->file(file_index_);
    total_size_ = qf.total_size;
    app_->queue().set_status(qf.row, XFER_TRANSFERRING);
    app_->sync_table();

    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) { terminate(XFER_RECONNECTING, "", true); return; }
    set_nonblock(fd_);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEFAULT_PORT);

    std::string a = app_->address();
    if (a.empty() || inet_pton(AF_INET, a.c_str(), &addr.sin_addr) != 1) {
        terminate(XFER_ERROR, "invalid Zaurus address", false);
        return;
    }

    int rc = connect(fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    if (rc == 0) { on_connectable(); return; }
    if (errno != EINPROGRESS) { terminate(XFER_RECONNECTING, "", true); return; }

    Fl::add_fd(fd_, FL_WRITE, connect_cb, this);
}

FileSend::~FileSend()
{
    if (fd_ >= 0) { Fl::remove_fd(fd_); close(fd_); }
    if (local_file_) fclose(local_file_);
    delete[] chunk_buf_;
}

void FileSend::abandon()
{
    close_fd_only();
    phase_ = CLOSED;
}

void FileSend::close_fd_only()
{
    if (fd_ >= 0) { Fl::remove_fd(fd_); close(fd_); fd_ = -1; }
    if (local_file_) { fclose(local_file_); local_file_ = 0; }
}

void FileSend::terminate(TransferStatus status, const std::string &detail, bool retry)
{
    if (phase_ == CLOSED)
        return;
    phase_ = CLOSED;

    QueuedFile &qf = app_->file(file_index_);
    app_->queue().set_status(qf.row, status, detail);
    if (status == XFER_DONE)
        app_->queue().set_progress(qf.row, total_size_);
    app_->sync_table();

    close_fd_only();
    if (retry)
        app_->schedule_retry(file_index_);
    app_->forget_attempt(this);

    Fl::add_timeout(0.0, deferred_delete_cb, this);
}

void FileSend::on_connectable()
{
    Fl::remove_fd(fd_);

    int err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) { terminate(XFER_RECONNECTING, "", true); return; }

    Fl::add_fd(fd_, FL_READ, read_cb, this);
    send_hello();
}

void FileSend::send_hello()
{
    HelloMsg h; h.version = PROTO_VERSION;
    if (!send_frame_blocking(fd_, MSG_HELLO, encode(h))) { terminate(XFER_RECONNECTING, "", true); return; }
    phase_ = WAIT_HELLO_ACK;
}

void FileSend::send_offer()
{
    QueuedFile &qf = app_->file(file_index_);
    FileOfferMsg fo; fo.name = qf.name; fo.total_size = qf.total_size;
    if (!send_frame_blocking(fd_, MSG_FILE_OFFER, encode(fo))) { terminate(XFER_RECONNECTING, "", true); return; }
    phase_ = WAIT_OFFER_ACK;
}

void FileSend::send_next_chunk()
{
    size_t n = fread(chunk_buf_, 1, MAX_CHUNK, local_file_);
    if (n == 0) { terminate(XFER_ERROR, "local file changed size unexpectedly", false); return; }

    DataChunkMsg dc;
    dc.offset = sent_offset_;
    dc.data.assign(chunk_buf_, n);
    if (!send_frame_blocking(fd_, MSG_DATA_CHUNK, encode(dc))) { terminate(XFER_RECONNECTING, "", true); return; }
    phase_ = WAIT_CHUNK_ACK;
}

void FileSend::send_complete()
{
    QueuedFile &qf = app_->file(file_index_);
    FileCompleteMsg fc; fc.crc32 = qf.crc32;
    if (!send_frame_blocking(fd_, MSG_FILE_COMPLETE, encode(fc))) { terminate(XFER_RECONNECTING, "", true); return; }
    phase_ = WAIT_COMPLETE_ACK;
}

void FileSend::on_read()
{
    char buf[16384];
    ssize_t n = read(fd_, buf, sizeof(buf));

    if (n == 0) { terminate(XFER_RECONNECTING, "", true); return; }
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        terminate(XFER_RECONNECTING, "", true);
        return;
    }

    reader_.feed(buf, static_cast<size_t>(n));

    for (;;) {
        uint32_t type;
        std::string payload;
        FrameReader::Result r = reader_.next(type, payload);
        if (r == FrameReader::NEED_MORE) return;
        if (r == FrameReader::DESYNC) { terminate(XFER_RECONNECTING, "", true); return; }
        handle_frame(type, payload);
        if (phase_ == CLOSED) return;
    }
}

void FileSend::handle_frame(uint32_t type, const std::string &payload)
{
    if (type == MSG_ERROR) {
        ErrorMsg em;
        decode_error(payload, em);
        terminate(XFER_ERROR, em.message.empty() ? "server error" : em.message, false);
        return;
    }

    switch (phase_) {
    case WAIT_HELLO_ACK: {
        if (type != MSG_HELLO_ACK) { terminate(XFER_RECONNECTING, "", true); return; }
        HelloMsg h;
        if (!decode_hello(payload, h) || h.version != PROTO_VERSION) {
            terminate(XFER_ERROR, "server protocol version mismatch", false);
            return;
        }
        send_offer();
        return;
    }

    case WAIT_OFFER_ACK: {
        if (type != MSG_FILE_OFFER_ACK) { terminate(XFER_RECONNECTING, "", true); return; }
        FileOfferAckMsg ack;
        if (!decode_file_offer_ack(payload, ack)) { terminate(XFER_RECONNECTING, "", true); return; }
        if (!ack.accepted) { terminate(XFER_ERROR, ack.reason, false); return; }

        QueuedFile &qf = app_->file(file_index_);
        sent_offset_ = ack.resume_offset;
        app_->queue().set_progress(qf.row, sent_offset_);
        app_->sync_table();

        if (sent_offset_ >= total_size_) { send_complete(); return; }

        local_file_ = fopen(qf.path.c_str(), "rb");
        if (!local_file_ || fseek(local_file_, static_cast<long>(sent_offset_), SEEK_SET) != 0) {
            terminate(XFER_ERROR, "could not reopen local file", false);
            return;
        }
        send_next_chunk();
        return;
    }

    case WAIT_CHUNK_ACK: {
        if (type != MSG_CHUNK_ACK) { terminate(XFER_RECONNECTING, "", true); return; }
        ChunkAckMsg ack;
        if (!decode_chunk_ack(payload, ack)) { terminate(XFER_RECONNECTING, "", true); return; }

        sent_offset_ = ack.bytes_written;
        QueuedFile &qf = app_->file(file_index_);
        app_->queue().set_progress(qf.row, sent_offset_);
        app_->sync_table();

        if (sent_offset_ >= total_size_) { send_complete(); return; }
        send_next_chunk();
        return;
    }

    case WAIT_COMPLETE_ACK: {
        if (type != MSG_FILE_COMPLETE_ACK) { terminate(XFER_RECONNECTING, "", true); return; }
        FileCompleteAckMsg ack;
        if (!decode_file_complete_ack(payload, ack)) { terminate(XFER_RECONNECTING, "", true); return; }
        if (ack.ok) terminate(XFER_DONE, "", false);
        else        terminate(XFER_ERROR, ack.reason, false);
        return;
    }

    case CONNECTING:
    case CLOSED:
    default:
        return;
    }
}

// Screenshots are a short, self-contained request/response, so unlike the
// file queue (which is fully async, see FileSend) this runs inline with a
// socket timeout. ~600KB over the board's flaky WiFi is a second or two; the
// timeout is what keeps a dropped association from wedging the UI forever.
static const int SHOT_TIMEOUT_SECS = 20;

static bool shot_recv_frame(int fd, FrameReader &reader, uint32_t &type,
                            std::string &payload, std::string &err)
{
    for (;;) {
        FrameReader::Result r = reader.next(type, payload);
        if (r == FrameReader::GOT_FRAME)
            return true;
        if (r == FrameReader::DESYNC) { err = "protocol desync"; return false; }

        char buf[16384];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) { reader.feed(buf, static_cast<size_t>(n)); continue; }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            err = "timed out waiting for the device";
        else if (n == 0)
            err = "device closed the connection";
        else
            err = std::string("read: ") + strerror(errno);
        return false;
    }
}

// Returns the saved path on success.
static bool take_screenshot(const std::string &address, const std::string &dir,
                            std::string &saved_path, std::string &err)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err = std::string("socket: ") + strerror(errno); return false; }

    struct timeval tv;
    tv.tv_sec = SHOT_TIMEOUT_SECS;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEFAULT_PORT);
    if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        err = "not a valid IPv4 address: " + address;
        close(fd);
        return false;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
        err = std::string("cannot reach piko-sync-server on ") + address + ": " + strerror(errno);
        close(fd);
        return false;
    }

    FrameReader reader;
    uint32_t type;
    std::string payload;

    HelloMsg hello;
    hello.version = PROTO_VERSION;
    if (!send_frame_blocking(fd, MSG_HELLO, encode(hello))) {
        err = "sending HELLO failed"; close(fd); return false;
    }
    if (!shot_recv_frame(fd, reader, type, payload, err)) { close(fd); return false; }
    if (type != MSG_HELLO_ACK) {
        err = "device did not answer HELLO (is this an older piko-sync-server?)";
        close(fd); return false;
    }

    if (!send_frame_blocking(fd, MSG_SCREENSHOT, std::string())) {
        err = "sending SCREENSHOT failed"; close(fd); return false;
    }
    if (!shot_recv_frame(fd, reader, type, payload, err)) { close(fd); return false; }
    if (type == MSG_ERROR) {
        ErrorMsg em;
        err = decode_error(payload, em) ? em.message : "device reported an error";
        close(fd); return false;
    }
    if (type != MSG_SCREENSHOT_INFO) {
        err = "device does not support screenshots (update piko-sync-server)";
        close(fd); return false;
    }

    ScreenshotInfoMsg info;
    if (!decode_screenshot_info(payload, info)) {
        err = "malformed SCREENSHOT_INFO"; close(fd); return false;
    }
    if (!info.ok) { err = info.reason; close(fd); return false; }
    if (info.bpp != 16 || info.width == 0 || info.height == 0 ||
        info.byte_count != info.width * info.height * 2) {
        err = "device sent geometry this client cannot decode";
        close(fd); return false;
    }

    std::string pixels;
    pixels.reserve(info.byte_count);
    Crc32 crc;
    while (pixels.size() < info.byte_count) {
        if (!shot_recv_frame(fd, reader, type, payload, err)) { close(fd); return false; }
        if (type != MSG_DATA_CHUNK) {
            err = "device interrupted the screenshot stream"; close(fd); return false;
        }
        DataChunkMsg chunk;
        if (!decode_data_chunk(payload, chunk)) {
            err = "malformed DATA_CHUNK"; close(fd); return false;
        }
        if (chunk.offset != pixels.size()) {
            err = "device sent chunks out of order"; close(fd); return false;
        }
        crc.update(chunk.data.data(), chunk.data.size());
        pixels.append(chunk.data);
    }

    if (!shot_recv_frame(fd, reader, type, payload, err)) { close(fd); return false; }
    if (type != MSG_FILE_COMPLETE) {
        err = "device did not finish the screenshot"; close(fd); return false;
    }
    FileCompleteMsg done;
    if (!decode_file_complete(payload, done)) {
        err = "malformed FILE_COMPLETE"; close(fd); return false;
    }
    close(fd);

    if (done.crc32 != crc.final_value()) {
        err = "screenshot arrived corrupt (CRC mismatch)";
        return false;
    }

    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        err = "cannot create " + dir + ": " + strerror(errno);
        return false;
    }

    time_t now = time(0);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    std::string path = dir + "/piko-screenshot-" + stamp + ".png";

    std::string rgb = rgb565_to_rgb888(pixels, info.width, info.height);
    if (!png_write_rgb(path.c_str(), rgb, info.width, info.height, err))
        return false;

    saved_path = path;
    return true;
}

void ClientApp::do_screenshot()
{
    std::string address = address_->value() ? address_->value() : "";
    if (address.empty()) { fl_alert("Set the Zaurus address first."); return; }

    const char *home = getenv("HOME");
    std::string dir = shot_dir_.empty()
        ? (std::string(home ? home : ".") + "/Pictures/piko")
        : shot_dir_;

    shot_btn_->deactivate();
    shot_btn_->label("Grabbing...");
    Fl::check();

    std::string path, err;
    bool ok = take_screenshot(address, dir, path, err);

    shot_btn_->label("Screenshot");
    shot_btn_->activate();

    if (ok)
        fl_message("Saved %s", path.c_str());
    else
        fl_alert("Screenshot failed:\n%s", err.c_str());
}

ClientApp::ClientApp(Fl_Group *tab, int X, int Y, int W, int H, const Settings &cfg)
{
    (void)tab;
    int m = 10;

    std::string saved_dir = cfg.get("transfer.last_dir");
    if (!saved_dir.empty()) {
        struct stat st;
        if (stat(saved_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            last_dir_ = saved_dir;
    }

    address_ = new Fl_Input(X + m + 90, Y + m, 160, 24, "Zaurus:");
    address_->align(FL_ALIGN_LEFT);
    address_->value(cfg.get("transfer.address", DEFAULT_ADDRESS).c_str());

    Fl_Button *add_btn = new Fl_Button(X + m + 260, Y + m, 100, 24, "Add Files...");
    add_btn->callback(add_files_cb, this);

    Fl_Button *retry_btn = new Fl_Button(X + m + 366, Y + m, 110, 24, "Retry failed");
    retry_btn->callback(retry_failed_cb, this);

    shot_btn_ = new Fl_Button(X + m + 482, Y + m, 110, 24, "Screenshot");
    shot_btn_->callback(screenshot_cb, this);
    shot_btn_->tooltip("Grab the device's screen and save it as a PNG on this machine");
    shot_dir_ = cfg.get("transfer.screenshot_dir");

    aggregate_bar_ = new Fl_Progress(X + m, Y + m + 32, W - 2 * m, 20);
    aggregate_bar_->minimum(0);
    aggregate_bar_->maximum(100);
    aggregate_bar_->value(0);
    aggregate_bar_->color(FL_BACKGROUND_COLOR);
    aggregate_bar_->selection_color(FL_BLUE);
    aggregate_bar_->label("0%");

    table_ = new TransferTable(X + m, Y + m + 60, W - 2 * m, H - m - 60 - m);
    table_->queue(&queue_);
}

ClientApp::~ClientApp()
{
    for (size_t i = 0; i < active_.size(); i++) {
        active_[i]->abandon();
        delete active_[i];
    }
    active_.clear();
}

void ClientApp::do_add_files()
{
    Fl_File_Chooser chooser(last_dir_.empty() ? "." : last_dir_.c_str(), "*",
                             Fl_File_Chooser::MULTI, "Add files to send");
    chooser.show();
    while (chooser.shown())
        Fl::wait();

    if (!chooser.value(1))
        return;

    for (int i = 1; i <= chooser.count(); i++) {
        const char *path = chooser.value(i);
        if (!path)
            continue;

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        QueuedFile qf;
        qf.path = path;
        qf.name = basename_of(path);
        qf.total_size = static_cast<uint64_t>(st.st_size);
        qf.crc32 = compute_file_crc32(path);
        qf.row = queue_.add(qf.name, qf.total_size);

        files_.push_back(qf);
        spawn_attempt(static_cast<int>(files_.size()) - 1);

        std::string::size_type slash = qf.path.rfind('/');
        if (slash != std::string::npos)
            last_dir_ = qf.path.substr(0, slash);
    }
    sync_table();
}

void ClientApp::do_retry_failed()
{
    for (size_t i = 0; i < files_.size(); i++) {
        if (queue_.row(files_[i].row).status == XFER_ERROR) {
            files_[i].retry_delay = 1.0;
            spawn_attempt(static_cast<int>(i));
        }
    }
}

struct Milestone { const char *substr; int percent; const char *phase; };

static const Milestone MILESTONES[] = {
    { "checking ",                              5, "Checking..."  },
    { "reconstructing kernel-src",              10, "Building..." },
    { "building zImage",                        20, "Building..." },
    { "build OK",                                45, "Building..." },
    { "building userspace",                      55, "Building..." },
    { "userspace build OK",                      70, "Building..." },
    { "building the X11/Matchbox stack",         80, "Building..." },
    { "repacking the X11/Matchbox payload",      90, "Building..." },
    { "X11 payload OK",                          95, "Building..." },
    { "deploying to",                            98, "Deploying..." },
};
static const int MILESTONE_COUNT = sizeof(MILESTONES) / sizeof(MILESTONES[0]);

class SettingsStore;

class BuildRunner {
public:
    BuildRunner(Fl_Group *tab, int X, int Y, int W, int H, const Settings &cfg,
                SettingsStore *store);

    void store_settings(Settings &cfg) const;

private:
    void apply_settings(const Settings &cfg);

    static void run_cb(Fl_Widget *, void *v) { static_cast<BuildRunner *>(v)->do_run(); }
    void do_run();

    static void out_ready(int fd, void *v) { static_cast<BuildRunner *>(v)->on_out(fd); }
    void on_out(int fd);
    void handle_line(const std::string &line);
    void maybe_finish();
    void finish(int rc);

    void append(const char *text);
    static void set_nonblock_local(int fd);

    std::string script_path() const;
    void build_argv(std::vector<std::string> &args) const;

    static void settings_cb(Fl_Widget *, void *v) { static_cast<BuildRunner *>(v)->do_settings(); }
    void do_settings();

    Fl_Button *settings_btn_;
    std::string repo_root_;
    std::string toolchain_bin_dir_;
    std::string jobs_;

    Fl_Choice *adapter_;
    Fl_Input *target_;
    Fl_Check_Button *kernel_only_;
    Fl_Check_Button *force_kernel_src_;
    Fl_Check_Button *skip_userspace_;
    Fl_Check_Button *skip_st_;
    Fl_Check_Button *skip_x11_;
    Fl_Check_Button *build_only_;
    Fl_Check_Button *no_backup_;
    Fl_Choice *destination_;
    Fl_Button *run_btn_;
    Fl_Box *status_label_;
    Fl_Progress *bar_;
    Fl_Text_Buffer *log_buf_;
    Fl_Text_Display *log_;

    pid_t pid_;
    int out_fd_;
    std::string pending_line_;
    int milestone_idx_;
    bool running_;
    SettingsStore *store_;
};

class SettingsStore {
public:
    SettingsStore() : client_(0), runner_(0) { cfg_.load(); }

    const Settings &cfg() const { return cfg_; }

    void bind(ClientApp *client, BuildRunner *runner)
    {
        client_ = client;
        runner_ = runner;
    }

    void save_now();

private:
    Settings cfg_;
    ClientApp *client_;
    BuildRunner *runner_;
};

BuildRunner::BuildRunner(Fl_Group *tab, int X, int Y, int W, int H,
                         const Settings &cfg, SettingsStore *store)
    : pid_(-1), out_fd_(-1), milestone_idx_(0), running_(false), store_(store)
{
    (void)tab;

    int m = 10, y = Y + m;

    adapter_ = new Fl_Choice(X + m + 70, y, 130, 22, "Adapter:");
    adapter_->align(FL_ALIGN_LEFT);
    {
        std::vector<std::string> ifaces = list_network_interfaces();
        for (size_t i = 0; i < ifaces.size(); i++)
            adapter_->add(ifaces[i].c_str());
        if (!ifaces.empty())
            adapter_->value(0);
    }
    target_ = new Fl_Input(X + m + 260, y, 160, 22, "Target:");
    target_->align(FL_ALIGN_LEFT);
    std::string default_target = std::string("root@") + DEFAULT_ADDRESS;
    target_->value(default_target.c_str());
    y += 28;

    kernel_only_ = new Fl_Check_Button(X + m, y, 120, 20, "kernel-only");
    force_kernel_src_ = new Fl_Check_Button(X + m + 120, y, 140, 20, "force-kernel-src");
    skip_userspace_ = new Fl_Check_Button(X + m + 260, y, 130, 20, "skip-userspace");
    y += 22;
    skip_st_ = new Fl_Check_Button(X + m, y, 90, 20, "skip-st");
    skip_x11_ = new Fl_Check_Button(X + m + 120, y, 90, 20, "skip-x11");
    build_only_ = new Fl_Check_Button(X + m + 260, y, 100, 20, "build-only");
    y += 22;
    no_backup_ = new Fl_Check_Button(X + m, y, 110, 20, "no-backup");
    no_backup_->value(1);
    destination_ = new Fl_Choice(X + m + 190, y - 1, 90, 22, "Staging:");
    destination_->align(FL_ALIGN_LEFT);
    destination_->add("sd");
    destination_->add("nand");
    destination_->add("cf", 0, 0, 0, FL_MENU_INACTIVE);
    destination_->value(0);
    y += 26;

    run_btn_ = new Fl_Button(X + m, y, 140, 26, "Build && Deploy");
    run_btn_->callback(run_cb, this);
    settings_btn_ = new Fl_Button(X + m + 150, y, 100, 26, "Settings...");
    settings_btn_->callback(settings_cb, this);
    y += 34;

    status_label_ = new Fl_Box(X + m, y, W - 2 * m, 16);
    status_label_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    status_label_->labelfont(FL_HELVETICA_ITALIC);
    status_label_->labelsize(12);
    status_label_->label("");
    y += 18;

    bar_ = new Fl_Progress(X + m, y, W - 2 * m, 20);
    bar_->minimum(0);
    bar_->maximum(100);
    bar_->value(0);
    bar_->color(FL_BACKGROUND_COLOR);
    bar_->selection_color(FL_BLUE);
    bar_->label("idle");
    y += 28;

    log_buf_ = new Fl_Text_Buffer();
    log_ = new Fl_Text_Display(X + m, y, W - 2 * m, (Y + H - m) - y);
    log_->buffer(log_buf_);
    log_->textfont(FL_COURIER);
    log_->textsize(11);
    log_->cursor_style(Fl_Text_Display::SIMPLE_CURSOR);

    apply_settings(cfg);
}

void BuildRunner::apply_settings(const Settings &cfg)
{
    repo_root_ = cfg.get("build.repo_root");
    toolchain_bin_dir_ = cfg.get("build.toolchain_bin_dir");
    jobs_ = cfg.get("build.jobs");

    if (const char *env_root = getenv("PIKO_SYNC_REPO_ROOT"))
        if (*env_root) repo_root_ = env_root;

    std::string target = cfg.get("build.target");
    if (!target.empty())
        target_->value(target.c_str());

    select_choice_by_label(adapter_, cfg.get("build.adapter"));
    select_choice_by_label(destination_, cfg.get("build.staging"));

    kernel_only_->value(cfg.get_bool("build.kernel_only", false) ? 1 : 0);
    force_kernel_src_->value(cfg.get_bool("build.force_kernel_src", false) ? 1 : 0);
    skip_userspace_->value(cfg.get_bool("build.skip_userspace", false) ? 1 : 0);
    skip_st_->value(cfg.get_bool("build.skip_st", false) ? 1 : 0);
    skip_x11_->value(cfg.get_bool("build.skip_x11", false) ? 1 : 0);
    build_only_->value(cfg.get_bool("build.build_only", false) ? 1 : 0);
    no_backup_->value(cfg.get_bool("build.no_backup", true) ? 1 : 0);
}

void BuildRunner::store_settings(Settings &cfg) const
{
    const char *env_root = getenv("PIKO_SYNC_REPO_ROOT");
    if (!env_root || !*env_root)
        cfg.set("build.repo_root", repo_root_);

    cfg.set("build.toolchain_bin_dir", toolchain_bin_dir_);
    cfg.set("build.jobs", jobs_);
    cfg.set("build.target", target_->value() ? target_->value() : "");
    cfg.set("build.adapter", choice_label(adapter_));
    cfg.set("build.staging", choice_label(destination_));

    cfg.set_bool("build.kernel_only", kernel_only_->value() != 0);
    cfg.set_bool("build.force_kernel_src", force_kernel_src_->value() != 0);
    cfg.set_bool("build.skip_userspace", skip_userspace_->value() != 0);
    cfg.set_bool("build.skip_st", skip_st_->value() != 0);
    cfg.set_bool("build.skip_x11", skip_x11_->value() != 0);
    cfg.set_bool("build.build_only", build_only_->value() != 0);
    cfg.set_bool("build.no_backup", no_backup_->value() != 0);
}

void SettingsStore::save_now()
{
    if (client_)
        client_->store_settings(cfg_);
    if (runner_)
        runner_->store_settings(cfg_);
    cfg_.save();
}

std::string BuildRunner::script_path() const
{
    std::string base = repo_root_.empty() ? "." : repo_root_;
    return base + "/tools/build-and-deploy.sh";
}

void BuildRunner::build_argv(std::vector<std::string> &args) const
{
    args.push_back(script_path());
    if (adapter_->text() && adapter_->text()[0]) {
        args.push_back("--adapter");
        args.push_back(adapter_->text());
    }
    if (force_kernel_src_->value()) args.push_back("--force-kernel-src");
    if (kernel_only_->value())      args.push_back("--kernel-only");
    if (skip_userspace_->value())   args.push_back("--skip-userspace");
    if (skip_st_->value())          args.push_back("--skip-st");
    if (skip_x11_->value())         args.push_back("--skip-x11");
    if (build_only_->value())       args.push_back("--build-only");
    if (!no_backup_->value())       args.push_back("--create-backup-files");
    if (destination_->text() && destination_->text()[0]) {
        args.push_back("--staging");
        args.push_back(destination_->text());
    }
    if (target_->value() && target_->value()[0]) {
        std::string t = target_->value();
        if (t.find('@') == std::string::npos)
            t = "root@" + t;
        args.push_back(t);
    }
}

namespace {

void browse_toolchain_cb(Fl_Widget *, void *v)
{
    Fl_Input *input = static_cast<Fl_Input *>(v);
    const char *start = (input->value() && input->value()[0]) ? input->value() : ".";

    Fl_File_Chooser chooser(start, "*", Fl_File_Chooser::DIRECTORY,
                             "Choose the toolchain bin directory");
    chooser.show();
    while (chooser.shown())
        Fl::wait();
    if (!chooser.value(1))
        return;

    std::string picked = chooser.value(1);
    std::string probe = picked + "/arm-unknown-linux-uclibcgnueabi-gcc";
    struct stat st;
    if (stat(probe.c_str(), &st) != 0) {
        fl_alert("Note: arm-unknown-linux-uclibcgnueabi-gcc was not found directly in:\n%s\n\n"
                 "build-and-deploy.sh may not find the cross compiler there.",
                 picked.c_str());
    }
    input->value(picked.c_str());
}

void browse_repo_cb(Fl_Widget *, void *v)
{
    Fl_Input *input = static_cast<Fl_Input *>(v);
    const char *start = (input->value() && input->value()[0]) ? input->value() : ".";

    Fl_File_Chooser chooser(start, "*", Fl_File_Chooser::DIRECTORY,
                             "Choose the piko repository root");
    chooser.show();
    while (chooser.shown())
        Fl::wait();
    if (!chooser.value(1))
        return;

    std::string picked = chooser.value(1);
    std::string probe = picked + "/tools/build-and-deploy.sh";
    struct stat st;
    if (stat(probe.c_str(), &st) != 0) {
        fl_alert("Note: tools/build-and-deploy.sh was not found under:\n%s\n\n"
                 "Build && Deploy will not work until this points at a piko checkout.",
                 picked.c_str());
    }
    input->value(picked.c_str());
}

struct OkCancelCtx {
    Fl_Double_Window *dlg;
    bool *ok;
};

void settings_ok_cb(Fl_Widget *, void *v)
{
    OkCancelCtx *ctx = static_cast<OkCancelCtx *>(v);
    *ctx->ok = true;
    ctx->dlg->hide();
}

void settings_cancel_cb(Fl_Widget *, void *v)
{
    static_cast<OkCancelCtx *>(v)->dlg->hide();
}

}

void BuildRunner::do_settings()
{
    Fl_Double_Window dlg(480, 290, "Build settings");
    dlg.begin();

    Fl_Input repo_input(90, 15, 300, 24, "Repo:");
    repo_input.align(FL_ALIGN_LEFT);
    repo_input.value(repo_root_.c_str());

    Fl_Button repo_browse_btn(400, 15, 70, 24, "Browse...");
    repo_browse_btn.callback(browse_repo_cb, &repo_input);

    Fl_Box repo_hint(10, 45, 460, 40,
                "The piko checkout containing tools/build-and-deploy.sh\n"
                "(PIKO_SYNC_REPO_ROOT). Leave blank to use the directory this\n"
                "app was started from.");
    repo_hint.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    repo_hint.labelsize(11);

    Fl_Input toolchain_input(90, 95, 300, 24, "Toolchain:");
    toolchain_input.align(FL_ALIGN_LEFT);
    toolchain_input.value(toolchain_bin_dir_.c_str());

    Fl_Button browse_btn(400, 95, 70, 24, "Browse...");
    browse_btn.callback(browse_toolchain_cb, &toolchain_input);

    Fl_Box toolchain_hint(10, 125, 460, 40,
                "Directory containing the arm-*-gcc cross compiler\n"
                "(TOOLCHAIN_BIN_DIR). Leave blank to use build-and-deploy.sh's\n"
                "own default (<repo>/toolchain/x-tools/.../bin).");
    toolchain_hint.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    toolchain_hint.labelsize(11);

    Fl_Input jobs_input(90, 175, 60, 24, "Jobs:");
    jobs_input.align(FL_ALIGN_LEFT);
    jobs_input.value(jobs_.c_str());

    Fl_Box jobs_hint(10, 205, 460, 40,
                "make -jN for the kernel build, and forwarded to\n"
                "tools/userspace/build-userspace.sh's own JOBS. Leave blank to use\n"
                "nproc (all detected cores).");
    jobs_hint.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    jobs_hint.labelsize(11);

    bool ok = false;
    OkCancelCtx ctx;
    ctx.dlg = &dlg;
    ctx.ok = &ok;

    Fl_Button ok_btn(290, 250, 80, 26, "OK");
    ok_btn.callback(settings_ok_cb, &ctx);
    Fl_Button cancel_btn(380, 250, 80, 26, "Cancel");
    cancel_btn.callback(settings_cancel_cb, &ctx);

    dlg.end();
    dlg.set_modal();
    dlg.show();
    while (dlg.shown())
        Fl::wait();

    if (!ok)
        return;

    repo_root_ = repo_input.value() ? repo_input.value() : "";
    toolchain_bin_dir_ = toolchain_input.value() ? toolchain_input.value() : "";

    std::string jobs_text = jobs_input.value() ? jobs_input.value() : "";
    if (!jobs_text.empty()) {
        char *end = 0;
        long n = strtol(jobs_text.c_str(), &end, 10);
        if (n <= 0 || !end || *end != '\0')
            fl_alert("Note: \"%s\" doesn't look like a positive number.\n"
                      "make -j will be passed this value as-is.", jobs_text.c_str());
    }
    jobs_ = jobs_text;

    if (store_)
        store_->save_now();
}

void BuildRunner::append(const char *text)
{
    log_buf_->append(text);
    log_->insert_position(log_buf_->length());
    log_->show_insert_position();
    log_->redraw();
}

void BuildRunner::set_nonblock_local(int fd) { set_nonblock(fd); }

void BuildRunner::do_run()
{
    if (running_) {
        fl_alert("A build is already running.");
        return;
    }

    std::vector<std::string> args;
    build_argv(args);

    struct stat st;
    if (stat(args[0].c_str(), &st) != 0) {
        fl_alert("Cannot find %s.\n\n"
                 "Set the piko checkout in Settings... -> Repo.", args[0].c_str());
        return;
    }

    int outp[2];
    if (pipe(outp) != 0) { fl_alert("Could not create pipe."); return; }

    pid_ = fork();
    if (pid_ < 0) {
        fl_alert("fork() failed.");
        close(outp[0]); close(outp[1]);
        return;
    }

    if (pid_ == 0) {
        close(outp[0]);
        dup2(outp[1], STDOUT_FILENO);
        dup2(outp[1], STDERR_FILENO);
        close(outp[1]);

        if (!repo_root_.empty()) {
            setenv("PIKO_SYNC_REPO_ROOT", repo_root_.c_str(), 1);
            if (chdir(repo_root_.c_str()) != 0) {
                fprintf(stderr, "piko-sync-client: cannot enter %s: %s\n",
                        repo_root_.c_str(), strerror(errno));
                _exit(127);
            }
        }
        if (!toolchain_bin_dir_.empty())
            setenv("TOOLCHAIN_BIN_DIR", toolchain_bin_dir_.c_str(), 1);
        if (!jobs_.empty())
            setenv("JOBS", jobs_.c_str(), 1);

        std::vector<char *> argv;
        for (size_t i = 0; i < args.size(); i++)
            argv.push_back(const_cast<char *>(args[i].c_str()));
        argv.push_back(0);

        execv(argv[0], &argv[0]);
        fprintf(stderr, "piko-sync-client: cannot run %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(outp[1]);
    out_fd_ = outp[0];
    set_nonblock_local(out_fd_);
    Fl::add_fd(out_fd_, FL_READ, out_ready, this);

    running_ = true;
    milestone_idx_ = 0;
    pending_line_.clear();
    log_buf_->text("");
    bar_->value(0);
    bar_->selection_color(FL_BLUE);
    bar_->copy_label("running...");
    status_label_->label("Building...");
    status_label_->redraw();
    run_btn_->deactivate();

    if (store_)
        store_->save_now();
}

void BuildRunner::on_out(int fd)
{
    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);

    if (n > 0) {
        buf[n] = '\0';
        append(buf);
        pending_line_.append(buf, static_cast<size_t>(n));
        std::string::size_type nl;
        while ((nl = pending_line_.find('\n')) != std::string::npos) {
            handle_line(pending_line_.substr(0, nl));
            pending_line_.erase(0, nl + 1);
        }
        return;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;

    Fl::remove_fd(fd);
    close(fd);
    out_fd_ = -1;
    maybe_finish();
}

void BuildRunner::handle_line(const std::string &line)
{
    for (int i = milestone_idx_; i < MILESTONE_COUNT; i++) {
        if (line.find(MILESTONES[i].substr) != std::string::npos) {
            milestone_idx_ = i + 1;
            bar_->value(static_cast<float>(MILESTONES[i].percent));
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "%d%%", MILESTONES[i].percent);
            bar_->copy_label(lbl);
            bar_->redraw();
            status_label_->label(MILESTONES[i].phase);
            status_label_->redraw();
            break;
        }
    }
}

void BuildRunner::maybe_finish()
{
    if (out_fd_ >= 0)
        return;

    int status = 0, rc = -1;
    if (pid_ > 0) {
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        pid_ = -1;
    }
    finish(rc);
}

void BuildRunner::finish(int rc)
{
    running_ = false;
    run_btn_->activate();

    char lbl[64];
    if (rc == 0) {
        bar_->value(100);
        bar_->selection_color(FL_DARK_GREEN);
        snprintf(lbl, sizeof(lbl), "done");
        status_label_->label("Done");
        append("\n--- build-and-deploy.sh finished successfully ---\n");
    } else {
        bar_->selection_color(FL_RED);
        snprintf(lbl, sizeof(lbl), "FAILED (exit %d)", rc);
        status_label_->label("Failed");
        append("\n--- build-and-deploy.sh FAILED. See the log above. ---\n");
    }
    bar_->copy_label(lbl);
    bar_->redraw();
    status_label_->redraw();
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    SettingsStore settings;

    Fl_Double_Window win(720, 520, "Piko Sync");
    win.begin();

    Fl_Tabs tabs(0, 0, 720, 520);
    tabs.begin();

    Fl_Group transfer_tab(0, 24, 720, 496, "Transfer");
    ClientApp client(&transfer_tab, 0, 24, 720, 496, settings.cfg());
    transfer_tab.end();

    Fl_Group deploy_tab(0, 24, 720, 496, "Build && Deploy");
    BuildRunner runner(&deploy_tab, 0, 24, 720, 496, settings.cfg(), &settings);
    deploy_tab.end();

    settings.bind(&client, &runner);

    tabs.end();
    tabs.resizable(transfer_tab);

    win.end();
    win.resizable(tabs);

    static Fl_Pixmap icon_pixmap(piko_sync_icon_xpm);
    static Fl_RGB_Image icon_img(&icon_pixmap);
    win.icon(&icon_img);

    win.show(argc, argv);

    int rc = Fl::run();

    settings.save_now();
    return rc;
}
