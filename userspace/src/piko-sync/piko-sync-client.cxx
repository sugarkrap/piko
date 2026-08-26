
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Pixmap.H>

#include <arpa/inet.h>
#include <dirent.h>
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
#include <map>

#include "rom_detect.h"
#include "emulation_db.h"
#include "bezel_db.h"
#include "bezel_format.h"
#include "bezel_bake.h"
#include "bezel_store.h"
#include "jar_meta.h"

using namespace piko_sync;

static const char *DEFAULT_ADDRESS = "10.208.47.2";
static const char *DEFAULT_DEST_DIR = "/mnt/card/Transfers";
static const char *ROM_DEST_DIR = "/mnt/card/Emulation";
static const char *APPLET_DEST_DIR = "/mnt/card/Applets";
static const int ROM_PANEL_H = 196;
static const int ROM_CONNECT_SECS = 5;
static int rom_columns_[] = { 190, 60, 90, 90, 0 };

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

class TransferPane;

class TransferObserver {
public:
    virtual ~TransferObserver() {}
    virtual void on_transfer_complete() = 0;
};

struct QueuedFile {
    std::string path;
    std::string name;
    std::string machine;
    std::string options;
    std::string dest;
    uint64_t total_size;
    uint32_t crc32;
    int row;
    double retry_delay;

    QueuedFile() : total_size(0), crc32(0), row(-1), retry_delay(1.0) {}
};

class FileSend {
public:
    FileSend(TransferPane *app, int file_index);
    ~FileSend();

    void abandon();
    void cancel();

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

    TransferPane *app_;
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
    TransferPane *app;
    int file_index;
};

class BuildRunner;

static Fl_Box *g_busy_label = 0;

class BusyCursor {
public:
    explicit BusyCursor(const char *msg)
    {
        if (g_busy_label) {
            g_busy_label->copy_label(msg);
            g_busy_label->redraw();
        }
        Fl::flush();
    }
    ~BusyCursor()
    {
        if (g_busy_label) {
            g_busy_label->label("");
            g_busy_label->redraw();
        }
        Fl::flush();
    }
};

class TransferPane {
public:
    TransferPane(int hx, int hy, int hw, int hh,
                 int dx, int dy, int dw, int dh,
                 const Settings &cfg);
    virtual ~TransferPane();

    bool queue_path(const std::string &path, const std::string &options = std::string(),
                    const std::string &dest = std::string(),
                    const std::string &machine = std::string());
    void choose_and_queue(const char *pattern, const char *title);
    void retry_failed() { do_retry_failed(); }
    void refresh_queue_view() { sync_table(); }

    virtual void store_settings(Settings &cfg) const
    {
        cfg.set("transfer.address", address_->value() ? address_->value() : "");
        cfg.set("transfer.last_dir", last_dir_);
    }

    void add_observer(TransferObserver *o) { observers_.push_back(o); }
    void notify_transfer_complete()
    {
        for (size_t i = 0; i < observers_.size(); i++)
            observers_[i]->on_transfer_complete();
    }

    TransferQueue &queue() { return queue_; }
    QueuedFile &file(int i) { return files_[i]; }
    std::string address() const { return address_->value() ? address_->value() : ""; }
    std::string dest_dir() const { return default_dest_; }
    void set_default_dest(const std::string &d) { default_dest_ = d; }

    bool expanded() const { return expanded_; }
    void set_expanded(bool on)
    {
        expanded_ = on;
        if (expanded_)
            table_->show();
        else
            table_->hide();
        if (toggle_btn_)
            toggle_btn_->label(expanded_ ? "@2>  Transfers" : "@>  Transfers");
        if (relayout_cb_)
            relayout_cb_(relayout_arg_);
    }
    void on_relayout(void (*fn)(void *), void *arg) { relayout_cb_ = fn; relayout_arg_ = arg; }
    int table_bottom() const { return table_->y() + table_->h(); }
    void dock_relayout(int dx, int dy, int dw, int dh)
    {
        int m = 10;
        toggle_btn_->resize(dx + m, dy, 120, 22);
        aggregate_bar_->resize(dx + m + 126, dy + 1, dw - 2 * m - 126 - 106, 20);
        shot_btn_->resize(dx + dw - m - 100, dy, 100, 22);
        int th = dh - 26;
        table_->resize(dx + m, dy + 26, dw - 2 * m, th > 1 ? th : 1);
    }
    const std::string &last_dir() const { return last_dir_; }

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

    void spawn_attempt(int file_index)
    {
        if (cancelled_)
            return;
        if (active_.size() >= max_active_) {
            pending_.push_back(file_index);
            return;
        }
        active_.push_back(new FileSend(this, file_index));
    }

    void forget_attempt(FileSend *fs)
    {
        for (size_t i = 0; i < active_.size(); i++) {
            if (active_[i] == fs) { active_.erase(active_.begin() + i); break; }
        }
        while (!cancelled_ && !pending_.empty() && active_.size() < max_active_) {
            int idx = pending_.front();
            pending_.erase(pending_.begin());
            active_.push_back(new FileSend(this, idx));
        }
    }

    bool idle() const
    {
        return active_.empty() && pending_.empty();
    }

    void cancel_all()
    {
        cancelled_ = true;

        for (size_t i = 0; i < pending_.size(); i++) {
            QueuedFile &qf = files_[pending_[i]];
            if (qf.row >= 0)
                queue_.set_status(qf.row, XFER_ERROR, "cancelled");
        }
        pending_.clear();

        std::vector<FileSend *> doomed = active_;
        for (size_t i = 0; i < doomed.size(); i++)
            doomed[i]->cancel();

        cancelled_ = false;
        sync_table();
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

protected:
    int reserve_y() const { return reserve_y_; }
    std::vector<TransferObserver *> observers_;

private:
    static void retry_cb(void *v)
    {
        RetryContext *ctx = static_cast<RetryContext *>(v);
        TransferPane *app = ctx->app;
        int idx = ctx->file_index;
        delete ctx;
        app->spawn_attempt(idx);
    }

    static void add_files_cb(Fl_Widget *, void *v) { static_cast<TransferPane *>(v)->do_add_files(); }
    void do_add_files();

    static void screenshot_cb(Fl_Widget *, void *v) { static_cast<TransferPane *>(v)->do_screenshot(); }
    static void toggle_cb(Fl_Widget *, void *v)
    {
        TransferPane *p = static_cast<TransferPane *>(v);
        p->set_expanded(!p->expanded());
    }
    void do_screenshot();

    static void retry_failed_cb(Fl_Widget *, void *v) { static_cast<TransferPane *>(v)->do_retry_failed(); }
    static void cancel_cb(Fl_Widget *, void *v) { static_cast<TransferPane *>(v)->cancel_all(); }
    void do_retry_failed();

    Fl_Input *address_;
    std::string default_dest_;
    Fl_Button *toggle_btn_;
    bool expanded_;
    void (*relayout_cb_)(void *);
    void *relayout_arg_;
    Fl_Progress *aggregate_bar_;
    Fl_Box *busy_label_;
    TransferTable *table_;
    Fl_Button *shot_btn_;
    Fl_Button *cancel_btn_;
    int reserve_y_;
    TransferQueue queue_;
    std::vector<QueuedFile> files_;
    std::vector<FileSend *> active_;
    std::vector<int> pending_;
    size_t max_active_;
    bool cancelled_;
    std::string last_dir_;
};

FileSend::FileSend(TransferPane *app, int file_index)
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

void FileSend::cancel()
{
    terminate(XFER_ERROR, "cancelled", false);
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

    if (status == XFER_DONE)
        app_->notify_transfer_complete();

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
    fo.dest_dir = qf.dest;
    fo.rom_machine = qf.machine;
    fo.rom_options = qf.options;
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

static const int SHOT_TIMEOUT_SECS = 20;
static const int BEZEL_FETCH_TIMEOUT_SECS = 180;

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

static const char *find_clipboard_cmd()
{
    struct Candidate { const char *bin; const char *cmd; };
    static const Candidate cands[] = {
        { "wl-copy", "wl-copy --type image/png" },
        { "xclip",   "xclip -selection clipboard -t image/png -i" },
        { 0, 0 }
    };
    for (int i = 0; cands[i].bin; i++) {
        std::string probe = std::string("command -v ") + cands[i].bin + " >/dev/null 2>&1";
        if (system(probe.c_str()) == 0)
            return cands[i].cmd;
    }
    return 0;
}

static bool copy_png_to_clipboard(const std::string &png, std::string &err)
{
    const char *cmd = find_clipboard_cmd();
    if (!cmd) {
        err = "no clipboard tool found -- install wl-clipboard (wl-copy) or xclip";
        return false;
    }

    void (*old_pipe)(int) = signal(SIGPIPE, SIG_IGN);
    FILE *p = popen(cmd, "w");
    if (!p) {
        signal(SIGPIPE, old_pipe);
        err = std::string("cannot run: ") + cmd;
        return false;
    }
    size_t wrote = fwrite(png.data(), 1, png.size(), p);
    int rc = pclose(p);
    signal(SIGPIPE, old_pipe);

    if (wrote != png.size()) {
        err = std::string("clipboard tool closed early: ") + cmd;
        return false;
    }
    if (rc != 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s exited with status %d", cmd, rc);
        err = buf;
        return false;
    }
    return true;
}

static bool take_screenshot(const std::string &address, std::string &err)
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

    std::string rgb = rgb565_to_rgb888(pixels, info.width, info.height);
    std::string png;
    if (!png_encode_rgb(rgb, info.width, info.height, png, err))
        return false;

    return copy_png_to_clipboard(png, err);
}

void TransferPane::do_screenshot()
{
    std::string address = address_->value() ? address_->value() : "";
    if (address.empty()) { fl_alert("Set the Zaurus address first."); return; }

    shot_btn_->deactivate();
    shot_btn_->label("Grabbing...");
    Fl::check();

    std::string err;
    bool ok = take_screenshot(address, err);

    shot_btn_->label("Screenshot");
    shot_btn_->activate();

    if (ok)
        fl_message("Screenshot copied to the clipboard.");
    else
        fl_alert("Screenshot failed:\n%s", err.c_str());
}

TransferPane::TransferPane(int hx, int hy, int hw, int hh,
                           int dx, int dy, int dw, int dh,
                           const Settings &cfg)
{
    int m = 10;
    expanded_ = false;
    max_active_ = 2;
    cancelled_ = false;
    toggle_btn_ = 0;
    relayout_cb_ = 0;
    relayout_arg_ = 0;
    reserve_y_ = 0;

    std::string saved_dir = cfg.get("transfer.last_dir");
    if (!saved_dir.empty()) {
        struct stat st;
        if (stat(saved_dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            last_dir_ = saved_dir;
    }
    default_dest_ = cfg.get("transfer.dest_dir", DEFAULT_DEST_DIR);

    address_ = new Fl_Input(hx + m + 60, hy + (hh - 24) / 2, 170, 24, "Zaurus:");
    address_->align(FL_ALIGN_LEFT);
    address_->value(cfg.get("transfer.address", DEFAULT_ADDRESS).c_str());
    address_->tooltip("Address of the device running piko-sync-server");

    toggle_btn_ = new Fl_Button(dx + m, dy, 120, 22, "@>  Transfers");
    toggle_btn_->box(FL_FLAT_BOX);
    toggle_btn_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    toggle_btn_->callback(toggle_cb, this);
    toggle_btn_->tooltip("Show or hide the file transfer list");

    aggregate_bar_ = new Fl_Progress(dx + m + 126, dy + 1, dw - 2 * m - 126 - 106 - 130, 20);
    aggregate_bar_->minimum(0);
    aggregate_bar_->maximum(100);
    aggregate_bar_->value(0);
    aggregate_bar_->color(FL_BACKGROUND_COLOR);
    aggregate_bar_->selection_color(FL_BLUE);
    aggregate_bar_->label("0%");

    busy_label_ = new Fl_Box(dx + dw - m - 306, dy + 1, 124, 20, "");
    busy_label_->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
    busy_label_->labelsize(11);
    busy_label_->labelcolor(FL_DARK_BLUE);
    g_busy_label = busy_label_;

    cancel_btn_ = new Fl_Button(dx + dw - m - 176, dy, 70, 22, "Cancel");
    cancel_btn_->callback(cancel_cb, this);
    cancel_btn_->tooltip("Stop every queued and in-flight transfer");

    shot_btn_ = new Fl_Button(dx + dw - m - 100, dy, 100, 22, "Screenshot");
    shot_btn_->callback(screenshot_cb, this);
    shot_btn_->tooltip("Grab the device's screen and save it as a PNG on this machine");

    table_ = new TransferTable(dx + m, dy + 26, dw - 2 * m, dh - 26);
    table_->queue(&queue_);
    table_->hide();
}

TransferPane::~TransferPane()
{
    for (size_t i = 0; i < active_.size(); i++) {
        active_[i]->abandon();
        delete active_[i];
    }
    active_.clear();
}

bool TransferPane::queue_path(const std::string &path, const std::string &options,
                              const std::string &dest, const std::string &machine)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;

    QueuedFile qf;
    qf.path = path;
    qf.name = basename_of(path);
    qf.total_size = static_cast<uint64_t>(st.st_size);
    qf.crc32 = compute_file_crc32(path);
    qf.machine = machine;
    qf.options = options;
    qf.dest = dest.empty() ? dest_dir() : dest;
    qf.row = queue_.add(qf.name, qf.total_size);

    files_.push_back(qf);
    spawn_attempt(static_cast<int>(files_.size()) - 1);

    std::string::size_type slash = qf.path.rfind('/');
    if (slash != std::string::npos)
        last_dir_ = qf.path.substr(0, slash);
    return true;
}

void TransferPane::choose_and_queue(const char *pattern, const char *title)
{
    Fl_File_Chooser chooser(last_dir_.empty() ? "." : last_dir_.c_str(), pattern,
                             Fl_File_Chooser::MULTI, title);
    chooser.show();
    while (chooser.shown())
        Fl::wait();

    if (!chooser.value(1))
        return;

    for (int i = 1; i <= chooser.count(); i++) {
        const char *path = chooser.value(i);
        if (path)
            queue_path(path);
    }
    sync_table();
}

void TransferPane::do_add_files()
{
    choose_and_queue("*", "Add files to send");
}

void TransferPane::do_retry_failed()
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
    Fl_Check_Button *deploy_root_image_;
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

class ManagerPane;
class TransferFilesPane;

class SettingsStore {
public:
    SettingsStore() : client_(0), roms_(0), runner_(0), files_(0) { cfg_.load(); }

    const Settings &cfg() const { return cfg_; }

    void bind(TransferPane *client, TransferPane *roms, BuildRunner *runner)
    {
        client_ = client;
        roms_ = roms;
        runner_ = runner;
    }

    void add_manager(ManagerPane *m) { managers_.push_back(m); }
    void add_files_pane(TransferFilesPane *f) { files_ = f; }

    void save_now();

private:
    Settings cfg_;
    TransferPane *client_;
    TransferPane *roms_;
    BuildRunner *runner_;
    TransferFilesPane *files_;
    std::vector<ManagerPane *> managers_;
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
    deploy_root_image_ = new Fl_Check_Button(X + m + 120, y, 200, 20, "deploy-root-image");
    deploy_root_image_->tooltip("Stage piko-root.img.new, promoted on the next boot");
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

    kernel_only_->value(cfg.get_bool("build.kernel_only", false) ? 1 : 0);
    force_kernel_src_->value(cfg.get_bool("build.force_kernel_src", false) ? 1 : 0);
    skip_userspace_->value(cfg.get_bool("build.skip_userspace", false) ? 1 : 0);
    skip_st_->value(cfg.get_bool("build.skip_st", false) ? 1 : 0);
    skip_x11_->value(cfg.get_bool("build.skip_x11", false) ? 1 : 0);
    build_only_->value(cfg.get_bool("build.build_only", false) ? 1 : 0);
    no_backup_->value(cfg.get_bool("build.no_backup", true) ? 1 : 0);
    deploy_root_image_->value(cfg.get_bool("build.deploy_root_image", false) ? 1 : 0);
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

    cfg.set_bool("build.kernel_only", kernel_only_->value() != 0);
    cfg.set_bool("build.force_kernel_src", force_kernel_src_->value() != 0);
    cfg.set_bool("build.skip_userspace", skip_userspace_->value() != 0);
    cfg.set_bool("build.skip_st", skip_st_->value() != 0);
    cfg.set_bool("build.skip_x11", skip_x11_->value() != 0);
    cfg.set_bool("build.build_only", build_only_->value() != 0);
    cfg.set_bool("build.no_backup", no_backup_->value() != 0);
    cfg.set_bool("build.deploy_root_image", deploy_root_image_->value() != 0);
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
    if (deploy_root_image_->value()) args.push_back("--deploy-root-image");
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

static bool connect_with_timeout(int fd, struct sockaddr_in *addr, int secs, std::string &err)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int rc = connect(fd, reinterpret_cast<struct sockaddr *>(addr), sizeof(*addr));
    if (rc != 0 && errno != EINPROGRESS) {
        err = strerror(errno);
        return false;
    }

    if (rc != 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv;
        tv.tv_sec = secs;
        tv.tv_usec = 0;
        int n = select(fd + 1, 0, &wfds, 0, &tv);
        if (n == 0) { err = "timed out reaching the device"; return false; }
        if (n < 0) { err = strerror(errno); return false; }

        int soerr = 0;
        socklen_t len = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0) {
            err = strerror(soerr ? soerr : errno);
            return false;
        }
    }

    fcntl(fd, F_SETFL, fl);
    return true;
}

static bool rom_request(const std::string &address, uint32_t type,
                        const std::string &payload, uint32_t want,
                        std::string &reply, std::string &err,
                        int timeout_secs = SHOT_TIMEOUT_SECS)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err = strerror(errno); return false; }

    struct timeval tv;
    tv.tv_sec = SHOT_TIMEOUT_SECS;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEFAULT_PORT);
    if (address.empty() || inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        err = "not a valid IPv4 address: " + address;
        close(fd);
        return false;
    }
    if (!connect_with_timeout(fd, &addr, ROM_CONNECT_SECS, err)) {
        err = "cannot reach the device: " + err;
        close(fd);
        return false;
    }

    FrameReader reader;
    uint32_t t;
    std::string p;

    HelloMsg hello;
    hello.version = PROTO_VERSION;
    if (!send_frame_blocking(fd, MSG_HELLO, encode(hello))) {
        err = "sending HELLO failed"; close(fd); return false;
    }
    if (!shot_recv_frame(fd, reader, t, p, err)) { close(fd); return false; }

    if (!send_frame_blocking(fd, type, payload)) {
        err = "sending request failed"; close(fd); return false;
    }
    if (!shot_recv_frame(fd, reader, t, p, err)) { close(fd); return false; }
    close(fd);

    if (t == MSG_ERROR) {
        ErrorMsg em;
        err = decode_error(p, em) ? em.message : "device reported an error";
        return false;
    }
    if (t != want) {
        err = "device does not support this request (update piko-sync-server)";
        return false;
    }
    reply = p;
    return true;
}

static bool fetch_rom_list(const std::string &address, std::string &records, std::string &err)
{
    std::string reply;
    if (!rom_request(address, MSG_ROM_LIST, std::string(), MSG_ROM_LIST_ACK, reply, err))
        return false;
    RomListAckMsg ack;
    if (!decode_rom_list_ack(reply, ack)) { err = "malformed rom list"; return false; }
    records = ack.records;
    return true;
}

static bool delete_rom(const std::string &address, const std::string &path, std::string &err)
{
    PathMsg pm;
    pm.path = path;
    std::string reply;
    if (!rom_request(address, MSG_ROM_DELETE, encode(pm), MSG_ROM_DELETE_ACK, reply, err))
        return false;
    OkReasonMsg ack;
    if (!decode_ok_reason(reply, ack)) { err = "malformed delete reply"; return false; }
    if (!ack.ok) { err = ack.reason; return false; }
    return true;
}

static bool set_rom_icon(const std::string &address, const std::string &rom_path,
                         const std::string &png, std::string &err)
{
    RomIconMsg m;
    m.rom_path = rom_path;
    m.icon_name = "icon.png";
    m.data = png;
    std::string reply;
    if (!rom_request(address, MSG_ROM_SET_ICON, encode(m), MSG_ROM_SET_ICON_ACK, reply, err))
        return false;
    OkReasonMsg ack;
    if (!decode_ok_reason(reply, ack)) { err = "malformed reply"; return false; }
    if (!ack.ok) { err = ack.reason; return false; }
    return true;
}

static bool get_rom_icon(const std::string &address, const std::string &rom_path,
                         std::string &png, std::string &err)
{
    PathMsg pm;
    pm.path = rom_path;
    std::string reply;
    if (!rom_request(address, MSG_ROM_GET_ICON, encode(pm), MSG_ROM_GET_ICON_ACK, reply, err))
        return false;
    RomIconMsg m;
    if (!decode_rom_icon(reply, m)) { err = "malformed icon reply"; return false; }
    png = m.data;
    return true;
}

static const char *media_base(int idx)
{
    switch (idx) {
    case 1:  return "/mnt/card";
    case 2:  return "/mnt/cf";
    default: return "/usr/local";
    }
}

static const char *media_name(int idx)
{
    switch (idx) {
    case 1:  return "SD";
    case 2:  return "CF";
    default: return "NAND";
    }
}

struct BackendDef {
    const char *key;
    const char *label;
    const char *machine;
    const char *subdir;
    const char *media_key;
    const char *add_label;
    const char *add_tip;
    const char *pattern;
    const char *chooser_title;
    const char *reject_hint;
    bool j2me;
};

static const BackendDef BACKENDS[] = {
    { "phoneme", "phoneME", "J2ME", "Applets", "applet.media",
      "Add J2ME applet...",
      "Send a MIDlet to the device, install it and register it",
      "J2ME applets (*.{jar,jad})",
      "Add J2ME applets to send",
      "A MIDlet is a .jar with a MIDlet manifest, or its .jad descriptor.",
      true }
};

static const int BACKEND_COUNT = (int)(sizeof(BACKENDS) / sizeof(BACKENDS[0]));

struct ManagerSpec {
    const char *dest_label;
    const char *empty_text;
};


static void bezel_scan_dir(const std::string &dir, std::vector<std::string> &out, int depth)
{
    if (depth > 6)
        return;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        std::string full = dir + "/" + name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            bezel_scan_dir(full, out, depth + 1);
        else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".cfg") == 0)
            out.push_back(full);
    }
    closedir(d);
}

class BezelPreviewBox : public Fl_Box {
public:
    BezelPreviewBox(int X, int Y, int W, int H)
        : Fl_Box(X, Y, W, H), img_(0), src_w_(0), src_h_(0),
          rx_(0), ry_(0), rw_(0), rh_(0), have_rect_(false)
    {
        box(FL_DOWN_BOX);
        color(FL_BACKGROUND2_COLOR);
    }

    ~BezelPreviewBox() { delete img_; }

    void set_frame(Fl_RGB_Image *img, int src_w, int src_h)
    {
        delete img_;
        img_ = img;
        src_w_ = src_w;
        src_h_ = src_h;
        redraw();
    }

    void clear_frame()
    {
        delete img_;
        img_ = 0;
        src_w_ = src_h_ = 0;
        have_rect_ = false;
        redraw();
    }

    void set_rect(int x, int y, int w, int h)
    {
        rx_ = x; ry_ = y; rw_ = w; rh_ = h;
        have_rect_ = (w > 0 && h > 0);
        redraw();
    }

    void draw()
    {
        draw_box();
        if (!img_ || src_w_ <= 0 || src_h_ <= 0) {
            fl_color(FL_INACTIVE_COLOR);
            fl_font(FL_HELVETICA, 11);
            fl_draw(label() ? label() : "", x(), y(), w(), h(),
                    FL_ALIGN_CENTER | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
            return;
        }

        int iw = img_->w(), ih = img_->h();
        int ox = x() + (w() - iw) / 2;
        int oy = y() + (h() - ih) / 2;
        img_->draw(ox, oy);

        if (!have_rect_)
            return;
        double sx = (double)iw / src_w_;
        double sy = (double)ih / src_h_;
        int bx = ox + (int)(rx_ * sx + 0.5);
        int by = oy + (int)(ry_ * sy + 0.5);
        int bw = (int)(rw_ * sx + 0.5);
        int bh = (int)(rh_ * sy + 0.5);
        fl_color(FL_MAGENTA);
        fl_line_style(FL_SOLID, 1);
        fl_rect(bx, by, bw, bh);
        fl_rect(bx - 1, by - 1, bw + 2, bh + 2);
        fl_line_style(0);
    }

private:
    Fl_RGB_Image *img_;
    int src_w_, src_h_;
    int rx_, ry_, rw_, rh_;
    bool have_rect_;
};

static Fl_RGB_Image *pkbz_to_image(const std::string &blob, PkbzHeader &hdr,
                                   int max_w, int max_h)
{
    size_t off = 0;
    if (!pkbz_decode_header(blob, hdr, off))
        return 0;

    int W = (int)hdr.width, H = (int)hdr.height;
    unsigned char *rgb = new unsigned char[(size_t)W * H * 3];
    const unsigned char *px = (const unsigned char *)blob.data() + off;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        unsigned short v = (unsigned short)(px[i * 2] | (px[i * 2 + 1] << 8));
        rgb[i * 3 + 0] = (unsigned char)(((v >> 11) & 0x1F) << 3);
        rgb[i * 3 + 1] = (unsigned char)(((v >> 5) & 0x3F) << 2);
        rgb[i * 3 + 2] = (unsigned char)((v & 0x1F) << 3);
    }
    Fl_RGB_Image *full = new Fl_RGB_Image(rgb, W, H, 3);
    full->alloc_array = 1;

    double s = (double)max_w / W;
    double sy = (double)max_h / H;
    if (sy < s) s = sy;
    if (s < 1.0) {
        Fl_RGB_Image *small = (Fl_RGB_Image *)full->copy((int)(W * s), (int)(H * s));
        delete full;
        return small;
    }
    return full;
}

struct DeviceBezel {
    std::string name;
    int w, h;
    int sx, sy, sw, sh;
};

static bool fetch_bezel_list(const std::string &address,
                             std::vector<DeviceBezel> &out, std::string &err)
{
    std::string reply;
    if (!rom_request(address, MSG_BEZEL_LIST, std::string(), MSG_BEZEL_LIST_ACK, reply, err))
        return false;
    BezelListAckMsg ack;
    if (!decode_bezel_list_ack(reply, ack)) { err = "malformed bezel list"; return false; }

    out.clear();
    std::string line;
    for (size_t i = 0; i <= ack.records.size(); i++) {
        if (i == ack.records.size() || ack.records[i] == '\n') {
            if (!line.empty()) {
                DeviceBezel b;
                b.w = b.h = b.sx = b.sy = b.sw = b.sh = 0;
                std::vector<std::string> f;
                std::string cur;
                for (size_t j = 0; j <= line.size(); j++) {
                    if (j == line.size() || line[j] == '|') { f.push_back(cur); cur.clear(); }
                    else cur += line[j];
                }
                if (f.size() >= 7) {
                    b.name = f[0];
                    b.w  = atoi(f[1].c_str()); b.h  = atoi(f[2].c_str());
                    b.sx = atoi(f[3].c_str()); b.sy = atoi(f[4].c_str());
                    b.sw = atoi(f[5].c_str()); b.sh = atoi(f[6].c_str());
                    out.push_back(b);
                }
            }
            line.clear();
        } else {
            line += ack.records[i];
        }
    }
    return true;
}

static bool delete_bezel(const std::string &address, const std::string &name,
                         std::string &err)
{
    BezelBlobMsg m;
    m.name = name;
    std::string reply;
    if (!rom_request(address, MSG_BEZEL_DELETE, encode(m), MSG_BEZEL_DELETE_ACK, reply, err))
        return false;
    OkReasonMsg ack;
    if (!decode_ok_reason(reply, ack)) { err = "malformed reply"; return false; }
    if (!ack.ok) { err = ack.reason; return false; }
    return true;
}

static bool get_bezel_blob(const std::string &address, const std::string &name,
                           std::string &data, std::string &err)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err = strerror(errno); return false; }

    struct timeval tv;
    tv.tv_sec = BEZEL_FETCH_TIMEOUT_SECS;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DEFAULT_PORT);
    if (address.empty() || inet_pton(AF_INET, address.c_str(), &addr.sin_addr) != 1) {
        err = "not a valid IPv4 address: " + address;
        close(fd);
        return false;
    }
    if (!connect_with_timeout(fd, &addr, ROM_CONNECT_SECS, err)) {
        err = "cannot reach the device: " + err;
        close(fd);
        return false;
    }

    FrameReader reader;
    uint32_t t;
    std::string p;

    HelloMsg hello;
    hello.version = PROTO_VERSION;
    if (!send_frame_blocking(fd, MSG_HELLO, encode(hello))) {
        err = "sending HELLO failed"; close(fd); return false;
    }
    if (!shot_recv_frame(fd, reader, t, p, err)) { close(fd); return false; }

    BezelBlobMsg m;
    m.name = name;
    if (!send_frame_blocking(fd, MSG_BEZEL_GET, encode(m))) {
        err = "sending request failed"; close(fd); return false;
    }

    data.clear();
    uint32_t total = 0;
    for (;;) {
        if (!shot_recv_frame(fd, reader, t, p, err)) { close(fd); return false; }
        if (t == MSG_ERROR) {
            ErrorMsg em;
            err = decode_error(p, em) ? em.message : "device reported an error";
            close(fd);
            return false;
        }
        if (t != MSG_BEZEL_GET_ACK) {
            err = "device does not support this request (update piko-sync-server)";
            close(fd);
            return false;
        }
        BezelChunkMsg c;
        if (!decode_bezel_chunk(p, c)) {
            err = "malformed bezel chunk";
            close(fd);
            return false;
        }
        if (c.total == 0) {
            close(fd);
            return true;
        }
        total = c.total;
        if (c.offset != data.size()) {
            err = "bezel chunks arrived out of order";
            close(fd);
            return false;
        }
        data += c.data;
        if (data.size() >= total)
            break;
    }
    close(fd);
    return true;
}

static const char *BEZEL_GLOBAL_PATH = "@global";

static std::string bezel_backend_path(const char *key)
{
    return std::string("@backend:") + key;
}

static bool set_rom_option(const std::string &address, const std::string &path,
                           const std::string &key, const std::string &value,
                           std::string &err)
{
    RomOptionMsg m;
    m.path = path;
    m.key = key;
    m.value = value;
    std::string reply;
    if (!rom_request(address, MSG_ROM_SET_OPTION, encode(m),
                     MSG_ROM_SET_OPTION_ACK, reply, err))
        return false;
    OkReasonMsg ack;
    if (!decode_ok_reason(reply, ack)) { err = "malformed reply"; return false; }
    if (!ack.ok) { err = ack.reason; return false; }
    return true;
}

struct BezelAssignments {
    std::string global;
    std::map<std::string, std::string> per_backend;
    std::map<std::string, int> game_counts;
};

static bool fetch_bezel_assignments(const std::string &address,
                                    BezelAssignments &out, std::string &err)
{
    std::string records;
    if (!fetch_rom_list(address, records, err))
        return false;

    out.global.clear();
    out.per_backend.clear();
    out.game_counts.clear();

    std::string line;
    for (size_t i = 0; i <= records.size(); i++) {
        if (i == records.size() || records[i] == '\n') {
            if (!line.empty()) {
                RomEntry e;
                if (decode_entry(line, e)) {
                    std::string bez = option_unescape(option_get(e.options, "bezel"));
                    if (!bez.empty()) {
                        if (e.path == BEZEL_GLOBAL_PATH)
                            out.global = bez;
                        else if (e.path.compare(0, 9, "@backend:") == 0)
                            out.per_backend[e.path.substr(9)] = bez;
                        else
                            out.game_counts[bez]++;
                    }
                }
            }
            line.clear();
        } else {
            line += records[i];
        }
    }
    return true;
}

static bool set_bezel_rect(const std::string &address, const std::string &name,
                           int x, int y, int w, int h, std::string &err)
{
    BezelRectMsg m;
    m.name = name;
    m.x = (uint32_t)x; m.y = (uint32_t)y;
    m.w = (uint32_t)w; m.h = (uint32_t)h;
    std::string reply;
    if (!rom_request(address, MSG_BEZEL_SET_RECT, encode(m),
                     MSG_BEZEL_SET_RECT_ACK, reply, err))
        return false;
    OkReasonMsg ack;
    if (!decode_ok_reason(reply, ack)) { err = "malformed reply"; return false; }
    if (!ack.ok) { err = ack.reason; return false; }
    return true;
}

static bool put_bezel(const std::string &address, const std::string &name,
                      const std::string &data, int media, std::string &err)
{
    BezelBlobMsg m;
    m.name = name;
    m.media = (uint32_t)media;
    m.data = data;
    std::string reply;
    if (!rom_request(address, MSG_BEZEL_PUT, encode(m), MSG_BEZEL_PUT_ACK, reply, err))
        return false;
    OkReasonMsg ack;
    if (!decode_ok_reason(reply, ack)) { err = "malformed reply"; return false; }
    if (!ack.ok) { err = ack.reason; return false; }
    return true;
}

class PresetPicker {
public:
    static bool run(const std::vector<std::string> &paths,
                    const std::vector<std::string> &names,
                    std::vector<int> &chosen)
    {
        PresetPicker p(paths, names);
        p.win_->set_modal();
        p.win_->show();
        while (p.win_->shown())
            Fl::wait();
        if (!p.accepted_)
            return false;
        chosen.clear();
        for (int i = 1; i <= p.list_->nitems(); i++)
            if (p.list_->checked(i))
                chosen.push_back(i - 1);
        return !chosen.empty();
    }

private:
    PresetPicker(const std::vector<std::string> &paths,
                 const std::vector<std::string> &names)
        : accepted_(false)
    {
        (void)paths;
        win_ = new Fl_Double_Window(460, 400, "Choose bezels to install");
        win_->begin();
        Fl_Box *hint = new Fl_Box(10, 8, 440, 20,
                                  "Tick the presets to bake and send to the device.");
        hint->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
        hint->labelsize(11);

        list_ = new Fl_Check_Browser(10, 32, 440, 292);
        for (size_t i = 0; i < names.size(); i++)
            list_->add(names[i].c_str(), 1);

        Fl_Button *all = new Fl_Button(10, 332, 60, 24, "All");
        all->callback(all_cb, this);
        Fl_Button *none = new Fl_Button(76, 332, 60, 24, "None");
        none->callback(none_cb, this);

        count_ = new Fl_Box(146, 332, 150, 24, "");
        count_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
        count_->labelsize(11);

        Fl_Button *ok = new Fl_Button(290, 332, 76, 24, "Install");
        ok->callback(ok_cb, this);
        Fl_Button *cancel = new Fl_Button(374, 332, 76, 24, "Cancel");
        cancel->callback(cancel_cb, this);
        win_->end();
        update_count();
    }

    ~PresetPicker() { delete win_; }

    static void all_cb(Fl_Widget *, void *v) { ((PresetPicker *)v)->set_all(1); }
    static void none_cb(Fl_Widget *, void *v) { ((PresetPicker *)v)->set_all(0); }
    static void ok_cb(Fl_Widget *, void *v) { ((PresetPicker *)v)->accept(); }
    static void cancel_cb(Fl_Widget *, void *v) { ((PresetPicker *)v)->win_->hide(); }

    void set_all(int on)
    {
        if (on)
            list_->check_all();
        else
            list_->check_none();
        list_->redraw();
        update_count();
    }

    void update_count()
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%d of %d selected",
                 list_->nchecked(), list_->nitems());
        count_->copy_label(buf);
        count_->redraw();
    }

    void accept()
    {
        if (list_->nchecked() == 0) {
            fl_alert("Tick at least one preset, or press Cancel.");
            return;
        }
        accepted_ = true;
        win_->hide();
    }

    Fl_Double_Window *win_;
    Fl_Check_Browser *list_;
    Fl_Box *count_;
    bool accepted_;
};

static bool write_local_file(const std::string &path, const std::string &data)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok)
        remove(path.c_str());
    return ok;
}

class BezelChooser {
public:
    static bool run(const std::string &address, const std::string &current,
                    std::string &picked)
    {
        std::vector<DeviceBezel> list;
        std::string err;
        if (!fetch_bezel_list(address, list, err)) {
            fl_alert("Could not read the bezel list from the device:\n%s", err.c_str());
            return false;
        }
        if (list.empty()) {
            fl_alert("No bezels installed on the device yet.\n\n"
                     "Add some from Emulation Settings first.");
            return false;
        }

        BezelChooser c(address, list, current);
        c.win_->set_modal();
        c.win_->show();
        while (c.win_->shown())
            Fl::wait();
        if (!c.accepted_)
            return false;
        picked = c.picked_;
        return true;
    }

private:
    BezelChooser(const std::string &address, const std::vector<DeviceBezel> &list,
                 const std::string &current)
        : address_(address), list_(list), accepted_(false)
    {
        win_ = new Fl_Double_Window(560, 320, "Choose a bezel");
        win_->begin();
        browser_ = new Fl_Hold_Browser(10, 10, 280, 265);
        browser_->callback(sel_cb, this);
        browser_->when(FL_WHEN_CHANGED);
        for (size_t i = 0; i < list_.size(); i++) {
            browser_->add(list_[i].name.c_str());
            if (list_[i].name == current)
                browser_->value((int)i + 1);
        }
        preview_ = new BezelPreviewBox(300, 10, 250, 190);
        preview_->label("select a bezel");

        Fl_Button *ok = new Fl_Button(300, 285, 80, 24, "Use");
        ok->callback(ok_cb, this);
        Fl_Button *cancel = new Fl_Button(390, 285, 80, 24, "Cancel");
        cancel->callback(cancel_cb, this);
        win_->end();
        if (browser_->value() >= 1)
            show_sel();
    }

    ~BezelChooser() { delete win_; }

    static void sel_cb(Fl_Widget *, void *v) { ((BezelChooser *)v)->show_sel(); }
    static void ok_cb(Fl_Widget *, void *v) { ((BezelChooser *)v)->accept(); }
    static void cancel_cb(Fl_Widget *, void *v) { ((BezelChooser *)v)->win_->hide(); }

    void accept()
    {
        int sel = browser_->value();
        if (sel < 1 || (size_t)sel > list_.size()) {
            fl_alert("Pick a bezel first.");
            return;
        }
        picked_ = list_[sel - 1].name;
        accepted_ = true;
        win_->hide();
    }

    void show_sel()
    {
        int sel = browser_->value();
        if (sel < 1 || (size_t)sel > list_.size()) {
            preview_->clear_frame();
            preview_->label("select a bezel");
            return;
        }
        const DeviceBezel &b = list_[sel - 1];
        preview_->clear_frame();
        preview_->label("fetching...");
        preview_->redraw();
        Fl::flush();

        std::string blob, err;
        if (!get_bezel_blob(address_, b.name, blob, err) || blob.empty()) {
            preview_->label("preview unavailable");
            preview_->redraw();
            return;
        }
        PkbzHeader hdr;
        Fl_RGB_Image *img = pkbz_to_image(blob, hdr, preview_->w() - 6,
                                          preview_->h() - 6);
        if (!img) {
            preview_->label("preview unreadable");
            preview_->redraw();
            return;
        }
        preview_->label("");
        preview_->set_frame(img, (int)hdr.width, (int)hdr.height);
        preview_->set_rect(b.sx / 2, b.sy / 2, b.sw / 2, b.sh / 2);
    }

    std::string address_;
    std::vector<DeviceBezel> list_;
    Fl_Double_Window *win_;
    Fl_Hold_Browser *browser_;
    BezelPreviewBox *preview_;
    std::string picked_;
    bool accepted_;
};

class EmulationSettingsWindow {
public:
    EmulationSettingsWindow()
        : win_(0), list_(0), preview_(0), preview_image_(0),
          progress_win_(0), progress_label_(0), progress_bar_(0), cancel_(false),
          xfer_(0) {}

    void set_address(const std::string &a) { address_ = a; }
    void set_xfer(TransferPane *x) { xfer_ = x; }

    ~EmulationSettingsWindow() { delete preview_image_; delete win_; }

    void show()
    {
        if (!win_)
            build();
        reload();
        win_->show();
    }

    void reload()
    {
        BusyCursor busy("Loading bezels...");
        std::string err;
        if (!fetch_bezel_list(address_, bezels_, err))
            bezels_.clear();
        if (!fetch_bezel_assignments(address_, assign_, err))
            assign_ = BezelAssignments();
        repopulate();
        update_assignment_buttons();
        selection_changed();
    }

    void update_assignment_buttons()
    {
        if (assign_.global.empty())
            rm_global_btn_->hide();
        else
            rm_global_btn_->show();

        std::map<std::string, std::string>::const_iterator it =
            assign_.per_backend.find(current_backend_key());
        if (it == assign_.per_backend.end() || it->second.empty())
            rm_backend_btn_->hide();
        else
            rm_backend_btn_->show();
    }

private:
    void build()
    {
        int W = 720, H = 470, m = 10;

        win_ = new Fl_Double_Window(W, H, "Emulation Settings");
        win_->begin();

        Fl_Tabs *tabs = new Fl_Tabs(0, 0, W, H);
        tabs->begin();

        int py = 26;
        int ph = H - py;
        Fl_Group *bez = new Fl_Group(0, py, W, ph, "Bezel");
        bez->begin();

        int y = py + m;

        add_btn_ = new Fl_Button(m, y, 200, 24, "Add RetroArch bezel family...");
        add_btn_->callback(add_cb, this);
        add_btn_->tooltip("Scan a RetroArch overlay pack for .cfg presets");

        del_btn_ = new Fl_Button(m + 206, y, 80, 24, "Delete");
        del_btn_->callback(del_cb, this);
        del_btn_->deactivate();

        store_media_ = new Fl_Choice(W - m - 60, y, 60, 24, "Stored on:");
        store_media_->align(FL_ALIGN_LEFT);
        store_media_->add("SD");
        store_media_->add("CF");
        store_media_->value(0);
        store_media_->tooltip("Which storage new bezels are written to");

        y += 30;

        set_global_btn_ = new Fl_Button(m, y, 140, 24, "Set global bezel");
        set_global_btn_->callback(set_global_cb, this);
        set_global_btn_->deactivate();

        rm_global_btn_ = new Fl_Button(m + 146, y, 160, 24, "Remove global bezel");
        rm_global_btn_->callback(rm_global_cb, this);
        rm_global_btn_->hide();

        y += 30;

        backend_ = new Fl_Choice(m + 86, y, 130, 24, "Per backend:");
        backend_->align(FL_ALIGN_LEFT);
        for (int i = 0; i < BACKEND_COUNT; i++)
            backend_->add(BACKENDS[i].label);
        backend_->value(0);
        backend_->callback(backend_changed_cb, this);

        set_backend_btn_ = new Fl_Button(m + 222, y, 100, 24, "Set bezel");
        set_backend_btn_->callback(set_backend_cb, this);
        set_backend_btn_->deactivate();

        rm_backend_btn_ = new Fl_Button(m + 328, y, 120, 24, "Remove bezel");
        rm_backend_btn_->callback(rm_backend_cb, this);
        rm_backend_btn_->hide();

        y += 32;
        int list_w = W - 2 * m - 270;
        int list_h = H - y - m;
        list_ = new Fl_Hold_Browser(m, y, list_w, list_h);
        list_->callback(select_cb, this);
        list_->when(FL_WHEN_CHANGED);
        static const int widths[] = { 230, 0 };
        list_->column_widths(widths);
        list_->column_char('\t');

        preview_ = new BezelPreviewBox(m + list_w + 10, y, 260, 195);
        preview_->label("no bezel selected");

        int fy = y + 201;
        int fx = m + list_w + 10;
        rect_x_ = new Fl_Int_Input(fx + 18, fy, 48, 22, "X");
        rect_y_ = new Fl_Int_Input(fx + 84, fy, 48, 22, "Y");
        rect_w_ = new Fl_Int_Input(fx + 150, fy, 48, 22, "W");
        rect_h_ = new Fl_Int_Input(fx + 216, fy, 34, 22, "H");
        rect_x_->align(FL_ALIGN_LEFT); rect_y_->align(FL_ALIGN_LEFT);
        rect_w_->align(FL_ALIGN_LEFT); rect_h_->align(FL_ALIGN_LEFT);
        rect_x_->callback(rect_changed_cb, this);
        rect_y_->callback(rect_changed_cb, this);
        rect_w_->callback(rect_changed_cb, this);
        rect_h_->callback(rect_changed_cb, this);
        rect_x_->when(FL_WHEN_CHANGED);
        rect_y_->when(FL_WHEN_CHANGED);
        rect_w_->when(FL_WHEN_CHANGED);
        rect_h_->when(FL_WHEN_CHANGED);
        rect_x_->tooltip("Where the game screen sits inside the bezel, in 640x480 units");

        apply_rect_btn_ = new Fl_Button(fx, fy + 26, 120, 24, "Apply screen rect");
        apply_rect_btn_->callback(apply_rect_cb, this);
        apply_rect_btn_->deactivate();

        bez->end();
        tabs->end();
        win_->end();
        win_->set_non_modal();
    }

    static void add_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->add_family(); }
    static void del_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->delete_selected(); }
    static void select_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->selection_changed(); }
    static void set_global_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->assign_global(true); }
    static void rm_global_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->assign_global(false); }
    static void set_backend_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->assign_backend(true); }
    static void rect_changed_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->rect_changed(); }
    static void cancel_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->cancel_ = true; }
    static void backend_changed_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->update_assignment_buttons(); }
    static void apply_rect_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->apply_rect(); }

    int field_int(Fl_Int_Input *f) const
    {
        const char *t = f->value();
        return (t && *t) ? atoi(t) : 0;
    }

    void rect_changed()
    {
        preview_->set_rect(field_int(rect_x_), field_int(rect_y_),
                           field_int(rect_w_), field_int(rect_h_));
        int sel = list_->value();
        if (sel >= 1 && (size_t)sel <= bezels_.size())
            apply_rect_btn_->activate();
    }

    void apply_rect()
    {
        BusyCursor busy("Saving...");
        int sel = list_->value();
        if (sel < 1 || (size_t)sel > bezels_.size())
            return;
        int x = field_int(rect_x_), y = field_int(rect_y_);
        int w = field_int(rect_w_), h = field_int(rect_h_);
        if (w <= 0 || h <= 0) { fl_alert("Width and height must be positive."); return; }

        std::string err;
        if (!set_bezel_rect(address_, bezels_[sel - 1].name, x, y, w, h, err)) {
            fl_alert("Could not set the screen rect:\n%s", err.c_str());
            return;
        }
        apply_rect_btn_->deactivate();
        reload();
    }

    void set_rect_fields(int x, int y, int w, int h)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", x); rect_x_->value(buf);
        snprintf(buf, sizeof(buf), "%d", y); rect_y_->value(buf);
        snprintf(buf, sizeof(buf), "%d", w); rect_w_->value(buf);
        snprintf(buf, sizeof(buf), "%d", h); rect_h_->value(buf);
    }
    static void rm_backend_cb(Fl_Widget *, void *v) { ((EmulationSettingsWindow *)v)->assign_backend(false); }

    const char *current_backend_key() const
    {
        int i = backend_->value();
        if (i < 0 || i >= BACKEND_COUNT) i = 0;
        return BACKENDS[i].key;
    }

    void assign_global(bool set)
    {
        BusyCursor busy("Saving...");
        std::string name, err;
        if (set) {
            int sel = list_->value();
            if (sel < 1 || (size_t)sel > bezels_.size()) return;
            name = bezels_[sel - 1].name;
        }
        if (!set_rom_option(address_, BEZEL_GLOBAL_PATH, "bezel", name, err)) {
            fl_alert("Could not update the global bezel:\n%s", err.c_str());
            return;
        }
        reload();
    }

    void assign_backend(bool set)
    {
        BusyCursor busy("Saving...");
        std::string name, err;
        if (set) {
            int sel = list_->value();
            if (sel < 1 || (size_t)sel > bezels_.size()) return;
            name = bezels_[sel - 1].name;
        }
        if (!set_rom_option(address_, bezel_backend_path(current_backend_key()),
                            "bezel", name, err)) {
            fl_alert("Could not update the backend bezel:\n%s", err.c_str());
            return;
        }
        reload();
    }

    void not_yet()
    {
        fl_message("Assignment writes to emulation.cfg are the next step.");
    }

    void add_family()
    {
        const char *dir = fl_dir_chooser("Choose a RetroArch bezel pack", 0, 0);
        if (!dir)
            return;

        std::vector<std::string> scanned;
        bezel_scan_dir(dir, scanned, 0);

        std::vector<std::string> all_found, names;
        for (size_t i = 0; i < scanned.size(); i++) {
            BezelPreset probe;
            if (!bezel_parse_preset(scanned[i], probe))
                continue;
            all_found.push_back(scanned[i]);
            names.push_back(probe.name);
        }
        if (all_found.empty()) {
            fl_alert("No RetroArch overlay .cfg files found under:\n%s\n\n"
                     "An overlay preset needs an overlay0_overlay = line.", dir);
            return;
        }

        std::vector<int> chosen;
        if (!PresetPicker::run(all_found, names, chosen))
            return;

        std::vector<std::string> found;
        for (size_t i = 0; i < chosen.size(); i++)
            found.push_back(all_found[chosen[i]]);

        std::string stage_dir = "/tmp/piko-sync-bezels";
        mkdir(stage_dir.c_str(), 0700);

        cancel_ = false;
        progress_win_ = new Fl_Double_Window(360, 110, "Baking bezels");
        progress_win_->begin();
        progress_label_ = new Fl_Box(10, 12, 340, 40, "");
        progress_label_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
        progress_bar_ = new Fl_Progress(10, 56, 340, 20);
        progress_bar_->minimum(0);
        progress_bar_->maximum((float)found.size());
        Fl_Button *cancel = new Fl_Button(255, 82, 95, 22, "Cancel");
        cancel->callback(cancel_cb, this);
        progress_win_->end();
        progress_win_->set_modal();
        progress_win_->show();

        int ok = 0, failed = 0;
        std::string err;
        for (size_t i = 0; i < found.size() && !cancel_; i++) {
            BezelPreset preset;
            char msg[512];

            if (!bezel_parse_preset(found[i], preset)) { failed++; continue; }
            if (!piko_sync::bezel_name_safe(preset.name)) { failed++; continue; }

            snprintf(msg, sizeof(msg), "%d/%d  %s",
                     (int)i + 1, (int)found.size(), preset.name.c_str());
            progress_label_->copy_label(msg);
            progress_bar_->value((float)i);
            Fl::check();
            if (cancel_) break;

            BakedBezel master;
            if (!bezel_bake(preset, BEZEL_MASTER_W, BEZEL_MASTER_H, master, err)) {
                failed++;
                continue;
            }
            Fl::check();
            if (cancel_) break;

            int media = media_from_choice(store_media_->value());
            std::string dest = std::string(media_zaurus_root(media)) + "/bezels";
            std::string path = stage_dir + "/" + preset.name + ".pkbz";

            if (!write_local_file(path, bezel_to_pkbz(master, preset.preset_path))) {
                err = "cannot stage baked bezel under " + stage_dir;
                failed++;
                continue;
            }
            if (!xfer_ || !xfer_->queue_path(path, std::string(), dest, std::string())) {
                err = "could not queue the transfer";
                failed++;
                continue;
            }
            ok++;
        }

        progress_win_->hide();
        delete progress_win_;
        progress_win_ = 0;
        progress_label_ = 0;
        progress_bar_ = 0;

        if (xfer_)
            xfer_->refresh_queue_view();
        if (cancel_)
            fl_message("Cancelled after baking %d bezel%s.\n\n"
                       "Queued transfers continue in the Transfers dock.",
                       ok, ok == 1 ? "" : "s");
        else if (failed)
            fl_message("Baked and queued %d bezel%s, %d failed.\n\nLast error: %s",
                       ok, ok == 1 ? "" : "s", failed,
                       err.empty() ? "(none reported)" : err.c_str());
        else
            fl_message("Baked and queued %d bezel%s.\n\n"
                       "Watch the Transfers dock; Refresh the list when it finishes.",
                       ok, ok == 1 ? "" : "s");
    }

    void delete_selected()
    {
        int sel = list_->value();
        if (sel < 1 || (size_t)sel > bezels_.size())
            return;
        std::string name = bezels_[sel - 1].name;
        if (fl_choice("Delete bezel \"%s\" from the device?", "Cancel", "Delete", 0,
                      name.c_str()) != 1)
            return;
        std::string err;
        if (!delete_bezel(address_, name, err))
            fl_alert("Could not delete %s:\n%s", name.c_str(), err.c_str());
        list_->deselect();
        reload();
    }

    void selection_changed()
    {
        int sel = list_->value();
        bool have = (sel >= 1 && (size_t)sel <= bezels_.size());

        if (have) {
            del_btn_->activate();
            set_global_btn_->activate();
            set_backend_btn_->activate();
        } else {
            del_btn_->deactivate();
            set_global_btn_->deactivate();
            set_backend_btn_->deactivate();
        }
        show_preview(have ? &bezels_[sel - 1] : 0);
    }

    void show_preview(const DeviceBezel *b)
    {
        apply_rect_btn_->deactivate();
        if (!b) {
            preview_->clear_frame();
            preview_->label("no bezel selected");
            set_rect_fields(0, 0, 0, 0);
            preview_->redraw();
            return;
        }

        preview_->clear_frame();
        preview_->label("fetching preview...");
        preview_->redraw();
        Fl::flush();

        std::string blob, err;
        if (!get_bezel_blob(address_, b->name, blob, err) || blob.empty()) {
            preview_->label("preview unavailable");
            preview_->redraw();
            set_rect_fields(b->sx, b->sy, b->sw, b->sh);
            return;
        }

        PkbzHeader hdr;
        Fl_RGB_Image *img = pkbz_to_image(blob, hdr, preview_->w() - 6,
                                          preview_->h() - 6);
        if (!img) {
            preview_->label("preview unreadable");
            preview_->redraw();
            return;
        }
        preview_->label("");
        preview_->set_frame(img, (int)hdr.width, (int)hdr.height);
        set_rect_fields(b->sx, b->sy, b->sw, b->sh);
        preview_->set_rect(b->sx / 2, b->sy / 2, b->sw / 2, b->sh / 2);
    }

    void repopulate()
    {
        list_->clear();
        for (size_t i = 0; i < bezels_.size(); i++) {
            std::string row = bezels_[i].name + "\t" + used_in(bezels_[i]);
            list_->add(row.c_str());
        }
        if (bezels_.empty())
            list_->add("no bezels installed yet");
    }

    std::string used_in(const DeviceBezel &b) const
    {
        std::vector<std::string> parts;
        char buf[64];

        std::map<std::string, int>::const_iterator gc = assign_.game_counts.find(b.name);
        if (gc != assign_.game_counts.end() && gc->second > 0) {
            snprintf(buf, sizeof(buf), "%d Game%s", gc->second,
                     gc->second == 1 ? "" : "s");
            parts.push_back(buf);
        }
        for (int i = 0; i < BACKEND_COUNT; i++) {
            std::map<std::string, std::string>::const_iterator it =
                assign_.per_backend.find(BACKENDS[i].key);
            if (it != assign_.per_backend.end() && it->second == b.name)
                parts.push_back(BACKENDS[i].label);
        }
        if (assign_.global == b.name)
            parts.push_back("Global");

        if (parts.empty())
            return "-";
        std::string out;
        for (size_t i = 0; i < parts.size(); i++) {
            if (i) out += ", ";
            out += parts[i];
        }
        return out;
    }

    Fl_Double_Window *win_;
    Fl_Hold_Browser *list_;
    BezelPreviewBox *preview_;
    Fl_Image *preview_image_;
    Fl_Int_Input *rect_x_, *rect_y_, *rect_w_, *rect_h_;
    Fl_Button *apply_rect_btn_;
    Fl_Button *add_btn_, *del_btn_, *set_global_btn_, *rm_global_btn_;
    Fl_Button *set_backend_btn_, *rm_backend_btn_;
    Fl_Choice *backend_;
    Fl_Choice *store_media_;
    std::vector<DeviceBezel> bezels_;
    BezelAssignments assign_;
    std::string address_;
    Fl_Double_Window *progress_win_;
    Fl_Box *progress_label_;
    Fl_Progress *progress_bar_;
    bool cancel_;
    TransferPane *xfer_;
};

static EmulationSettingsWindow g_emu_settings;

class ManagerPane : public TransferObserver {
public:
    ManagerPane(Fl_Group *tab, TransferPane *xfer, int X, int Y, int W, int H,
                const Settings &cfg, const ManagerSpec &spec)
        : xfer_(xfer), spec_(spec), preview_(0), preview_image_(0),
          bezel_preview_(0), icon_btn_(0), rotate_chk_(0), backend_(0), settings_btn_(0),
          set_bezel_btn_(0), del_bezel_btn_(0)
    {
        int m = 10;
        int y = Y + m;
        tab->begin();

        media_ = new Fl_Choice(X + m + 90, y, 110, 24, spec_.dest_label);
        media_->align(FL_ALIGN_LEFT);
        media_->add("SD");
        media_->add("CF");
        {
            std::string saved = cfg.get(BACKENDS[0].media_key, "SD");
            media_->value(media_to_choice(part_media_from_name(saved)));
        }
        media_->tooltip("Which storage the device keeps these on");

        backend_ = new Fl_Choice(X + m + 285, y, 130, 24, "Backend:");
        backend_->align(FL_ALIGN_LEFT);
        for (int i = 0; i < BACKEND_COUNT; i++)
            backend_->add(BACKENDS[i].label);
        backend_->value(0);
        backend_->callback(backend_cb, this);
        backend_->tooltip("Which runtime new entries are added for");

        add_btn_ = new Fl_Button(X + m + 425, y, 165, 24, BACKENDS[0].add_label);
        add_btn_->callback(add_cb, this);
        add_btn_->tooltip(BACKENDS[0].add_tip);

        y += 30;

        refresh_btn_ = new Fl_Button(X + m, y, 90, 24, "Refresh");
        refresh_btn_->callback(refresh_cb, this);

        delete_btn_ = new Fl_Button(X + m + 96, y, 90, 24, "Delete");
        delete_btn_->callback(delete_cb, this);

        settings_btn_ = new Fl_Button(X + m + 192, y, 145, 24, "Emulation Settings...");
        settings_btn_->callback(settings_cb, this);
        settings_btn_->tooltip("Bezels and global emulation settings");

        set_bezel_btn_ = new Fl_Button(X + W - m - 110, y, 110, 24, "Set bezel...");
        set_bezel_btn_->callback(set_bezel_cb, this);
        set_bezel_btn_->tooltip("Choose the bezel this game runs with");

        del_bezel_btn_ = new Fl_Button(X + W - m - 234, y, 120, 24, "Remove bezel");
        del_bezel_btn_->callback(del_bezel_cb, this);
        del_bezel_btn_->hide();

        y += 30;

        int list_h = Y + H - y - m;
        int list_w = W - 2 * m - 56;
        list_ = new Fl_Hold_Browser(X + m, y, list_w, list_h);
        list_->callback(select_cb, this);
        list_->when(FL_WHEN_CHANGED);
        static const int widths[] = { 190, 45, 55, 130, 0 };
        list_->column_widths(widths);
        list_->column_char('\t');

        preview_ = new Fl_Box(X + W - m - 48, y, 48, 48);
        preview_->box(FL_DOWN_BOX);
        preview_->color(FL_BACKGROUND2_COLOR);
        preview_->tooltip("Icon matchbox-desktop shows for the selected entry");

        bezel_preview_ = new BezelPreviewBox(X + W - m - 48, y + 54, 48, 36);
        bezel_preview_->label("");
        bezel_preview_->tooltip("Bezel this entry runs with");

        tab->end();

        xfer_->add_observer(this);
        Fl::add_timeout(0.4, first_refresh_cb, this);
    }

    virtual ~ManagerPane() { delete preview_image_; }

    void on_transfer_complete()
    {
        if (!pending_icons_.empty()) {
            std::map<std::string, std::string>::iterator it;
            for (it = pending_icons_.begin(); it != pending_icons_.end(); ++it) {
                std::string err;
                set_rom_icon(address(), it->first, it->second, err);
            }
            pending_icons_.clear();
        }
        refresh();
    }

    void store_settings(Settings &cfg) const
    {
        cfg.set(backend().media_key, media_name(media_from_choice(media_->value())));
    }

private:
    static void first_refresh_cb(void *v) { ((ManagerPane *)v)->refresh(); }
    static void refresh_cb(Fl_Widget *, void *v) { ((ManagerPane *)v)->refresh(); }
    static void delete_cb(Fl_Widget *, void *v) { ((ManagerPane *)v)->remove_selected(); }
    static void add_cb(Fl_Widget *, void *v) { ((ManagerPane *)v)->add_entries(); }
    static void select_cb(Fl_Widget *, void *v) { ((ManagerPane *)v)->selection_changed(); }

    void selection_changed()
    {
        show_icon();
        update_bezel_buttons();
        show_bezel_preview();
    }

    void show_bezel_preview()
    {
        BusyCursor busy("Loading bezel...");
        const RomEntry *e = selected();
        std::string name = e ? option_unescape(option_get(e->options, "bezel"))
                             : std::string();
        if (name.empty()) {
            bezel_preview_->clear_frame();
            bezel_preview_->label("");
            bezel_preview_->redraw();
            return;
        }
        std::string blob, err;
        if (!get_bezel_blob(address(), name, blob, err) || blob.empty()) {
            bezel_preview_->clear_frame();
            bezel_preview_->label("?");
            bezel_preview_->redraw();
            return;
        }
        PkbzHeader hdr;
        Fl_RGB_Image *img = pkbz_to_image(blob, hdr, bezel_preview_->w() - 4,
                                          bezel_preview_->h() - 4);
        if (!img) {
            bezel_preview_->clear_frame();
            bezel_preview_->label("?");
            bezel_preview_->redraw();
            return;
        }
        bezel_preview_->label("");
        bezel_preview_->set_frame(img, (int)hdr.width, (int)hdr.height);
    }

    void update_bezel_buttons()
    {
        const RomEntry *e = selected();
        std::string bezel = e ? option_get(e->options, "bezel") : std::string();

        if (bezel.empty())
            del_bezel_btn_->hide();
        else
            del_bezel_btn_->show();
        if (e)
            set_bezel_btn_->activate();
        else
            set_bezel_btn_->deactivate();
    }
    static void icon_cb(Fl_Widget *, void *v) { ((ManagerPane *)v)->choose_icon(); }
    static void backend_cb(Fl_Widget *, void *v) { ((ManagerPane *)v)->backend_changed(); }
    static void settings_cb(Fl_Widget *, void *v) { ((ManagerPane *)v)->open_settings(); }
    static void set_bezel_cb(Fl_Widget *, void *v) { ((ManagerPane *)v)->set_bezel(); }
    static void del_bezel_cb(Fl_Widget *, void *v) { ((ManagerPane *)v)->clear_bezel(); }

    const BackendDef &backend() const
    {
        int i = backend_ ? backend_->value() : 0;
        if (i < 0 || i >= BACKEND_COUNT) i = 0;
        return BACKENDS[i];
    }

    void backend_changed()
    {
        const BackendDef &b = backend();
        add_btn_->label(b.add_label);
        add_btn_->tooltip(b.add_tip);
        add_btn_->redraw();
    }

    std::string address() const { return xfer_->address(); }
    std::string dest() const
    {
        return std::string(media_base(media_from_choice(media_->value()))) + "/" + backend().subdir;
    }

    bool wanted(const RomEntry &e) const
    {
        return !e.machine.empty() && e.path[0] != '@';
    }

    const RomEntry *selected() const
    {
        int sel = list_->value();
        if (sel < 1 || (size_t)sel > entries_.size())
            return 0;
        return &entries_[sel - 1];
    }

    void add_entries()
    {
        Fl_File_Chooser chooser(xfer_->last_dir().empty() ? "." : xfer_->last_dir().c_str(),
                                 backend().pattern, Fl_File_Chooser::MULTI, backend().chooser_title);
        chooser.show();
        while (chooser.shown())
            Fl::wait();
        if (!chooser.value(1))
            return;

        std::string options;
        option_set(options, "media", media_name(media_from_choice(media_->value())));
        if (rotate_chk_ && rotate_chk_->value())
            option_set(options, "rotate", "1");

        std::string rejected;
        for (int i = 1; i <= chooser.count(); i++) {
            const char *path = chooser.value(i);
            if (!path)
                continue;
            std::string machine = detect_machine(path);
            bool ok = backend().j2me ? (machine == "J2ME") : (!machine.empty() && machine != "J2ME");
            if (!ok) {
                rejected += std::string("\n  ") + basename_of_path(path);
                continue;
            }
            std::string per_file = options;
            if (backend().j2me) {
                JarMeta meta;
                if (jar_read_meta(path, meta)) {
                    if (!meta.title.empty())
                        option_set(per_file, "title", option_escape(meta.title));
                    if (!meta.icon_png.empty())
                        pending_icons_[dest() + "/" + basename_of_path(path)] = meta.icon_png;
                }
            }
            xfer_->queue_path(path, per_file, dest(), machine);
        }
        xfer_->refresh_queue_view();

        if (!rejected.empty())
            fl_alert("Not sent, because they were not recognised:%s\n\n%s",
                     rejected.c_str(), backend().reject_hint);
    }

    void refresh()
    {
        BusyCursor busy("Loading list...");
        entries_.clear();
        list_->clear();

        std::string err;
        std::string records;
        if (!fetch_rom_list(address(), records, err)) {
            list_->add(("@C1cannot read the device's list: " + err).c_str());
            return;
        }

        std::string line;
        for (size_t i = 0; i <= records.size(); i++) {
            if (i == records.size() || records[i] == '\n') {
                if (!line.empty()) {
                    RomEntry e;
                    if (decode_entry(line, e) && wanted(e))
                        entries_.push_back(e);
                }
                line.clear();
            } else {
                line += records[i];
            }
        }

        for (size_t i = 0; i < entries_.size(); i++) {
            const RomEntry &e = entries_[i];
            std::string media = option_get(e.options, "media");
            if (media.empty())
                media = "?";
            std::string bezel = option_get(e.options, "bezel");
            if (bezel.empty())
                bezel = "Default";
            else
                bezel = option_unescape(bezel);
            std::string row = strip_extension(basename_of_path(e.path)) + "\t"
                            + media + "\t" + e.machine + "\t" + bezel + "\t" + e.path;
            list_->add(row.c_str());
        }
        if (entries_.empty())
            list_->add(spec_.empty_text);
    }

    void remove_selected()
    {
        const RomEntry *e = selected();
        if (!e) { fl_alert("Select an entry to delete first."); return; }
        std::string name = strip_extension(basename_of_path(e->path));
        std::string path = e->path;
        if (fl_choice("Delete \"%s\" from the device?\n%s",
                      "Cancel", "Delete", 0, name.c_str(), path.c_str()) != 1)
            return;

        std::string err;
        if (!delete_rom(address(), path, err))
            fl_alert("Could not delete %s:\n%s", name.c_str(), err.c_str());
        refresh();
    }

    void open_settings()
    {
        std::string a = address();
        if (a.empty()) {
            fl_alert("Set the Zaurus address first.");
            return;
        }
        g_emu_settings.set_address(a);
        g_emu_settings.set_xfer(xfer_);
        g_emu_settings.show();
    }

    void set_bezel()
    {
        const RomEntry *e = selected();
        if (!e) { fl_alert("Select a game first."); return; }

        std::string current = option_unescape(option_get(e->options, "bezel"));
        std::string picked, err;
        std::string path = e->path;
        if (!BezelChooser::run(address(), current, picked))
            return;
        if (!set_rom_option(address(), path, "bezel", picked, err)) {
            fl_alert("Could not set the bezel:\n%s", err.c_str());
            return;
        }
        refresh();
    }

    void clear_bezel()
    {
        const RomEntry *e = selected();
        if (!e) return;
        std::string name = strip_extension(basename_of_path(e->path));
        std::string path = e->path;
        if (fl_choice("Remove the bezel from \"%s\"?\n\n"
                      "It will fall back to the backend or global bezel.",
                      "Cancel", "Remove", 0, name.c_str()) != 1)
            return;
        std::string err;
        if (!set_rom_option(address(), path, "bezel", "", err)) {
            fl_alert("Could not remove the bezel:\n%s", err.c_str());
            return;
        }
        refresh();
    }

    void show_icon()
    {
        BusyCursor busy("Loading icon...");
        if (!preview_)
            return;
        const RomEntry *e = selected();
        preview_->image(0);
        delete preview_image_;
        preview_image_ = 0;
        if (!e) { preview_->redraw(); return; }

        std::string png, err;
        if (get_rom_icon(address(), e->path, png, err) && !png.empty()) {
            const char *tmp = "/tmp/.piko-sync-icon-preview.png";
            FILE *f = fopen(tmp, "wb");
            if (f) {
                bool wrote = fwrite(png.data(), 1, png.size(), f) == png.size();
                fclose(f);
                if (wrote) {
                    preview_image_ = new Fl_PNG_Image(tmp);
                    if (preview_image_->w() > 0)
                        preview_->image(preview_image_);
                }
                remove(tmp);
            }
        }
        preview_->redraw();
    }

    void choose_icon()
    {
        const RomEntry *e = selected();
        if (!e) { fl_alert("Select an entry first."); return; }

        Fl_File_Chooser chooser(".", "PNG icons (*.png)",
                                 Fl_File_Chooser::SINGLE, "Choose an icon");
        chooser.show();
        while (chooser.shown())
            Fl::wait();
        if (!chooser.value())
            return;

        FILE *f = fopen(chooser.value(), "rb");
        if (!f) { fl_alert("Cannot read %s", chooser.value()); return; }
        std::string png;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            png.append(buf, n);
        fclose(f);

        if (png.size() < 8 || png.compare(1, 3, "PNG") != 0) {
            fl_alert("%s is not a PNG image.", chooser.value());
            return;
        }
        if (png.size() > MAX_CHUNK) {
            fl_alert("That icon is %d KB; keep it under %d KB.",
                     (int)(png.size() / 1024), (int)(MAX_CHUNK / 1024));
            return;
        }

        std::string err;
        std::string rom = e->path;
        if (!set_rom_icon(address(), rom, png, err)) {
            fl_alert("Could not set the icon:\n%s", err.c_str());
            return;
        }
        refresh();
        show_icon();
    }

    TransferPane *xfer_;
    ManagerSpec spec_;
    Fl_Choice *media_;
    Fl_Button *add_btn_;
    Fl_Button *refresh_btn_;
    Fl_Button *delete_btn_;
    Fl_Button *icon_btn_;
    Fl_Check_Button *rotate_chk_;
    Fl_Choice *backend_;
    Fl_Button *settings_btn_;
    Fl_Button *set_bezel_btn_;
    Fl_Button *del_bezel_btn_;
    Fl_Hold_Browser *list_;
    Fl_Box *preview_;
    BezelPreviewBox *bezel_preview_;
    Fl_PNG_Image *preview_image_;
    std::vector<RomEntry> entries_;
    std::map<std::string, std::string> pending_icons_;
};

class TransferFilesPane {
public:
    TransferFilesPane(Fl_Group *tab, TransferPane *xfer, int X, int Y, int W, int H,
                      const Settings &cfg)
        : xfer_(xfer)
    {
        int m = 10;
        int y = Y + m;
        (void)H;
        tab->begin();

        dest_ = new Fl_Input(X + m + 90, y, W - 2 * m - 90, 24, "Folder:");
        dest_->align(FL_ALIGN_LEFT);
        dest_->value(cfg.get("transfer.dest_dir", DEFAULT_DEST_DIR).c_str());
        dest_->tooltip("Absolute path on the Zaurus where incoming files are written");
        dest_->callback(dest_cb, this);
        dest_->when(FL_WHEN_CHANGED);

        y += 32;

        add_btn_ = new Fl_Button(X + m, y, 110, 24, "Add Files...");
        add_btn_->callback(add_cb, this);
        add_btn_->tooltip("Send any file to the folder above, without registering it");

        retry_btn_ = new Fl_Button(X + m + 116, y, 110, 24, "Retry failed");
        retry_btn_->callback(retry_cb, this);

        tab->end();

        xfer_->set_default_dest(dest_->value() ? dest_->value() : "");
    }

    void store_settings(Settings &cfg) const
    {
        cfg.set("transfer.dest_dir", dest_->value() ? dest_->value() : "");
    }

private:
    static void dest_cb(Fl_Widget *, void *v)
    {
        TransferFilesPane *p = (TransferFilesPane *)v;
        p->xfer_->set_default_dest(p->dest_->value() ? p->dest_->value() : "");
    }
    static void add_cb(Fl_Widget *, void *v)
    {
        TransferFilesPane *p = (TransferFilesPane *)v;
        p->xfer_->set_default_dest(p->dest_->value() ? p->dest_->value() : "");
        p->xfer_->choose_and_queue("All files (*)", "Add files to send");
    }
    static void retry_cb(Fl_Widget *, void *v)
    {
        ((TransferFilesPane *)v)->xfer_->retry_failed();
    }

    TransferPane *xfer_;
    Fl_Input *dest_;
    Fl_Button *add_btn_;
    Fl_Button *retry_btn_;
};

void SettingsStore::save_now()
{
    if (client_)
        client_->store_settings(cfg_);
    if (roms_)
        roms_->store_settings(cfg_);
    if (runner_)
        runner_->store_settings(cfg_);
    for (size_t i = 0; i < managers_.size(); i++)
        managers_[i]->store_settings(cfg_);
    if (files_)
        files_->store_settings(cfg_);
    cfg_.save();
}

static const int WIN_W = 720;
static const int WIN_H = 520;
static const int HEADER_H = 32;
static const int STATUS_H = 26;
static const int DOCK_OPEN_H = 170;

struct Layout {
    Fl_Tabs *tabs;
    TransferPane *xfer;
    std::vector<Fl_Group *> pages;
};

static Layout g_layout;

static void apply_layout()
{
    int dock_h = g_layout.xfer->expanded() ? DOCK_OPEN_H : STATUS_H;
    int tabs_h = WIN_H - HEADER_H - dock_h;

    g_layout.tabs->resize(0, HEADER_H, WIN_W, tabs_h);
    for (size_t i = 0; i < g_layout.pages.size(); i++)
        g_layout.pages[i]->resize(0, HEADER_H + 24, WIN_W, tabs_h - 24);

    g_layout.xfer->dock_relayout(0, HEADER_H + tabs_h, WIN_W, dock_h);
    g_layout.tabs->redraw();
}

static void relayout_cb(void *) { apply_layout(); }

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    SettingsStore settings;

    Fl_Double_Window win(WIN_W, WIN_H, "Piko Sync");
    win.begin();

    int tabs_h = WIN_H - HEADER_H - DOCK_OPEN_H;
    int page_y = HEADER_H + 24;
    int page_h = tabs_h - 24;

    TransferPane xfer(0, 0, WIN_W, HEADER_H,
                      0, HEADER_H + tabs_h, WIN_W, DOCK_OPEN_H,
                      settings.cfg());

    Fl_Tabs tabs(0, HEADER_H, WIN_W, tabs_h);
    tabs.begin();

    ManagerSpec emu_spec;
    emu_spec.dest_label = "Stored on:";
    emu_spec.empty_text = "nothing registered on the device yet";

    Fl_Group emu_tab(0, page_y, WIN_W, page_h, "Emulation");
    ManagerPane emulation(&emu_tab, &xfer, 0, page_y, WIN_W, page_h, settings.cfg(), emu_spec);
    emu_tab.end();

    Fl_Group files_tab(0, page_y, WIN_W, page_h, "Transfer files");
    TransferFilesPane files(&files_tab, &xfer, 0, page_y, WIN_W, page_h, settings.cfg());
    files_tab.end();

    Fl_Group deploy_tab(0, page_y, WIN_W, page_h, "Build && Deploy");
    BuildRunner runner(&deploy_tab, 0, page_y, WIN_W, page_h, settings.cfg(), &settings);
    deploy_tab.end();

    tabs.end();
    tabs.resizable(emu_tab);

    win.end();

    g_layout.tabs = &tabs;
    g_layout.xfer = &xfer;
    g_layout.pages.push_back(&emu_tab);
    g_layout.pages.push_back(&files_tab);
    g_layout.pages.push_back(&deploy_tab);
    xfer.on_relayout(relayout_cb, 0);

    settings.bind(&xfer, 0, &runner);
    settings.add_manager(&emulation);
    settings.add_files_pane(&files);

    static Fl_Pixmap icon_pixmap(piko_sync_icon_xpm);
    static Fl_RGB_Image icon_img(&icon_pixmap);
    win.icon(&icon_img);

    apply_layout();
    win.show(argc, argv);

    int rc = Fl::run();

    settings.save_now();
    return rc;
}
