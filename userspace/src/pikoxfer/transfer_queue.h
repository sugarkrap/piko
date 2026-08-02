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

} /* namespace pikoxfer */

#endif /* PIKOXFER_TRANSFER_QUEUE_H */
