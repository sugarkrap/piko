/*
 * mb-wallpaper-picker -- the wallpaper picker as a program of its own.
 *
 * There is deliberately almost nothing here. Everything this used to
 * contain -- scanning, the thumbnail grid, the mode selector, the preview
 * swatch, Browse, applying, the _MB_WALLPAPER_SPEC handshake with
 * matchbox-desktop -- now lives in panels/panel-wallpaper.cxx as an
 * Fl_Group subclass, because piko-settings shows the *same* panel inside
 * its own window when "Set Wallpaper" is tapped.
 *
 * That split is the entire point: one copy of the UI and of the logic,
 * reached two ways. This file is one of the ways; the other is the
 * PANELS[] table in piko-settings.cxx. See panels/piko-panel.H for the
 * contract and for why this is a shared widget rather than the XEmbed'd
 * second process XFCE uses.
 *
 * Keeping this a real binary rather than folding it into piko-settings
 * leaves the .desktop file's Exec= and everything that launches it by name
 * untouched, and keeps the picker usable from a shell over SSH or on a
 * system where the settings window is not what you want.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include "panels/panel-wallpaper.H"

int main(int argc, char **argv)
{
    int win_w = Fl::w();
    int win_h = Fl::h();

    Fl_Double_Window win(win_w, win_h, "Set Wallpaper");
    win.begin();

    /* No fl_register_images() here: the panel does it, guarded, because
     * FLTK's is not idempotent and piko-settings needs it too. */
    Fl_Group *panel = new WallpaperPanel(0, 0, win_w, win_h);

    win.end();
    win.resizable(panel);
    win.show(argc, argv);

    return Fl::run();
}
