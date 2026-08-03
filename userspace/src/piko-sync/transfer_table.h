/*
 * transfer_table.h -- the Fl_Table that renders a TransferQueue, shared by
 * piko-sync-server (incoming files) and piko-sync-client (outgoing files).
 * Same table-of-rows-with-a-drawn-progress-bar technique as pikostore's
 * HistoryTable, which itself draws its (always-disabled) Revert buttons
 * inside draw_cell rather than as real widgets -- see pikostore.cxx's file
 * header for why: it avoids embedding and repositioning N real widgets
 * for something that is redrawn on every wire update, not clicked.
 */

#ifndef PIKO_SYNC_TRANSFER_TABLE_H
#define PIKO_SYNC_TRANSFER_TABLE_H

#include <FL/Fl_Table.H>
#include <FL/Enumerations.H>

#include "transfer_queue.h"

namespace piko_sync {

class TransferTable : public Fl_Table {
public:
    TransferTable(int X, int Y, int W, int H, const char *L = 0);

    /* The queue is owned by whoever creates this table (the server/client
     * app's top-level window state), not by the table -- this widget only
     * ever reads it. */
    void queue(TransferQueue *q) { queue_ = q; }

    /* Call after add()/set_status()/set_progress() on the queue so the
     * row count and the drawn contents stay in step with it. Cheap
     * enough to call after every wire event -- this is a handful of rows
     * on a 400MHz device, not thousands. */
    void sync();

protected:
    void draw_cell(TableContext ctx, int R = 0, int C = 0,
                   int X = 0, int Y = 0, int W = 0, int H = 0);

private:
    static const char *status_text(TransferStatus s);
    static Fl_Color status_color(TransferStatus s);

    TransferQueue *queue_;
};

} /* namespace piko_sync */

#endif /* PIKO_SYNC_TRANSFER_TABLE_H */
