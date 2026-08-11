
#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/fl_draw.H>

#include <stdio.h>
#include <stdlib.h>

static void quit_cb(Fl_Widget *, void *) {
    exit(0);
}

class ProofBox : public Fl_Box {
public:
    ProofBox(int x, int y, int w, int h) : Fl_Box(x, y, w, h) {}
    void draw() {
        Fl_Box::draw();
        fl_color(FL_BLUE);
        fl_line(x(), y(), x() + w() - 1, y() + h() - 1);
        fl_line(x(), y() + h() - 1, x() + w() - 1, y());
        fl_color(FL_BLACK);
        fl_rect(x(), y(), w(), h());
    }
};

int main(int argc, char **argv) {
    printf("fltktest: FLTK %d.%d.%d\n",
           FL_MAJOR_VERSION, FL_MINOR_VERSION, FL_PATCH_VERSION);
    fflush(stdout);

    Fl_Window win(260, 170, "FLTK on Zaurus");

    Fl_Box title(10, 8, 240, 24, "FLTK is alive");
    title.labelfont(FL_HELVETICA_BOLD);
    title.labelsize(16);

    ProofBox proof(10, 38, 240, 70);

    Fl_Button quit(85, 118, 90, 32, "Quit");
    quit.callback(quit_cb);

    win.end();
    win.show(argc, argv);

    printf("fltktest: window shown, entering event loop\n");
    fflush(stdout);

    return Fl::run();
}
