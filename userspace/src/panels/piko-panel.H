
#ifndef PIKO_PANEL_H
#define PIKO_PANEL_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Shared_Image.H>

typedef Fl_Group *(*PikoPanelFactory)(int X, int Y, int W, int H);

inline void piko_register_images(void)
{
    static bool registered = false;

    if (!registered) {
        fl_register_images();
        registered = true;
    }
}

#endif
