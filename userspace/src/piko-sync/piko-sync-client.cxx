/*
 * piko-sync-client -- runs on the host. Two unrelated jobs share one
 * window because they are both "things you do while iterating against
 * the one spare Zaurus": queuing files to a running piko-sync-server, and
 * triggering the existing tools/build-and-deploy.sh. FLTK 1.3, built
 * against the HOST's own FLTK (see the piko-sync Makefile), not the
 * cross-compiled/staged one piko-sync-server uses.
 *
 * DESIGN NOTES THAT ARE NOT OBVIOUS
 *
 * Stop-and-wait, not pipelined, chunk sending. Each DATA_CHUNK waits for
 * its CHUNK_ACK before the next one is sent -- simpler to reason about
 * than tracking an in-flight window, at some throughput cost. Given the
 * receiver is a 400MHz PXA255 on a flaky link, the bottleneck is rarely
 * round-trip latency anyway. A future revision chasing more throughput
 * would want a small pipelining window instead.
 *
 * Transport drop vs. protocol-level reject are handled differently on
 * purpose. A connection that just dies (WiFi drop, server restart) marks
 * the row RECONNECTING and retries with backoff automatically, because
 * that is the exact failure this app exists to be resilient against. A
 * FILE_OFFER_ACK/FILE_COMPLETE_ACK that explicitly says no (bad name,
 * checksum mismatch) marks the row ERROR and does NOT auto-retry --
 * retrying a rejection forever would just hammer the link for no
 * reason. The "Retry failed" button re-queues those by hand.
 *
 * Why "Build & Deploy" shells out to the existing script unmodified.
 * tools/build-and-deploy.sh is the ROUTINE path for updating the running
 * kernel on the project's one spare board (AGENTS.md). Reimplementing
 * any of its steps here would be a second copy of logic that already
 * works and is trusted; this tab is a GUI front end for it, nothing
 * more. The step progress bar is driven by matching the script's known
 * `echo "==>"` milestone lines -- a soft coupling, same as
 * ssh-payload.sh's own documented one, kept in one place (MILESTONES
 * below) so it is obvious what to update if the script's wording changes.
 */

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
#include <unistd.h>

#include <string>
#include <vector>

#include "protocol.h"
#include "transfer_queue.h"
#include "transfer_table.h"
#include "net_io.h"

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

/* Real local interface names for the Build & Deploy tab's Adapter
 * dropdown, in first-seen order, each listed once regardless of how many
 * addresses it has. getifaddrs() returns one entry per address (IPv4,
 * IPv6, link-layer, ...), not one per interface, hence the dedup. */
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

class ClientApp;

/* ---------------------------------------------------------------------- *
 * One file the user queued, persistent for the life of the app (never    *
 * erased -- only its row's status changes -- so an int index into        *
 * ClientApp::files_ stays valid across FileSend attempts and retries).    *
 * ---------------------------------------------------------------------- */

struct QueuedFile {
    std::string path;
    std::string name;
    uint64_t total_size;
    uint32_t crc32;
    int row;
    double retry_delay; /* seconds, doubles on each transport-level drop */

    QueuedFile() : total_size(0), crc32(0), row(-1), retry_delay(1.0) {}
};

/* ---------------------------------------------------------------------- *
 * One connection attempt for one queued file.                             *
 * ---------------------------------------------------------------------- */

class FileSend {
public:
    FileSend(ClientApp *app, int file_index);
    ~FileSend();

    void abandon(); /* app teardown only: stop watching fd, no callbacks */

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

/* ---------------------------------------------------------------------- *
 * ClientApp: the Transfer tab's state (queue, files, in-flight sends).    *
 * ---------------------------------------------------------------------- */

struct RetryContext {
    ClientApp *app;
    int file_index;
};

class BuildRunner; /* defined below, owns the Build & Deploy tab */

class ClientApp {
public:
    ClientApp(Fl_Group *transfer_tab, int X, int Y, int W, int H);
    ~ClientApp();

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

    static void retry_failed_cb(Fl_Widget *, void *v) { static_cast<ClientApp *>(v)->do_retry_failed(); }
    void do_retry_failed();

    Fl_Input *address_;
    Fl_Progress *aggregate_bar_;
    TransferTable *table_;
    TransferQueue queue_;
    std::vector<QueuedFile> files_;
    std::vector<FileSend *> active_;
    std::string last_dir_;
};

/* ---------------------------------------------------------------------- *
 * FileSend                                                                 *
 * ---------------------------------------------------------------------- */

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

    /* Deferred for the same reason as piko-sync-server's Connection: this
     * call is frequently still on the stack of one of this object's own
     * member functions. */
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

/* ---------------------------------------------------------------------- *
 * ClientApp                                                                *
 * ---------------------------------------------------------------------- */

ClientApp::ClientApp(Fl_Group *tab, int X, int Y, int W, int H)
{
    (void)tab;
    int m = 10;

    address_ = new Fl_Input(X + m + 90, Y + m, 160, 24, "Zaurus:");
    address_->align(FL_ALIGN_LEFT);
    address_->value(DEFAULT_ADDRESS);

    Fl_Button *add_btn = new Fl_Button(X + m + 260, Y + m, 100, 24, "Add Files...");
    add_btn->callback(add_files_cb, this);

    Fl_Button *retry_btn = new Fl_Button(X + m + 366, Y + m, 110, 24, "Retry failed");
    retry_btn->callback(retry_failed_cb, this);

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
        qf.crc32 = compute_file_crc32(path); /* reads the whole file once, synchronously --
                                               * a very large file will visibly pause the UI
                                               * here; acceptable for this device's scale. */
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

/* ---------------------------------------------------------------------- *
 * Build & Deploy tab: a GUI front end for tools/build-and-deploy.sh.       *
 * ---------------------------------------------------------------------- */

/* `phase` drives status_label_, the one-line "what's happening right now"
 * above the percent bar -- distinct from the bar's own "N%"/"done"/
 * "FAILED" label. Data-driven for the same reason MILESTONES itself is:
 * adding or reordering a milestone should not require hunting down a
 * separate if/else chain to keep the phase text in sync. */
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

class BuildRunner {
public:
    BuildRunner(Fl_Group *tab, int X, int Y, int W, int H);

private:
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
    std::string toolchain_bin_dir_; /* empty = inherit build-and-deploy.sh's own default */
    std::string jobs_; /* empty = inherit build-and-deploy.sh's own nproc-based default */

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
};

BuildRunner::BuildRunner(Fl_Group *tab, int X, int Y, int W, int H)
    : pid_(-1), out_fd_(-1), milestone_idx_(0), running_(false)
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
            adapter_->value(0); /* default to the first one, as asked */
    }
    target_ = new Fl_Input(X + m + 260, y, 160, 22, "Target:");
    target_->align(FL_ALIGN_LEFT);
    /* Prefilled with a real user@host, not just an IP: a bare IP typed
     * here (reasonably, since that's all the Transfer tab's address
     * field wants) makes ssh fall back to the LOCAL shell user's name
     * as the remote login -- "Permission denied" against a device that
     * only accepts root, with no hint that a username was ever the
     * problem. build_argv() below also auto-prepends root@ if the
     * field is edited down to a bare host, as a second line of defense. */
    std::string default_target = std::string("root@") + DEFAULT_ADDRESS;
    target_->value(default_target.c_str()); /* Fl_Input copies it internally */
    y += 28;

    kernel_only_ = new Fl_Check_Button(X + m, y, 120, 20, "kernel-only");
    force_kernel_src_ = new Fl_Check_Button(X + m + 120, y, 140, 20, "force-kernel-src");
    skip_userspace_ = new Fl_Check_Button(X + m + 260, y, 130, 20, "skip-userspace");
    y += 22;
    skip_st_ = new Fl_Check_Button(X + m, y, 90, 20, "skip-st");
    skip_x11_ = new Fl_Check_Button(X + m + 120, y, 90, 20, "skip-x11");
    build_only_ = new Fl_Check_Button(X + m + 260, y, 100, 20, "build-only");
    y += 22;
    /* Checked by default: matches --create-backup-files' own off-by-
     * default (every file it touches ends up duplicated on the ~68 MiB
     * root jffs2 if it's on -- see build-and-deploy.sh's flag comment).
     * Unchecking is what actually passes --create-backup-files through. */
    no_backup_ = new Fl_Check_Button(X + m, y, 110, 20, "no-backup");
    no_backup_->value(1);
    destination_ = new Fl_Choice(X + m + 190, y - 1, 90, 22, "Staging:");
    destination_->align(FL_ALIGN_LEFT);
    /* SD first (default): most deploy destinations live on the ~68 MiB
     * NAND root, so staging there too risks the exact ENOSPC problem
     * chunked-deploy.sh's REMOTE_STAGE fix solved, just for deploy
     * instead of plain transfer -- see piko-sync-deploy's own --staging
     * default and piko-sync-server.cxx's resolve_staging_dir(). CF is
     * listed but disabled: real CF mounting does not exist anywhere in
     * this ROM yet (no mdev rule, no confirmed driver), so it would be a
     * selectable option that silently can't work. */
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

    /* What this run is doing right now -- "Building.../Deploying..." --
     * distinct from bar_'s own label, which is a percentage (or "done"/
     * "FAILED (exit N)" once it stops), not a description. */
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
}

std::string BuildRunner::script_path() const
{
    const char *root = getenv("PIKO_SYNC_REPO_ROOT");
    std::string base = root && *root ? root : ".";
    return base + "/tools/build-and-deploy.sh";
}

void BuildRunner::build_argv(std::vector<std::string> &args) const
{
    args.push_back(script_path());
    /* Fl_Choice::text() (no args) is the selected item's label -- unlike
     * the Fl_Input this used to be, value() here is the selected INDEX,
     * not a string. No selection (empty interface list) means no
     * --adapter, same as leaving the old text field blank. */
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
        /* A bare host (no user@) is a completely reasonable thing to
         * type here -- it's what the Transfer tab's address field
         * wants -- but ssh would silently try the LOCAL shell user's
         * name against a device that only accepts root, and fail with
         * a "Permission denied" that gives no hint a username was ever
         * the issue. Fill it in rather than let that surprise happen. */
        if (t.find('@') == std::string::npos)
            t = "root@" + t;
        args.push_back(t);
    }
}

namespace {

/* Directory picker for the Toolchain field below -- a plain Fl_Input, not
 * bundled into a context struct, since that's the only widget this
 * callback needs to update. */
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
        return; /* cancelled */

    std::string picked = chooser.value(1);
    /* Soft hint, not a hard block -- build-and-deploy.sh's own check is
     * the one that actually matters and will fail loudly on its own if
     * this is wrong; this just saves a build attempt's worth of time. */
    std::string probe = picked + "/arm-unknown-linux-uclibcgnueabi-gcc";
    struct stat st;
    if (stat(probe.c_str(), &st) != 0) {
        fl_alert("Note: arm-unknown-linux-uclibcgnueabi-gcc was not found directly in:\n%s\n\n"
                 "build-and-deploy.sh may not find the cross compiler there.",
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

} /* anonymous namespace */

void BuildRunner::do_settings()
{
    Fl_Double_Window dlg(480, 210, "Build settings");
    dlg.begin();

    Fl_Input toolchain_input(90, 15, 300, 24, "Toolchain:");
    toolchain_input.align(FL_ALIGN_LEFT);
    toolchain_input.value(toolchain_bin_dir_.c_str());

    Fl_Button browse_btn(400, 15, 70, 24, "Browse...");
    browse_btn.callback(browse_toolchain_cb, &toolchain_input);

    Fl_Box toolchain_hint(10, 45, 460, 40,
                "Directory containing the arm-*-gcc cross compiler\n"
                "(TOOLCHAIN_BIN_DIR). Leave blank to use build-and-deploy.sh's\n"
                "own default (<repo>/toolchain/x-tools/.../bin).");
    toolchain_hint.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    toolchain_hint.labelsize(11);

    Fl_Input jobs_input(90, 95, 60, 24, "Jobs:");
    jobs_input.align(FL_ALIGN_LEFT);
    jobs_input.value(jobs_.c_str());

    Fl_Box jobs_hint(10, 125, 460, 40,
                "make -jN for the kernel build, and forwarded to\n"
                "tools/build-userspace.sh's own JOBS. Leave blank to use\n"
                "nproc (all detected cores).");
    jobs_hint.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    jobs_hint.labelsize(11);

    bool ok = false;
    OkCancelCtx ctx;
    ctx.dlg = &dlg;
    ctx.ok = &ok;

    Fl_Button ok_btn(290, 170, 80, 26, "OK");
    ok_btn.callback(settings_ok_cb, &ctx);
    Fl_Button cancel_btn(380, 170, 80, 26, "Cancel");
    cancel_btn.callback(settings_cancel_cb, &ctx);

    dlg.end();
    dlg.set_modal();
    dlg.show();
    while (dlg.shown())
        Fl::wait();

    if (!ok)
        return;

    toolchain_bin_dir_ = toolchain_input.value() ? toolchain_input.value() : "";

    std::string jobs_text = jobs_input.value() ? jobs_input.value() : "";
    if (!jobs_text.empty()) {
        char *end = 0;
        long n = strtol(jobs_text.c_str(), &end, 10);
        /* Soft warning, same call as the Toolchain field above: the
         * real check is `make -jN` itself, which will misbehave on its
         * own terms regardless; this just catches an obvious typo
         * before a build attempt is spent on it. */
        if (n <= 0 || !end || *end != '\0')
            fl_alert("Note: \"%s\" doesn't look like a positive number.\n"
                      "make -j will be passed this value as-is.", jobs_text.c_str());
    }
    jobs_ = jobs_text;
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
        fl_alert("Cannot find %s.\nRun piko-sync-client from the piko repo root,\n"
                 "or set PIKO_SYNC_REPO_ROOT.", args[0].c_str());
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

        /* Only set if the user picked one via Settings -- otherwise
         * build-and-deploy.sh's own ${TOOLCHAIN_BIN_DIR:-default} /
         * ${JOBS:-nproc} take over, unchanged. setenv() here only
         * affects this forked child's environment, never the running
         * GUI's own. */
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
    /* Monotonic on purpose: only ever match a milestone AFTER the last
     * one matched, so the bar never regresses even if a substring
     * happens to reappear later in the script's own sub-tool output. */
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

/* ---------------------------------------------------------------------- *
 * main                                                                     *
 * ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    Fl_Double_Window win(720, 520, "Piko Sync");
    win.begin();

    Fl_Tabs tabs(0, 0, 720, 520);
    tabs.begin();

    Fl_Group transfer_tab(0, 24, 720, 496, "Transfer");
    ClientApp client(&transfer_tab, 0, 24, 720, 496);
    transfer_tab.end();

    Fl_Group deploy_tab(0, 24, 720, 496, "Build && Deploy");
    BuildRunner runner(&deploy_tab, 0, 24, 720, 496);
    deploy_tab.end();

    tabs.end();
    tabs.resizable(transfer_tab);

    win.end();
    win.resizable(tabs);
    win.show(argc, argv);

    return Fl::run();
}
