/*
 * transfer_queue.h -- the row list behind a transfer table (server:
 * incoming files, client: outgoing files) plus the aggregate progress
 * across all of them. Kept apart from the Fl_Table that renders it
 * (transfer_table.h/.cxx) so the bookkeeping is exercised by
 * tests/protocol-test.cxx without FLTK, same split as the rest of this
 * app -- see protocol.h and transfer_state.h.
 */

#ifndef PIKOXFER_TRANSFER_QUEUE_H
#define PIKOXFER_TRANSFER_QUEUE_H

#include <stdint.h>

#include <string>
#include <vector>

namespace pikoxfer {

enum TransferStatus {
    XFER_QUEUED,
    XFER_TRANSFERRING,
    XFER_RECONNECTING,
    XFER_DONE,
    XFER_ERROR
};

struct TransferRow {
    std::string name;
    TransferStatus status;
    uint64_t bytes_done;
    uint64_t bytes_total;
    std::string detail; /* error reason, or a short status word */

    TransferRow() : status(XFER_QUEUED), bytes_done(0), bytes_total(0) {}
};

class TransferQueue {
public:
    /* Returns the new row's index, used afterwards to address it. */
    int add(const std::string &name, uint64_t bytes_total)
    {
        TransferRow r;
        r.name = name;
        r.bytes_total = bytes_total;
        rows_.push_back(r);
        return static_cast<int>(rows_.size()) - 1;
    }

    /* Like add(), but reuses an existing row for the same name instead of
     * appending a duplicate. A dropped connection mid-transfer (expected
     * on this project's known-flaky link -- pikodeploy alone retries a
     * single file up to 30 times) re-offers the identical name on
     * reconnect; without this, every attempt left its own permanent row
     * behind, so a file that needed 3 tries showed up 3 times and its
     * size was counted 3 times in aggregate_percent()'s denominator --
     * the aggregate bar never reached 100% and read lower than real
     * progress. */
    int find_or_add(const std::string &name, uint64_t bytes_total)
    {
        for (size_t i = 0; i < rows_.size(); i++) {
            if (rows_[i].name == name) {
                rows_[i].bytes_total = bytes_total;
                rows_[i].bytes_done = 0;
                rows_[i].status = XFER_QUEUED;
                rows_[i].detail.clear();
                return static_cast<int>(i);
            }
        }
        return add(name, bytes_total);
    }

    int count() const { return static_cast<int>(rows_.size()); }
    const TransferRow &row(int i) const { return rows_[i]; }

    void set_status(int i, TransferStatus s, const std::string &detail = std::string())
    {
        rows_[i].status = s;
        rows_[i].detail = detail;
    }

    void set_progress(int i, uint64_t done)
    {
        rows_[i].bytes_done = done > rows_[i].bytes_total ? rows_[i].bytes_total : done;
    }

    /* 0-100 across every row's bytes_total. A row with an unknown total
     * (0, e.g. queued before its size was read) is excluded from both
     * the numerator and denominator -- otherwise one unsized row would
     * either divide by itself or silently drag the bar towards 0%. */
    double aggregate_percent() const
    {
        uint64_t done = 0, total = 0;
        for (size_t i = 0; i < rows_.size(); i++) {
            if (rows_[i].bytes_total == 0)
                continue;
            done += rows_[i].bytes_done;
            total += rows_[i].bytes_total;
        }
        if (total == 0)
            return 0.0;
        double pct = 100.0 * static_cast<double>(done) / static_cast<double>(total);
        if (pct < 0.0)   pct = 0.0;
        if (pct > 100.0) pct = 100.0;
        return pct;
    }

private:
    std::vector<TransferRow> rows_;
};

/* Deploy-wide progress, spanning every file in a whole "Build & Deploy"
 * run -- NOT the same thing as TransferQueue's aggregate_percent() above,
 * which can only reflect files the server has already been told about, on
 * their own short-lived connections. A deploy is 100+ separate PUT_OFFERs
 * (one connection each); files that turn out PUT_ALREADY_SATISFIED never
 * even get a TransferQueue row. Without an upfront total, the bar reads
 * 100% between files while dozens more are still queued behind it (seen
 * live 2026-08-03). MSG_DEPLOY_BEGIN (protocol.h) carries that upfront
 * total from pikodeploy once, before the first PUT_OFFER; this class is
 * just the arithmetic, kept apart from Connection/App so it's covered by
 * tests/protocol-test.cxx without FLTK or a socket. */
class DeploySession {
public:
    DeploySession() : total_bytes_(0), bytes_done_(0), active_(false) {}

    void begin(uint64_t total_bytes)
    {
        total_bytes_ = total_bytes;
        bytes_done_ = 0;
        active_ = true;
    }

    /* Bytes newly accounted for -- either actual chunk bytes as they're
     * written, or a whole file's size in one shot when PUT_OFFER already
     * finds it satisfied (that file will never generate DATA_CHUNKs at
     * all, so its weight has to be credited some other way or the bar
     * could never reach 100%). Safe to call with no active session (a
     * plain, non-deploy transfer) -- it's just a no-op then. */
    void add_bytes(uint64_t delta)
    {
        if (!active_)
            return;
        bytes_done_ += delta;
        if (bytes_done_ > total_bytes_)
            bytes_done_ = total_bytes_;
    }

    bool active() const { return active_; }

    /* 0-100, or 0 if no session has begun / it declared a 0-byte total
     * (an empty deploy, not a real one -- avoids a divide by zero). */
    double percent() const
    {
        if (!active_ || total_bytes_ == 0)
            return 0.0;
        double pct = 100.0 * static_cast<double>(bytes_done_) / static_cast<double>(total_bytes_);
        if (pct < 0.0)   pct = 0.0;
        if (pct > 100.0) pct = 100.0;
        return pct;
    }

private:
    uint64_t total_bytes_;
    uint64_t bytes_done_;
    bool active_;
};

} /* namespace pikoxfer */

#endif /* PIKOXFER_TRANSFER_QUEUE_H */
