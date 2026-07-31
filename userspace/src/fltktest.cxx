/*
 * fltktest -- the smallest program that proves the cross-built libfltk.so
 * actually works on this device. Same role as userspace/src/sdltest.c does
 * for libSDL: built by tools/build-fltk.sh, shipped to /usr/local/bin by
 * tools/build-matchbox-payload.sh, run from st (or any shell with DISPLAY
 * set) once the Matchbox session is up.
 *
 * What it demonstrates, in the order the failures tend to happen:
 *   1. the dynamic loader can resolve libfltk.so.1.3 + libstdc++.so.6
 *      (prints the version line before touching X, so a loader failure and
 *      an X failure look different);
 *   2. FLTK can open the display and create a window on Xfbdev;
 *   3. Xft/fontconfig text renders -- this board has no core X fonts at
 *      all, so blank labels here mean the Xft path is broken, not FLTK;
 *   4. the event loop and a redraw survive on the real hardware.
 *
 * Deliberately keyboard-free: the Zaurus keyboard cannot type many
 * characters (see AGENTS.md), so quitting is a click or the window's close
 * button, never a typed command.
 */

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

/* A box that draws something FLTK-specific rather than just a label, so a
 * window that appears but stays empty is distinguishable from one that
 * never got a draw() call at all. */
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
    /* Before anything opens the display: if this line never appears, the
     * problem is the loader (a missing libfltk/libstdc++), not X. */
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
