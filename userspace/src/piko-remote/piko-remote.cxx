#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

#include "remote.h"

#define HEADER_H        48
#define TAB_H           38
#define STATUS_H        28
#define PAD             8
#define GAP             6
#define FAV_W           48
#define ENABLE_W        96
#define CANCEL_W        88
#define CELL_MAX_H      76
#define TAB_MIN_W       64
#define POLL_SECONDS    2.0

#define LIRC_DEV        "/dev/lirc0"
#define IRCTL_BIN       "/usr/bin/irctl"
#define CONFIG_CARD     "/mnt/card/.zaurus/etc/piko/remote.cfg"

#define C_HEADER        fl_rgb_color(0xF0, 0xF0, 0xEC)
#define C_TEXT          fl_rgb_color(0x20, 0x20, 0x20)
#define C_SUBTEXT       fl_rgb_color(0x60, 0x60, 0x60)
#define C_KEY           fl_rgb_color(0xE4, 0xE4, 0xE0)
#define C_KEY_OFF       fl_rgb_color(0xEE, 0xEE, 0xEC)
#define C_KEY_TEXT_OFF  fl_rgb_color(0xB4, 0xB4, 0xB0)
#define C_ACCENT        fl_rgb_color(0x34, 0x65, 0xA4)
#define C_LINE          fl_rgb_color(0xC8, 0xC8, 0xC4)
#define C_POWER         fl_rgb_color(0xC0, 0x30, 0x28)
#define C_RED           fl_rgb_color(0xD0, 0x40, 0x38)
#define C_GREEN         fl_rgb_color(0x40, 0xA0, 0x48)
#define C_YELLOW        fl_rgb_color(0xE0, 0xC0, 0x30)
#define C_BLUE          fl_rgb_color(0x38, 0x68, 0xC8)

enum Tint { T_NONE, T_POWER, T_RED, T_GREEN, T_YELLOW, T_BLUE };

struct Cell {
    const char *label;
    PIRKeyName key;
    Tint tint;
};

struct Panel {
    const char *name;
    int cols;
    const Cell *cells;
    bool inputs;
};

#define END { NULL, Unmapped_Key, T_NONE }
#define SKIP { "", Unmapped_Key, T_NONE }

static const Cell main_cells[] = {
    { "Power", Power_Key, T_POWER }, { "Input", Input_Key, T_NONE },
    { "Mute", Mute_Key, T_NONE },
    { "Vol +", VolumeUp_Key, T_NONE }, { "Info", Info_Key, T_NONE },
    { "Ch +", ChannelUp_Key, T_NONE },
    { "Vol -", VolumeDown_Key, T_NONE }, { "Last", PrevChannel_Key, T_NONE },
    { "Ch -", ChannelDown_Key, T_NONE },
    END
};

static const Cell utility_cells[] = {
    { "", Red_Key, T_RED }, { "", Green_Key, T_GREEN },
    { "", Yellow_Key, T_YELLOW }, { "", Blue_Key, T_BLUE },
    { "Picture", PictureMode_Key, T_NONE }, { "Sound", SoundMode_Key, T_NONE },
    { "Aspect", AspectRatio_Key, T_NONE }, { "Audio", Audio_Key, T_NONE },
    { "Info", Info_Key, T_NONE }, { "Subtitles", Captions_Key, T_NONE },
    { "Input", Input_Key, T_NONE }, { "Sleep", Sleep_Key, T_NONE },
    { "Guide", Guide_Key, T_NONE }, { "Eco", EnergySave_Key, T_NONE },
    { "Zoom", Zoom_Key, T_NONE }, { "Favorites", Favorites_Key, T_NONE },
    END
};

static const Cell keypad_cells[] = {
    { "1", One_Key, T_NONE }, { "2", Two_Key, T_NONE },
    { "3", Three_Key, T_NONE }, { "Enter", Enter_Key, T_NONE },
    { "4", Four_Key, T_NONE }, { "5", Five_Key, T_NONE },
    { "6", Six_Key, T_NONE }, { "Clear", Clear_Key, T_NONE },
    { "7", Seven_Key, T_NONE }, { "8", Eight_Key, T_NONE },
    { "9", Nine_Key, T_NONE }, { "Last", PrevChannel_Key, T_NONE },
    { "-/--", DoubleDigit_Key, T_NONE }, { "0", Zero_Key, T_NONE },
    { "+100", PlusOneHundred_Key, T_NONE }, { "Dash", Dash_Key, T_NONE },
    END
};

static const Cell menu_cells[] = {
    { "Menu", Menu_Key, T_NONE }, { "@8->", Up_Key, T_NONE },
    { "Exit", Exit_Key, T_NONE },
    { "@4->", Left_Key, T_NONE }, { "OK", Select_Key, T_NONE },
    { "@->", Right_Key, T_NONE },
    { "Guide", Guide_Key, T_NONE }, { "@2->", Down_Key, T_NONE },
    { "Disc menu", DiscMenu_Key, T_NONE },
    END
};

static const Cell tvmenu_cells[] = {
    { "Menu", Menu_Key, T_NONE }, { "@8->", Up_Key, T_NONE },
    { "Exit", Exit_Key, T_NONE },
    { "@4->", Left_Key, T_NONE }, { "OK", Select_Key, T_NONE },
    { "@->", Right_Key, T_NONE },
    { "Back", Back_Key, T_NONE }, { "@2->", Down_Key, T_NONE },
    { "Tools", ToolsMenu_Key, T_NONE },
    { "Quick", QuickMenu_Key, T_NONE }, { "Apps", AppsMenu_Key, T_NONE },
    { "Link", CECMenu_Key, T_NONE },
    END
};

static const Cell media_cells[] = {
    { "@|<", Previous_Key, T_NONE }, { "@<<", Rewind_Key, T_NONE },
    { "@>>", FastForward_Key, T_NONE }, { "@>|", Next_Key, T_NONE },
    { "Replay", Replay_Key, T_NONE }, { "Step -", StepBack_Key, T_NONE },
    { "Step +", StepForward_Key, T_NONE }, { "Skip", Advance_Key, T_NONE },
    { "@>", Play_Key, T_NONE }, { "@||", Pause_Key, T_NONE },
    { "@square", Stop_Key, T_NONE }, { "Eject", Eject_Key, T_NONE },
    END
};

static const Cell record_cells[] = {
    { "Record", Record_Key, T_POWER }, { "Rec pause", RecordPause_Key, T_NONE },
    { "Rec stop", RecordStop_Key, T_NONE }, { "Rec mute", RecordMute_Key, T_NONE },
    { "Timer", Timer_Key, T_NONE }, { "Program", Program_Key, T_NONE },
    { "Memory", Memory_Key, T_NONE }, { "Call", Call_Key, T_NONE },
    { "Favorites", Favorites_Key, T_NONE }, { "Scan", Scan_Key, T_NONE },
    { "Preset -", PrevPreset_Key, T_NONE }, { "Preset +", NextPreset_Key, T_NONE },
    END
};

static const Cell tv_cells[] = {
    { "Teletext", Teletext_Key, T_NONE }, { "Hold", TeletextHold_Key, T_NONE },
    { "Page +", PageUp_Key, T_NONE }, { "Page -", PageDown_Key, T_NONE },
    { "PIP", PIP_Key, T_NONE }, { "Swap", PIPSwap_Key, T_NONE },
    { "Move", PIPMove_Key, T_NONE }, { "Source", PIPSource_Key, T_NONE },
    { "PIP ch +", PIPChannelUp_Key, T_NONE },
    { "PIP ch -", PIPChannelDown_Key, T_NONE },
    { "Freeze", PIPPause_Key, T_NONE }, { "Size", PIPSize_Key, T_NONE },
    { "Favorites", Favorites_Key, T_NONE }, { "Eco", EnergySave_Key, T_NONE },
    { "Smart", SmartHub_Key, T_NONE }, { "Live", LiveTV_Key, T_NONE },
    END
};

static const Cell receiver_cells[] = {
    { "Power", Power_Key, T_POWER }, { "Mute", Mute_Key, T_NONE },
    { "Vol +", VolumeUp_Key, T_NONE }, { "Vol -", VolumeDown_Key, T_NONE },
    { "Ch +", ChannelUp_Key, T_NONE }, { "Ch -", ChannelDown_Key, T_NONE },
    { "Preset -", PrevPreset_Key, T_NONE }, { "Preset +", NextPreset_Key, T_NONE },
    { "Band", ToggleBand_Key, T_NONE }, { "LW", LW_Key, T_NONE },
    { "AM", AM_Key, T_NONE }, { "FM", FM_Key, T_NONE },
    END
};

static const Cell audio_cells[] = {
    { "FM mode", FMMode_Key, T_NONE }, { "RDS", RDS_Key, T_NONE },
    { "Sleep", Sleep_Key, T_NONE },
    { "Repeat", Repeat_Key, T_NONE }, { "A-B", RepeatAB_Key, T_NONE },
    { "Random", Random_Key, T_NONE },
    { "Disc -", PrevDisc_Key, T_NONE }, { "Disc +", NextDisc_Key, T_NONE },
    { "Info", Info_Key, T_NONE },
    END
};

static const Cell adjust_cells[] = {
    { "Bass +", BassUp_Key, T_NONE }, { "Treble +", TrebleUp_Key, T_NONE },
    { "Bal left", BalanceLeft_Key, T_NONE },
    { "Bal right", BalanceRight_Key, T_NONE },
    { "Bass -", BassDown_Key, T_NONE }, { "Treble -", TrebleDown_Key, T_NONE },
    { "Surround", Surround_Key, T_NONE }, { "Bass boost", EnhancedBass_Key, T_NONE },
    { "Color +", ColorUp_Key, T_NONE }, { "Bright +", BrightnessUp_Key, T_NONE },
    { "Contrast +", ContrastUp_Key, T_NONE },
    { "Denoise", NoiseReduction_Key, T_NONE },
    { "Color -", ColorDown_Key, T_NONE }, { "Bright -", BrightnessDown_Key, T_NONE },
    { "Contrast -", ContrastDown_Key, T_NONE }, { "Zoom", Zoom_Key, T_NONE },
    END
};

static const Cell inputs_cells[] = {
    { "Next input", Input_Key, T_NONE },
    END
};

static const Cell ac_cells[] = {
    { "Power", Power_Key, T_POWER }, { "Mode", Mode_Key, T_NONE },
    { "Timer", Timer_Key, T_NONE },
    { "Temp +", TempUp_Key, T_NONE }, { "Fan +", FanFaster_Key, T_NONE },
    { "Swing", Oscillate_Key, T_NONE },
    { "Temp -", TempDown_Key, T_NONE }, { "Fan -", FanSlower_Key, T_NONE },
    { "Eco", EnergySave_Key, T_NONE },
    { "Cool", Cool_Key, T_NONE }, { "Heat", Heat_Key, T_NONE },
    { "Fan", Fan_Key, T_NONE },
    END
};

static const Cell camera_cells[] = {
    { "Shutter", OpenShutter_Key, T_NONE },
    { "Delayed", DelayedOpenShutter_Key, T_NONE },
    END
};

static const Cell roomba_cells[] = {
    { "Spot", RobotSpot_Key, T_NONE }, { "@8->", RobotForward_Key, T_NONE },
    { "Clean", RobotClean_Key, T_NONE },
    { "@4->", RobotCCW_Key, T_NONE }, { "@||", Pause_Key, T_NONE },
    { "@->", RobotCW_Key, T_NONE },
    { "Max", RobotMax_Key, T_NONE }, SKIP,
    { "Power", Power_Key, T_POWER },
    END
};

static const Cell playstation_cells[] = {
    { "Select", PSSelect_Key, T_NONE }, { "L1", PSL1_Key, T_NONE },
    { "L2", PSL2_Key, T_NONE }, { "L3", PSL3_Key, T_NONE },
    { "Start", PSStart_Key, T_NONE }, { "R1", PSR1_Key, T_NONE },
    { "R2", PSR2_Key, T_NONE }, { "R3", PSR3_Key, T_NONE },
    { "@8->", PSUp_Key, T_NONE }, { "@2->", PSDown_Key, T_NONE },
    { "@4->", PSLeft_Key, T_NONE }, { "@->", PSRight_Key, T_NONE },
    { "Triangle", PSTriangle_Key, T_GREEN }, { "Cross", PSCross_Key, T_BLUE },
    { "Square", PSSquare_Key, T_POWER }, { "Circle", PSCircle_Key, T_RED },
    END
};

static const Panel P_MAIN = { "Main", 3, main_cells, false };
static const Panel P_UTILITY = { "Extras", 4, utility_cells, false };
static const Panel P_KEYPAD = { "Keypad", 4, keypad_cells, false };
static const Panel P_MENU = { "Menu", 3, menu_cells, false };
static const Panel P_TVMENU = { "Menu", 3, tvmenu_cells, false };
static const Panel P_MEDIA = { "Media", 4, media_cells, false };
static const Panel P_RECORD = { "Record", 4, record_cells, false };
static const Panel P_TV = { "TV", 4, tv_cells, false };
static const Panel P_RECEIVER = { "Receiver", 4, receiver_cells, false };
static const Panel P_AUDIO = { "Audio", 3, audio_cells, false };
static const Panel P_ADJUST = { "Adjust", 4, adjust_cells, false };
static const Panel P_INPUTS = { "Inputs", 3, inputs_cells, true };
static const Panel P_AC = { "Climate", 3, ac_cells, false };
static const Panel P_CAMERA = { "Camera", 2, camera_cells, false };
static const Panel P_ROOMBA = { "Roomba", 3, roomba_cells, false };
static const Panel P_PLAYSTATION = { "Console", 4, playstation_cells, false };

class KeyButton;

static Fl_Double_Window *g_win;
static Fl_Box *g_header;
static Fl_Button *g_pick;
static Fl_Button *g_fav;
static Fl_Group *g_tabs;
static Fl_Group *g_panel;
static Fl_Box *g_empty;
static Fl_Box *g_status;
static Fl_Button *g_enable;
static Fl_Group *g_picker;
static Fl_Input *g_search;
static Fl_Button *g_cancel;
static Fl_Hold_Browser *g_makes;
static Fl_Hold_Browser *g_list;

static std::vector<const Panel *> g_panels;
static std::vector<Fl_Button *> g_tab_buttons;
static std::vector<KeyButton *> g_keys;
static std::vector<RemoteInput> g_inputs;
static size_t g_active;
static std::string g_active_name;
static std::set<std::string> g_favs;
static std::vector<RemoteDevice> g_devices;
static bool g_ir_on;
static std::string g_pick_label;

static std::string escape(const std::string &in)
{
    std::string out;

    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '@' || in[i] == '&')
            out += in[i];
        out += in[i];
    }
    return out;
}

static std::string lower(const std::string &in)
{
    std::string out(in);

    for (size_t i = 0; i < out.size(); i++)
        if (out[i] >= 'A' && out[i] <= 'Z')
            out[i] = out[i] - 'A' + 'a';
    return out;
}

static std::string ident(const RemoteKeyset &k)
{
    return k.make + "\t" + k.name;
}

static void status(const std::string &text)
{
    g_status->copy_label(escape(text).c_str());
    g_status->redraw();
}

static const char *config_path(void)
{
    const char *env = getenv("PIKO_REMOTE_CFG");

    return env && *env ? env : CONFIG_CARD;
}

static void mkdirs(const std::string &path)
{
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] != '/')
            continue;
        mkdir(path.substr(0, i).c_str(), 0755);
    }
}

static void save_config(void)
{
    std::string path = config_path();
    std::string tmp = path + ".new";
    const RemoteKeyset *cur = remote::find(remote::current());
    FILE *f;

    mkdirs(path);
    f = fopen(tmp.c_str(), "w");
    if (!f)
        return;

    if (cur)
        fprintf(f, "last\t%s\n", ident(*cur).c_str());
    if (!g_active_name.empty())
        fprintf(f, "panel\t%s\n", g_active_name.c_str());
    for (std::set<std::string>::iterator i = g_favs.begin(); i != g_favs.end(); ++i)
        fprintf(f, "fav\t%s\n", i->c_str());

    if (fclose(f) == 0)
        rename(tmp.c_str(), path.c_str());
}

static std::string load_config(void)
{
    FILE *f = fopen(config_path(), "r");
    std::string last;
    char line[512];

    if (!f)
        return last;

    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        size_t n = strlen(line);

        if (n && line[n - 1] == '\n')
            line[n - 1] = '\0';
        if (!tab)
            continue;
        *tab++ = '\0';

        if (!strcmp(line, "last"))
            last = tab;
        else if (!strcmp(line, "fav"))
            g_favs.insert(tab);
        else if (!strcmp(line, "panel"))
            g_active_name = tab;
    }

    fclose(f);
    return last;
}

static bool ir_available(void)
{
    return access(LIRC_DEV, F_OK) == 0;
}

static void rounded(int X, int Y, int W, int H, int r, Fl_Color c)
{
    int d = r * 2;

    fl_color(c);
    fl_rectf(X + r, Y, W - d, H);
    fl_rectf(X, Y + r, r, H - d);
    fl_rectf(X + W - r, Y + r, r, H - d);
    fl_pie(X, Y, d, d, 90, 180);
    fl_pie(X + W - d - 1, Y, d, d, 0, 90);
    fl_pie(X, Y + H - d - 1, d, d, 180, 270);
    fl_pie(X + W - d - 1, Y + H - d - 1, d, d, 270, 360);
}

static void stop_cb(void *);
static void relayout(void);

class KeyButton : public Fl_Button {
public:
    KeyButton(const Cell &c, PIRKeyName key, const std::string &label)
        : Fl_Button(0, 0, 10, 10), key_(key), tint_(c.tint), held_(false)
    {
        copy_label(label.c_str());
        box(FL_FLAT_BOX);
        labelsize(15);
        labelfont(FL_HELVETICA_BOLD);
        clear_visible_focus();
    }

    int handle(int e)
    {
        switch (e) {
        case FL_PUSH:
            if (!active_r())
                return 1;
            if (!g_ir_on) {
                status("Infrared is off -- tap Enable IR");
                return 1;
            }
            held_ = true;
            redraw();
            if (!remote::press(key_))
                status("Still sending, try again");
            else
                status(std::string("Sending ") + (label() && *label() && *label() != '@' ? label() : "key"));
            return 1;
        case FL_RELEASE:
            if (held_) {
                held_ = false;
                remote::release();
                Fl::add_timeout(0.05, stop_cb);
                redraw();
            }
            return 1;
        case FL_DRAG:
            return 1;
        }
        return Fl_Button::handle(e);
    }

    void draw()
    {
        bool on = active_r();
        Fl_Color fill = on ? C_KEY : C_KEY_OFF;
        Fl_Color text = on ? C_TEXT : C_KEY_TEXT_OFF;
        Fl_Color mark = tint_color();

        fl_color(g_panel->color());
        fl_rectf(x(), y(), w(), h());

        if (held_) {
            fill = C_ACCENT;
            text = FL_WHITE;
        } else if (mark && !(label() && *label())) {
            fill = on ? mark : fl_color_average(mark, C_KEY_OFF, 0.3f);
        }

        rounded(x(), y(), w(), h(), 8, fill);

        if (mark && label() && *label() && !held_) {
            fl_color(on ? mark : C_KEY_TEXT_OFF);
            fl_rectf(x() + 10, y() + h() - 7, w() - 20, 3);
        }

        labelcolor(text);
        draw_label(x() + 4, y(), w() - 8, h(), FL_ALIGN_CENTER | FL_ALIGN_WRAP);
    }

    PIRKeyName key() const { return key_; }

private:
    Fl_Color tint_color() const
    {
        switch (tint_) {
        case T_POWER: return C_POWER;
        case T_RED: return C_RED;
        case T_GREEN: return C_GREEN;
        case T_YELLOW: return C_YELLOW;
        case T_BLUE: return C_BLUE;
        default: return 0;
        }
    }

    PIRKeyName key_;
    Tint tint_;
    bool held_;
};

static void stop_cb(void *)
{
    if (remote::busy()) {
        remote::release();
        Fl::repeat_timeout(0.05, stop_cb);
    }
}

static bool panel_has_keys(const Panel &p)
{
    for (const Cell *c = p.cells; c->label; c++)
        if (c->key != Unmapped_Key && remote::has(c->key))
            return true;
    return p.inputs && !remote::inputs().empty();
}

static void add_panel(const Panel &p)
{
    for (size_t i = 0; i < g_panels.size(); i++)
        if (g_panels[i] == &p)
            return;
    if (panel_has_keys(p))
        g_panels.push_back(&p);
}

static void collect_panels(PIRPanelTypes types)
{
    g_panels.clear();

    if (types & TV_Panels) {
        add_panel(P_MAIN);
        add_panel(P_KEYPAD);
        add_panel(P_TVMENU);
        add_panel(P_UTILITY);
        if (types & MediaControl_Panels)
            add_panel(P_MEDIA);
        add_panel(P_INPUTS);
        add_panel(P_TV);
        add_panel(P_ADJUST);
    }

    if ((types & MediaControl_Panels) && !(types & TV_Panels)) {
        add_panel(P_MAIN);
        add_panel(P_MEDIA);
        add_panel(P_MENU);
        add_panel(P_KEYPAD);
        add_panel(P_UTILITY);
        add_panel(P_INPUTS);
    }

    if (types & Receiver_Panels) {
        add_panel(P_RECEIVER);
        add_panel(P_AUDIO);
        add_panel(P_KEYPAD);
        add_panel(P_MENU);
        add_panel(P_MEDIA);
        add_panel(P_INPUTS);
        add_panel(P_ADJUST);
    }

    if (types & MediaRecord_Panels)
        add_panel(P_RECORD);
    if (types & (ACFan_Panels | StatefulAC_Panels))
        add_panel(P_AC);
    if (types & Camera_Panels)
        add_panel(P_CAMERA);
    if (types & Roomba_Panels)
        add_panel(P_ROOMBA);
    if (types & Playstation_Panels)
        add_panel(P_PLAYSTATION);

    if (g_panels.empty()) {
        add_panel(P_MAIN);
        add_panel(P_KEYPAD);
        add_panel(P_MENU);
        add_panel(P_MEDIA);
        add_panel(P_UTILITY);
        add_panel(P_INPUTS);
    }
}

static void layout_panel(void)
{
    if (g_active >= g_panels.size()) {
        g_empty->resize(g_panel->x(), g_panel->y(), g_panel->w(), g_panel->h());
        return;
    }

    const Panel &p = *g_panels[g_active];
    int n = (int)g_keys.size();
    int cols = p.cols;

    if (p.inputs && cols > 1 && g_panel->w() < g_panel->h())
        cols = 2;
    if (!p.inputs && cols == 4 && g_panel->w() < 360)
        cols = 3;

    int rows = (n + cols - 1) / cols;
    if (rows < 1)
        rows = 1;

    int aw = g_panel->w() - PAD * 2;
    int ah = g_panel->h() - PAD * 2;
    int cw = (aw - GAP * (cols - 1)) / cols;
    int ch = (ah - GAP * (rows - 1)) / rows;

    if (ch > CELL_MAX_H)
        ch = CELL_MAX_H;

    int total_h = rows * ch + (rows - 1) * GAP;
    int y0 = g_panel->y() + PAD + (ah - total_h) / 2;

    for (int i = 0; i < n; i++) {
        int r = i / cols;
        int c = i % cols;

        g_keys[i]->resize(g_panel->x() + PAD + c * (cw + GAP),
                          y0 + r * (ch + GAP), cw, ch);
    }
}

static void build_panel(void)
{
    g_panel->clear();
    g_keys.clear();

    g_panel->begin();
    g_empty = new Fl_Box(0, 0, 10, 10);
    g_empty->labelsize(14);
    g_empty->labelcolor(C_SUBTEXT);
    g_empty->align(FL_ALIGN_CENTER | FL_ALIGN_WRAP | FL_ALIGN_INSIDE);

    if (g_active < g_panels.size()) {
        const Panel &p = *g_panels[g_active];

        g_empty->label(NULL);

        for (const Cell *c = p.cells; c->label; c++) {
            KeyButton *b = new KeyButton(*c, c->key, c->label);
            if (c->key == Unmapped_Key) {
                b->hide();
                b->deactivate();
            } else if (!remote::has(c->key)) {
                b->deactivate();
            }
            g_keys.push_back(b);
        }

        if (p.inputs) {
            for (size_t i = 0; i < g_inputs.size(); i++) {
                Cell c = { "", g_inputs[i].second, T_NONE };
                g_keys.push_back(new KeyButton(c, g_inputs[i].second,
                                               escape(g_inputs[i].first)));
            }
        }
    } else {
        g_empty->label(remote::current()
                       ? "This keyset has no keys this remote can show yet."
                       : "Tap the bar above to pick a device.");
    }
    g_panel->end();

    layout_panel();
    g_panel->redraw();
}

static int tab_rows(int avail)
{
    int n = (int)g_tab_buttons.size();
    int total = 0;

    fl_font(FL_HELVETICA_BOLD, 13);
    for (int i = 0; i < n; i++) {
        int w = (int)fl_width(g_tab_buttons[i]->label()) + 20;
        total += w < TAB_MIN_W ? TAB_MIN_W : w;
    }

    if (n == 0 || avail <= 0)
        return 1;
    return (total + avail - 1) / avail;
}

static void layout_tabs(void)
{
    int n = (int)g_tab_buttons.size();
    int rows = tab_rows(g_tabs->w());
    int per = (n + rows - 1) / (rows ? rows : 1);

    for (int i = 0; i < n; i++) {
        int r = i / per;
        int c = i % per;
        int in_row = r == rows - 1 ? n - per * r : per;
        int x0 = g_tabs->x() + g_tabs->w() * c / in_row;
        int x1 = g_tabs->x() + g_tabs->w() * (c + 1) / in_row;

        g_tab_buttons[i]->resize(x0, g_tabs->y() + r * TAB_H, x1 - x0, TAB_H);
    }
}

class TabButton : public Fl_Button {
public:
    TabButton(const char *l) : Fl_Button(0, 0, 10, TAB_H)
    {
        copy_label(l);
        type(FL_RADIO_BUTTON);
        box(FL_FLAT_BOX);
        labelsize(13);
        labelfont(FL_HELVETICA_BOLD);
        clear_visible_focus();
    }

    void draw()
    {
        fl_color(C_HEADER);
        fl_rectf(x(), y(), w(), h());
        labelcolor(value() ? C_ACCENT : C_SUBTEXT);
        draw_label();
        fl_color(C_LINE);
        fl_line(x(), y() + h() - 1, x() + w() - 1, y() + h() - 1);
        if (value()) {
            fl_color(C_ACCENT);
            fl_rectf(x() + 6, y() + h() - 4, w() - 12, 4);
        }
    }
};

static void tab_cb(Fl_Widget *, void *data)
{
    g_active = (size_t)(intptr_t)data;
    g_active_name = g_panels[g_active]->name;
    for (size_t i = 0; i < g_tab_buttons.size(); i++)
        g_tab_buttons[i]->value(i == g_active);
    build_panel();
    save_config();
    g_tabs->redraw();
}

static void build_tabs(void)
{
    g_tabs->clear();
    g_tab_buttons.clear();

    g_tabs->begin();
    for (size_t i = 0; i < g_panels.size(); i++) {
        Fl_Button *b = new TabButton(g_panels[i]->name);
        b->callback(tab_cb, (void *)(intptr_t)i);
        b->value(i == g_active);
        g_tab_buttons.push_back(b);
    }
    g_tabs->end();

    relayout();
}

static void update_header(void)
{
    const RemoteKeyset *k = remote::find(remote::current());

    if (k) {
        g_pick_label = escape(k->make) + "\n" + escape(k->name);
        g_fav->activate();
        g_fav->value(g_favs.count(ident(*k)) ? 1 : 0);
    } else {
        g_pick_label = "Choose a device\nTap to browse keysets";
        g_fav->deactivate();
        g_fav->value(0);
    }

    g_pick->label(g_pick_label.c_str());
    g_pick->redraw();
    g_fav->redraw();
}

static void select_keyset(unsigned int id)
{
    if (!remote::select(id)) {
        status("That keyset is gone");
        return;
    }

    const RemoteKeyset *k = remote::find(id);

    g_inputs = remote::inputs();
    collect_panels(k->panels);

    g_active = 0;
    for (size_t i = 0; i < g_panels.size(); i++)
        if (g_active_name == g_panels[i]->name)
            g_active = i;
    if (g_active < g_panels.size())
        g_active_name = g_panels[g_active]->name;

    build_tabs();
    build_panel();
    update_header();
    save_config();

    status(k->make + " " + k->name);
}

static void fill_makes(void)
{
    std::vector<std::string> makes = remote::makes();

    g_makes->clear();
    g_makes->add("@b@.Favorites");
    for (size_t i = 0; i < makes.size(); i++)
        g_makes->add(("@." + makes[i]).c_str());
}

static void add_keyset_line(const RemoteKeyset &k, bool with_make)
{
    std::string line = "@.";

    if (g_favs.count(ident(k)))
        line += "* ";
    if (with_make)
        line += k.make + "  ";
    line += k.name;
    g_list->add(line.c_str(), (void *)(uintptr_t)k.id);
    if (k.id == remote::current())
        g_list->select(g_list->size());
}

static void show_make(int line)
{
    const std::vector<RemoteKeyset> &all = remote::keysets();

    g_list->clear();

    if (line == 1) {
        for (size_t i = 0; i < all.size(); i++)
            if (g_favs.count(ident(all[i])))
                add_keyset_line(all[i], true);
        if (!g_list->size())
            g_list->add("@i@.No favorites yet");
        return;
    }

    std::string make = g_makes->text(line) + 2;
    for (size_t i = 0; i < all.size(); i++)
        if (all[i].make == make)
            add_keyset_line(all[i], false);
}

static void show_search(const std::string &query)
{
    const std::vector<RemoteKeyset> &all = remote::keysets();
    std::string q = lower(query);

    g_list->clear();
    g_makes->deselect();

    for (size_t i = 0; i < all.size(); i++) {
        if (lower(all[i].make + " " + all[i].name).find(q) == std::string::npos)
            continue;
        add_keyset_line(all[i], true);
    }

    for (size_t i = 0; i < g_devices.size(); i++) {
        const RemoteDevice &d = g_devices[i];
        if (lower(d.make + " " + d.model).find(q) == std::string::npos)
            continue;
        const RemoteKeyset *k = remote::find(d.keyset);
        if (!k)
            continue;
        std::string line = "@." + d.make + " " + d.model + "  (" + k->name + ")";
        g_list->add(line.c_str(), (void *)(uintptr_t)k->id);
    }

    if (!g_list->size())
        g_list->add("@i@.Nothing matches");
}

static void open_picker(void)
{
    if (g_devices.empty())
        g_devices = remote::devices();

    fill_makes();

    const RemoteKeyset *cur = remote::find(remote::current());
    int line = 1;

    if (cur) {
        for (int i = 2; i <= g_makes->size(); i++) {
            if (cur->make == g_makes->text(i) + 2) {
                line = i;
                break;
            }
        }
    }

    g_search->value("");
    g_makes->select(line);
    g_makes->middleline(line);
    show_make(line);

    g_picker->show();
    g_tabs->hide();
    g_panel->hide();
    relayout();
}

static void close_picker(void)
{
    g_picker->hide();
    g_tabs->show();
    g_panel->show();
    relayout();
}

static void pick_cb(Fl_Widget *, void *)
{
    if (g_picker->visible())
        close_picker();
    else
        open_picker();
}

static void fav_cb(Fl_Widget *, void *)
{
    const RemoteKeyset *k = remote::find(remote::current());

    if (!k)
        return;

    std::string id = ident(*k);
    if (g_favs.count(id)) {
        g_favs.erase(id);
        status("Removed from favorites");
    } else {
        g_favs.insert(id);
        status("Added to favorites");
    }

    save_config();
    update_header();
}

static void makes_cb(Fl_Widget *, void *)
{
    int line = g_makes->value();

    if (line < 1)
        return;
    g_search->value("");
    show_make(line);
}

static void list_cb(Fl_Widget *, void *)
{
    int line = g_list->value();

    if (line < 1 || !g_list->data(line))
        return;

    unsigned int id = (unsigned int)(uintptr_t)g_list->data(line);

    close_picker();
    select_keyset(id);
}

static void search_cb(Fl_Widget *, void *)
{
    std::string q = g_search->value();

    if (q.empty()) {
        int line = g_makes->value();
        if (line < 1) {
            line = 1;
            g_makes->select(line);
        }
        show_make(line);
    } else {
        show_search(q);
    }
}

static void cancel_cb(Fl_Widget *, void *)
{
    close_picker();
}

static void apply_ir_state(bool on)
{
    g_ir_on = on;

    if (on)
        g_enable->hide();
    else
        g_enable->show();

    relayout();
}

static int run_irctl(std::string &out)
{
    const char *env = getenv("PIKO_IRCTL");
    std::string cmd = std::string(env && *env ? env : IRCTL_BIN) + " blaster 2>&1";
    FILE *p = popen(cmd.c_str(), "r");
    char buf[256];
    int rc;

    out.clear();
    if (!p)
        return -1;

    while (fgets(buf, sizeof(buf), p))
        if (out.empty())
            out = buf;

    rc = pclose(p);
    if (!out.empty() && out[out.size() - 1] == '\n')
        out.erase(out.size() - 1);

    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static void enable_cb(Fl_Widget *, void *)
{
    std::string out;
    int rc;

    status("Turning infrared on...");
    Fl::check();

    rc = run_irctl(out);

    if (!ir_available()) {
        if (!out.empty())
            status(out);
        else
            status("irctl blaster failed (" + std::to_string(rc) + ")");
        return;
    }

    apply_ir_state(true);
    status("Infrared is on");
}

static void poll_cb(void *)
{
    bool on = ir_available();

    if (on != g_ir_on) {
        apply_ir_state(on);
        status(on ? "Infrared is on" : "Infrared was turned off");
    }

    Fl::repeat_timeout(POLL_SECONDS, poll_cb);
}

static void wake_cb(void *)
{
    std::string err;

    if (remote::take_error(err))
        status(err);
}

static void wake(void)
{
    Fl::awake(wake_cb, NULL);
}

static void relayout(void)
{
    int W = g_win->w();
    int H = g_win->h();
    int body_y = HEADER_H;
    int body_h = H - HEADER_H - STATUS_H;

    g_header->resize(0, 0, W, HEADER_H);
    g_fav->resize(W - PAD - FAV_W, (HEADER_H - 36) / 2, FAV_W, 36);
    g_pick->resize(PAD, 2, W - PAD * 3 - FAV_W, HEADER_H - 4);

    int tabs_h = g_tab_buttons.empty() ? 0 : tab_rows(W) * TAB_H;
    g_tabs->resize(0, body_y, W, tabs_h);
    layout_tabs();
    g_panel->resize(0, body_y + tabs_h, W, body_h - tabs_h);
    layout_panel();

    g_picker->resize(0, body_y, W, body_h);
    int sw = W - PAD * 3 - CANCEL_W;
    g_search->resize(PAD, body_y + PAD, sw, 34);
    g_cancel->resize(W - PAD - CANCEL_W, body_y + PAD, CANCEL_W, 34);

    int ly = body_y + PAD * 2 + 34;
    int lh = body_h - PAD * 3 - 34;
    int mw = W < H ? W * 2 / 5 : W / 3;
    g_makes->resize(PAD, ly, mw, lh);
    g_list->resize(PAD * 2 + mw, ly, W - PAD * 3 - mw, lh);

    int ew = g_enable->visible() ? ENABLE_W + PAD : 0;
    g_status->resize(0, H - STATUS_H, W - ew, STATUS_H);
    g_enable->resize(W - ENABLE_W - PAD / 2, H - STATUS_H + 2, ENABLE_W,
                     STATUS_H - 4);

    g_win->redraw();
}

class RemoteWindow : public Fl_Double_Window {
public:
    RemoteWindow(int w, int h, const char *l) : Fl_Double_Window(w, h, l) {}

    void resize(int X, int Y, int W, int H)
    {
        Fl_Double_Window::resize(X, Y, W, H);
        relayout();
    }
};

class PickButton : public Fl_Button {
public:
    PickButton() : Fl_Button(0, 0, 10, 10)
    {
        box(FL_FLAT_BOX);
        clear_visible_focus();
    }

    void draw()
    {
        const char *l = label() ? label() : "";
        const char *nl = strchr(l, '\n');
        std::string top = nl ? std::string(l, nl - l) : std::string(l);
        std::string sub = nl ? std::string(nl + 1) : std::string();
        bool down = value() || g_picker->visible();

        fl_color(down ? fl_color_average(C_ACCENT, C_HEADER, 0.15f) : C_HEADER);
        fl_rectf(x(), y(), w(), h());

        fl_push_clip(x(), y(), w() - 28, h());
        fl_font(FL_HELVETICA_BOLD, 16);
        fl_color(C_TEXT);
        fl_draw(top.c_str(), x() + 4, y() + h() / 2 - 3);
        fl_font(FL_HELVETICA, 12);
        fl_color(C_SUBTEXT);
        fl_draw(sub.c_str(), x() + 4, y() + h() / 2 + 14);
        fl_pop_clip();

        fl_color(C_SUBTEXT);
        fl_draw_symbol(g_picker->visible() ? "@8>" : "@2>",
                       x() + w() - 24, y() + h() / 2 - 8, 16, 16, C_SUBTEXT);
    }
};

class FavButton : public Fl_Button {
public:
    FavButton() : Fl_Button(0, 0, 10, 10)
    {
        box(FL_FLAT_BOX);
        clear_visible_focus();
    }

    void draw()
    {
        bool on = value() != 0;
        Fl_Color c = !active_r() ? C_KEY_TEXT_OFF : on ? C_YELLOW : C_SUBTEXT;
        int cx = x() + w() / 2;
        int cy = y() + h() / 2;
        int r = 13;

        fl_color(C_HEADER);
        fl_rectf(x(), y(), w(), h());

        fl_color(c);
        if (on)
            fl_begin_polygon();
        else
            fl_begin_loop();
        for (int i = 0; i < 10; i++) {
            double a = -M_PI / 2 + i * M_PI / 5;
            double rr = (i % 2) ? r * 0.45 : r;
            fl_vertex(cx + rr * cos(a), cy + rr * sin(a));
        }
        if (on)
            fl_end_polygon();
        else
            fl_end_loop();
    }

    int handle(int e)
    {
        if (e == FL_PUSH && active_r()) {
            do_callback();
            return 1;
        }
        return e == FL_RELEASE ? 1 : Fl_Button::handle(e);
    }
};

class TouchBrowser : public Fl_Hold_Browser {
public:
    TouchBrowser() : Fl_Hold_Browser(0, 0, 10, 10) {}

protected:
    int item_height(void *item) const
    {
        return Fl_Hold_Browser::item_height(item) + 14;
    }

    void item_draw(void *item, int X, int Y, int W, int H) const
    {
        Fl_Hold_Browser::item_draw(item, X + 4, Y, W - 4, H);
        fl_color(C_LINE);
        fl_line(X, Y + H - 1, X + W, Y + H - 1);
    }
};

class SearchInput : public Fl_Input {
public:
    SearchInput() : Fl_Input(0, 0, 10, 10) {}

    void draw()
    {
        Fl_Input::draw();
        if (!size() && Fl::focus() != this) {
            fl_font(FL_HELVETICA_ITALIC, textsize());
            fl_color(C_KEY_TEXT_OFF);
            fl_draw("Search make, keyset or model", x() + 8, y(), w() - 16,
                    h(), FL_ALIGN_LEFT);
        }
    }
};

static Fl_Hold_Browser *make_browser(void)
{
    Fl_Hold_Browser *b = new TouchBrowser();

    b->textsize(16);
    b->box(FL_FLAT_BOX);
    b->color(FL_WHITE);
    b->selection_color(C_ACCENT);
    b->scrollbar_size(28);
    b->scrollbar.slider(FL_FLAT_BOX);
    b->scrollbar.color(C_KEY_OFF, C_KEY_TEXT_OFF);
    b->scrollbar.box(FL_FLAT_BOX);
    b->scrollbar.labelcolor(C_SUBTEXT);
    b->when(FL_WHEN_RELEASE_ALWAYS);
    b->clear_visible_focus();
    return b;
}

int main(int argc, char **argv)
{
    Fl::lock();
    Fl::scheme("none");

    remote::load(wake);
    std::string last = load_config();

    RemoteWindow win(Fl::w(), Fl::h(), "Remote");
    g_win = &win;
    win.color(fl_rgb_color(0xF6, 0xF6, 0xF4));
    win.begin();

    g_header = new Fl_Box(0, 0, 10, HEADER_H);
    g_header->box(FL_FLAT_BOX);
    g_header->color(C_HEADER);

    g_pick = new PickButton();
    g_pick->callback(pick_cb);

    g_fav = new FavButton();
    g_fav->callback(fav_cb);

    g_tabs = new Fl_Group(0, 0, 10, TAB_H);
    g_tabs->box(FL_FLAT_BOX);
    g_tabs->color(C_HEADER);
    g_tabs->end();

    g_panel = new Fl_Group(0, 0, 10, 10);
    g_panel->box(FL_FLAT_BOX);
    g_panel->color(win.color());
    g_panel->end();

    g_picker = new Fl_Group(0, 0, 10, 10);
    g_picker->box(FL_FLAT_BOX);
    g_picker->color(win.color());

    g_search = new SearchInput();
    g_search->textsize(16);
    g_search->box(FL_FLAT_BOX);
    g_search->color(FL_WHITE);
    g_search->when(FL_WHEN_CHANGED);
    g_search->callback(search_cb);

    g_cancel = new Fl_Button(0, 0, 10, 10, "Close");
    g_cancel->labelsize(14);
    g_cancel->box(FL_FLAT_BOX);
    g_cancel->color(C_KEY);
    g_cancel->clear_visible_focus();
    g_cancel->callback(cancel_cb);

    g_makes = make_browser();
    g_makes->callback(makes_cb);

    g_list = make_browser();
    g_list->callback(list_cb);

    g_picker->end();
    g_picker->hide();

    g_status = new Fl_Box(0, 0, 10, STATUS_H);
    g_status->box(FL_FLAT_BOX);
    g_status->color(C_HEADER);
    g_status->labelsize(12);
    g_status->labelcolor(C_SUBTEXT);
    g_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);

    g_enable = new Fl_Button(0, 0, ENABLE_W, STATUS_H, "Enable IR");
    g_enable->labelsize(12);
    g_enable->box(FL_FLAT_BOX);
    g_enable->color(C_ACCENT);
    g_enable->labelcolor(FL_WHITE);
    g_enable->clear_visible_focus();
    g_enable->callback(enable_cb);

    win.end();
    win.resizable(NULL);
    win.show(argc, argv);

    build_panel();
    update_header();

    size_t tab = last.find('\t');
    const RemoteKeyset *k = tab == std::string::npos
        ? NULL : remote::find(last.substr(0, tab), last.substr(tab + 1));

    apply_ir_state(ir_available());

    if (k)
        select_keyset(k->id);
    else
        open_picker();

    if (!g_ir_on)
        status("Infrared is off -- tap Enable IR");

    Fl::add_timeout(POLL_SECONDS, poll_cb);

    return Fl::run();
}
