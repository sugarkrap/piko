/*
 * panel-wallpaper.H -- "Set Wallpaper" as a reusable panel.
 *
 * This is the entire wallpaper picker: scanning, the thumbnail grid, the
 * mode selector, the preview swatch, Browse, and applying. It was
 * previously the whole of mb-wallpaper-picker.cxx; that file is now a
 * fifteen-line main() around this class, and piko-settings constructs the
 * same class inside its own window. See panels/piko-panel.H for the
 * contract and why it is a widget rather than an XEmbed'd process.
 *
 * Behaviour matchbox-desktop depends on is unchanged -- see
 * docs/HOWTO-MATCHBOX-DESKTOP.md, "Wallpaper: modes, formats, and why
 * it's cached raw":
 *
 *   - scans /usr/share/backgrounds and $HOME/.matchbox/backgrounds for
 *     .png/.jpg/.jpeg/.bmp and shows them as a tap-to-apply grid;
 *   - a 4-way mode selector (Mosaic/Centered/Stretch/Fill) that is
 *     remembered but only takes effect the next time something is applied;
 *   - applying writes $HOME/.matchbox/wallpaper (mode:filename) atomically
 *     and sets _MB_WALLPAPER_SPEC on the root window so an already-running
 *     desktop updates immediately. That property exists because this
 *     device's busybox has no kill/killall/pkill at all -- a property the
 *     desktop already watches via PropertyNotify is the only way to signal
 *     it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PIKO_PANEL_WALLPAPER_H
#define PIKO_PANEL_WALLPAPER_H

#include "piko-panel.H"

#include <string>
#include <vector>

class Fl_Box;
class Fl_Scroll;

class WallpaperPanel : public Fl_Group {
public:
    WallpaperPanel(int X, int Y, int W, int H);

    /* Rebuilds the grid when the width changes -- the column count is a
     * function of it. See the note in the .cxx: the standalone picker used
     * to build its grid once at startup and did not survive a rotation. */
    void resize(int X, int Y, int W, int H);

private:
    struct Entry {
        std::string path;
        std::string label;
    };

    /* Stable per-widget callback context. FLTK callbacks carry one void*,
     * and these need two things (which panel, which item) -- so the panel
     * owns the pairs and hands out pointers into them. Both vectors are
     * sized before any pointer is taken, so nothing reallocates under a
     * live widget. */
    struct ThumbRef { WallpaperPanel *panel; int index; };
    struct ModeRef  { WallpaperPanel *panel; int mode;  };

    std::vector<Entry>    entries_;
    std::vector<ThumbRef> thumb_refs_;
    std::vector<ModeRef>  mode_refs_;

    int         selected_mode_;
    std::string current_path_;      /* what the preview is showing */

    Fl_Box    *preview_;
    Fl_Box    *status_;
    Fl_Scroll *scroll_;

    void scan(void);
    bool read_current_spec(void);

    /* A static member, not a file-scope function, only because Entry is
     * private -- std::sort needs to be handed something that can name it. */
    static bool entry_less(const Entry &a, const Entry &b);
    void build_grid(void);
    void update_preview(void);
    void apply(const std::string &path, const std::string &label);

    static void mode_cb(Fl_Widget *, void *);
    static void thumb_cb(Fl_Widget *, void *);
    static void browse_cb(Fl_Widget *, void *);
};

/* The PikoPanelFactory for this panel; what PANELS[] in piko-settings.cxx
 * points at. */
Fl_Group *piko_panel_wallpaper_new(int X, int Y, int W, int H);

#endif /* PIKO_PANEL_WALLPAPER_H */
