
#ifndef PIKO_SYNC_TRANSFER_QUEUE_H
#define PIKO_SYNC_TRANSFER_QUEUE_H

#include <stdint.h>

#include <string>
#include <vector>

namespace piko_sync {

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
    std::string detail;

    TransferRow() : status(XFER_QUEUED), bytes_done(0), bytes_total(0) {}
};

class TransferQueue {
public:
    int add(const std::string &name, uint64_t bytes_total)
    {
        TransferRow r;
        r.name = name;
        r.bytes_total = bytes_total;
        rows_.push_back(r);
        return static_cast<int>(rows_.size()) - 1;
    }

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

class DeploySession {
public:
    DeploySession() : total_bytes_(0), bytes_done_(0), active_(false) {}

    void begin(uint64_t total_bytes)
    {
        total_bytes_ = total_bytes;
        bytes_done_ = 0;
        active_ = true;
    }

    void add_bytes(uint64_t delta)
    {
        if (!active_)
            return;
        bytes_done_ += delta;
        if (bytes_done_ > total_bytes_)
            bytes_done_ = total_bytes_;
    }

    bool active() const { return active_; }

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

}

#endif
