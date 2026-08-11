#include "transfer_table.h"

#include <FL/fl_draw.H>

#include <stdio.h>

namespace piko_sync {

static const int COL_NAME = 0;
static const int COL_STATUS = 1;
static const int COL_PROGRESS = 2;

TransferTable::TransferTable(int X, int Y, int W, int H, const char *L)
    : Fl_Table(X, Y, W, H, L), queue_(0)
{
    col_header(1);
    col_resize(1);
    row_header(0);
    row_height_all(24);
    col_header_height(24);
    cols(3);
    rows(0);
    end();
}

void TransferTable::sync()
{
    if (!queue_)
        return;
    rows(queue_->count());
    col_width(COL_NAME, w() > 260 ? w() - 260 : 140);
    col_width(COL_STATUS, 90);
    col_width(COL_PROGRESS, 160);
    redraw();
}

const char *TransferTable::status_text(TransferStatus s)
{
    switch (s) {
    case XFER_QUEUED:        return "queued";
    case XFER_TRANSFERRING:  return "sending";
    case XFER_RECONNECTING:  return "reconnecting";
    case XFER_DONE:          return "done";
    case XFER_ERROR:         return "error";
    default:                 return "?";
    }
}

Fl_Color TransferTable::status_color(TransferStatus s)
{
    switch (s) {
    case XFER_QUEUED:        return FL_INACTIVE_COLOR;
    case XFER_TRANSFERRING:  return FL_BLUE;
    case XFER_RECONNECTING:  return fl_rgb_color(0xE0, 0x90, 0x00);
    case XFER_DONE:          return FL_DARK_GREEN;
    case XFER_ERROR:         return FL_RED;
    default:                 return FL_FOREGROUND_COLOR;
    }
}

void TransferTable::draw_cell(TableContext ctx, int R, int C,
                               int X, int Y, int W, int H)
{
    switch (ctx) {
    case CONTEXT_STARTPAGE:
        fl_font(FL_HELVETICA, 12);
        return;

    case CONTEXT_COL_HEADER: {
        static const char *headers[3] = { "File", "Status", "Progress" };
        fl_push_clip(X, Y, W, H);
        fl_draw_box(FL_THIN_UP_BOX, X, Y, W, H, FL_BACKGROUND_COLOR);
        fl_color(FL_FOREGROUND_COLOR);
        fl_font(FL_HELVETICA_BOLD, 12);
        if (C >= 0 && C < 3)
            fl_draw(headers[C], X + 4, Y, W - 8, H, FL_ALIGN_LEFT);
        fl_pop_clip();
        return;
    }

    case CONTEXT_CELL: {
        if (!queue_ || R < 0 || R >= queue_->count())
            return;
        const TransferRow &row = queue_->row(queue_->count() - 1 - R);

        fl_push_clip(X, Y, W, H);
        fl_color(FL_WHITE);
        fl_rectf(X, Y, W, H);

        if (C == COL_NAME) {
            fl_color(FL_FOREGROUND_COLOR);
            fl_font(FL_HELVETICA, 12);
            fl_draw(row.name.c_str(), X + 4, Y, W - 8, H, FL_ALIGN_LEFT);

        } else if (C == COL_STATUS) {
            fl_color(status_color(row.status));
            fl_font(FL_HELVETICA, 12);
            const char *label = (row.status == XFER_ERROR && !row.detail.empty())
                ? row.detail.c_str() : status_text(row.status);
            fl_draw(label, X + 4, Y, W - 8, H, FL_ALIGN_LEFT);

        } else if (C == COL_PROGRESS) {
            int bw = W - 10, bh = H - 8;
            int bx = X + 5, by = Y + 4;
            fl_draw_box(FL_DOWN_BOX, bx, by, bw, bh, FL_BACKGROUND_COLOR);

            double pct = 0.0;
            if (row.bytes_total > 0)
                pct = 100.0 * static_cast<double>(row.bytes_done)
                            / static_cast<double>(row.bytes_total);
            if (pct < 0.0) pct = 0.0;
            if (pct > 100.0) pct = 100.0;

            int fill = static_cast<int>((bw - 2) * pct / 100.0);
            if (fill > 0)
                fl_rectf(bx + 1, by + 1, fill, bh - 2, status_color(row.status));

            char pctbuf[16];
            snprintf(pctbuf, sizeof(pctbuf), "%d%%", static_cast<int>(pct + 0.5));
            fl_color(FL_FOREGROUND_COLOR);
            fl_font(FL_HELVETICA, 11);
            fl_draw(pctbuf, bx, by, bw, bh, FL_ALIGN_CENTER);
        }

        fl_color(FL_LIGHT2);
        fl_rect(X, Y, W, H);
        fl_pop_clip();
        return;
    }

    default:
        return;
    }
}

}
