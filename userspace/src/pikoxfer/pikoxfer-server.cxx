/*
 * pikoxfer-server -- runs on the Zaurus. Listens for pikoxfer-client
 * connections and writes incoming files into /mnt/card/Transfers. FLTK
 * 1.3, X11, built against the staged libfltk (see pikostore.cxx / piko's
 * docs/HOWTO-FLTK.md) -- same toolkit, same target, same conventions.
 *
 * DESIGN NOTES THAT ARE NOT OBVIOUS
 *
 * Why Fl::add_fd and not a thread, again. Same reasoning as pikostore:
 * FLTK 1.3 is not thread-safe without lock()/unlock() discipline, and
 * this device is a 400MHz PXA255. The listening socket and every
 * accepted connection are watched by the event loop; no thread is ever
 * created. One read() per FL_READ callback (not a drain loop) matches
 * pikostore's on_out/on_prog exactly -- level-triggered select() means
 * FLTK calls back again immediately if more is still buffered.
 *
 * Why resume state is in memory, not on disk. See transfer_state.h's
 * header comment: a .pikoxfer-part file's size on disk says how far a
 * transfer got, never how far it is supposed to go, so "what total size
 * was promised" only lives in this process's TransferMap for as long as
 * it keeps running. That is an intentional, documented scope limit, not
 * an oversight -- see the pikoxfer README.
 *
 * Why a CRC mismatch resets the part file to empty. The alternative --
 * leaving it half-verified -- would make every future resume offer this
 * same (name, size) key report "nothing left to send" forever, since the
 * byte count on disk never changes. Truncating back to 0 on a mismatch
 * is what actually lets a subsequent full resend succeed.
 */

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Progress.H>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "protocol.h"
#include "transfer_state.h"
#include "transfer_queue.h"
#include "transfer_table.h"
#include "net_io.h"

using namespace pikoxfer;

static const char *TRANSFERS_DIR = "/mnt/card/Transfers";
static const char *PART_SUFFIX = ".pikoxfer-part";

class ServerApp;

/* ---------------------------------------------------------------------- *
 * One accepted connection = one file, start to finish or to a drop.       *
 * ---------------------------------------------------------------------- */

class Connection {
public:
    Connection(int fd, ServerApp *app);
    ~Connection();

    /* Public so ServerApp can force-close every live connection when it
     * is torn down (window close / process exit cleanup). Everything
     * else about a Connection is internal to its own state machine. */
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

    bool send(uint32_t type, const std::string &payload);
    void fail(const std::string &reason); /* send ERROR (best effort), then close */
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
};

/* ---------------------------------------------------------------------- *
 * The window: address/status text, aggregate bar, transfer table.         *
 * ---------------------------------------------------------------------- */

class ServerApp {
public:
    ServerApp(int X, int Y, int W, int H);
    ~ServerApp();

    Fl_Widget *resizable_widget() { return table_; }

    TransferQueue &queue() { return queue_; }
    TransferMap &transfer_map() { return transfer_map_; }
    const std::vector<std::string> &complete_names() const { return complete_names_; }
    void note_complete_name(const std::string &name) { complete_names_.push_back(name); }

    void sync_table()
    {
        table_->sync();
        aggregate_bar_->value(static_cast<float>(queue_.aggregate_percent()));
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "%d%%", static_cast<int>(queue_.aggregate_percent() + 0.5));
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
    Fl_Progress *aggregate_bar_;
    TransferTable *table_;
    TransferQueue queue_;
    TransferMap transfer_map_;
    std::vector<std::string> complete_names_;
    int listen_fd_;
    std::vector<Connection *> connections_;
};

/* ---------------------------------------------------------------------- *
 * Connection                                                               *
 * ---------------------------------------------------------------------- */

Connection::Connection(int fd, ServerApp *app)
    : app_(app), fd_(fd), phase_(WAIT_HELLO),
      total_size_(0), next_offset_(0), already_fully_done_(false),
      row_(-1), part_fd_(-1)
{
    set_nonblock(fd_);
    Fl::add_fd(fd_, FL_READ, read_cb, this);
}

Connection::~Connection()
{
    /* Normally a no-op: close_connection() has already closed both fds
     * and set them to -1 by the time the deferred delete (see below)
     * runs. Guarded anyway in case that ever changes. */
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
    send_frame_blocking(fd_, MSG_ERROR, encode(em)); /* best effort */
    close_connection();
}

void Connection::close_connection()
{
    if (phase_ == CLOSED)
        return;
    phase_ = CLOSED;

    /* A connection that drops mid-transfer is not an error -- the part
     * file is exactly where it was left, and the client is expected to
     * reconnect and resume. Only mark the row RECONNECTING if it was not
     * already given a terminal status (DONE/ERROR) by handle_complete(). */
    if (row_ >= 0) {
        const TransferRow &r = app_->queue().row(row_);
        if (r.status == XFER_TRANSFERRING)
            app_->queue().set_status(row_, XFER_RECONNECTING);
        app_->sync_table();
    }

    if (fd_ >= 0)      { Fl::remove_fd(fd_); close(fd_); fd_ = -1; }
    if (part_fd_ >= 0) { close(part_fd_); part_fd_ = -1; }

    app_->forget_connection(this);

    /* `this` cannot be deleted here: close_connection() is frequently
     * still on the call stack of one of this object's own member
     * functions (fail() -> close_connection(), called from deep inside
     * handle_frame()), and on_read()'s loop reads phase_ again right
     * after handle_frame() returns. Deleting synchronously would make
     * that a use-after-free. Deferring to the next event-loop tick is
     * the same trick Fl::delete_widget() uses for the identical reason. */
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
            close_connection(); /* stream is untrustworthy; no ERROR reply */
            return;
        }
        handle_frame(type, payload);
        if (phase_ == CLOSED)
            return; /* `this` may be dangling past here if we didn't return */
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
        if (type != MSG_FILE_OFFER) { fail("expected FILE_OFFER"); return; }
        handle_offer(payload);
        return;
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

    original_name_ = fo.name;
    total_size_ = fo.total_size;

    TransferKey key(fo.name, fo.total_size);
    TransferMap::const_iterator it = app_->transfer_map().find(key);
    already_fully_done_ = (it != app_->transfer_map().end() && it->second.complete);

    OfferDecision d = decide_offer(app_->transfer_map(), fo.name, fo.total_size,
                                    app_->complete_names());
    final_name_ = d.final_name;
    next_offset_ = d.resume_offset;

    if (next_offset_ < total_size_) {
        std::string part_path = std::string(TRANSFERS_DIR) + "/" + final_name_ + PART_SUFFIX;
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

    row_ = app_->queue().add(final_name_, total_size_);
    app_->queue().set_status(row_, XFER_TRANSFERRING);
    app_->queue().set_progress(row_, next_offset_);
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

    ssize_t w = pwrite(part_fd_, dc.data.data(), dc.data.size(),
                        static_cast<off_t>(dc.offset));
    if (w < 0 || static_cast<size_t>(w) != dc.data.size()) {
        char msg[128];
        snprintf(msg, sizeof(msg), "write failed: %s", strerror(errno));
        fail(msg);
        return;
    }

    next_offset_ += dc.data.size();
    app_->transfer_map()[TransferKey(original_name_, total_size_)].bytes_on_disk = next_offset_;

    app_->queue().set_progress(row_, next_offset_);
    app_->sync_table();

    ChunkAckMsg ack; ack.bytes_written = next_offset_;
    send(MSG_CHUNK_ACK, encode(ack));
}

void Connection::handle_complete(const std::string &payload)
{
    FileCompleteMsg fc;
    if (!decode_file_complete(payload, fc)) { fail("malformed FILE_COMPLETE"); return; }

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

    /* Recompute the CRC from what is actually on disk, independent of
     * how many reconnects it took to get there -- see the file header. */
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
        /* Reset so the NEXT offer for this (name, size) resumes at 0
         * instead of reporting "nothing left to send" forever. */
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

    std::string part_path = std::string(TRANSFERS_DIR) + "/" + final_name_ + PART_SUFFIX;
    std::string final_path = std::string(TRANSFERS_DIR) + "/" + final_name_;
    close(part_fd_);
    part_fd_ = -1;

    if (rename(part_path.c_str(), final_path.c_str()) != 0) {
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

/* ---------------------------------------------------------------------- *
 * ServerApp                                                                *
 * ---------------------------------------------------------------------- */

ServerApp::ServerApp(int X, int Y, int W, int H)
    : listen_fd_(-1)
{
    int m = 10;
    address_box_ = new Fl_Box(X + m, Y + m, W - 2 * m, 40);
    address_box_->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    address_box_->label("starting...");

    aggregate_bar_ = new Fl_Progress(X + m, Y + m + 44, W - 2 * m, 20);
    aggregate_bar_->minimum(0);
    aggregate_bar_->maximum(100);
    aggregate_bar_->value(0);
    aggregate_bar_->color(FL_BACKGROUND_COLOR);
    aggregate_bar_->selection_color(FL_BLUE);
    aggregate_bar_->label("0%");

    table_ = new TransferTable(X + m, Y + m + 72, W - 2 * m, H - m - 72 - m);
    table_->queue(&queue_);

    /* mkdir best-effort: /mnt/card is expected to already be the SD
     * mount point (same assumption pikostore makes for SD_CARD_DIR). */
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
    /* Each call removes exactly itself from connections_ (via
     * forget_connection), so repeatedly closing the last entry drains
     * the vector without invalidating the loop. */
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
            continue; /* a leftover partial from a previous run/session */

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
            return; /* EAGAIN: nothing more pending right now */
        connections_.push_back(new Connection(fd, this));
    }
}

/* ---------------------------------------------------------------------- *
 * main                                                                     *
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    Fl_Double_Window win(640, 480, "pikoxfer");
    win.begin();
    ServerApp app(0, 0, 640, 480);
    win.end();
    win.resizable(app.resizable_widget());
    win.show(argc, argv);

    return Fl::run();
}
