
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

    void resize(int X, int Y, int W, int H);

private:
    struct Entry {
        std::string path;
        std::string label;
    };

    struct ThumbRef { WallpaperPanel *panel; int index; };
    struct ModeRef  { WallpaperPanel *panel; int mode;  };

    std::vector<Entry>    entries_;
    std::vector<ThumbRef> thumb_refs_;
    std::vector<ModeRef>  mode_refs_;

    int         selected_mode_;
    std::string current_path_;

    Fl_Box    *preview_;
    Fl_Box    *status_;
    Fl_Scroll *scroll_;

    void scan(void);
    bool read_current_spec(void);

    static bool entry_less(const Entry &a, const Entry &b);
    void build_grid(void);
    void update_preview(void);
    void apply(const std::string &path, const std::string &label);

    static void mode_cb(Fl_Widget *, void *);
    static void thumb_cb(Fl_Widget *, void *);
    static void browse_cb(Fl_Widget *, void *);
};

Fl_Group *piko_panel_wallpaper_new(int X, int Y, int W, int H);

#endif
