
#ifndef PIKO_SYNC_TRANSFER_TABLE_H
#define PIKO_SYNC_TRANSFER_TABLE_H

#include <FL/Fl_Table.H>
#include <FL/Enumerations.H>

#include "transfer_queue.h"

namespace piko_sync {

class TransferTable : public Fl_Table {
public:
    TransferTable(int X, int Y, int W, int H, const char *L = 0);

    void queue(TransferQueue *q) { queue_ = q; }

    void sync();

protected:
    void draw_cell(TableContext ctx, int R = 0, int C = 0,
                   int X = 0, int Y = 0, int W = 0, int H = 0);

private:
    static const char *status_text(TransferStatus s);
    static Fl_Color status_color(TransferStatus s);

    TransferQueue *queue_;
};

}

#endif
