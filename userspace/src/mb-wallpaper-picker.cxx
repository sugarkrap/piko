
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include "panels/panel-wallpaper.H"

int main(int argc, char **argv)
{
    int win_w = Fl::w();
    int win_h = Fl::h();

    Fl_Double_Window win(win_w, win_h, "Set Wallpaper");
    win.begin();

    Fl_Group *panel = new WallpaperPanel(0, 0, win_w, win_h);

    win.end();
    win.resizable(panel);
    win.show(argc, argv);

    return Fl::run();
}
