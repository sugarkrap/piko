#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Multiline_Output.H>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

#define HEADER_H        52
#define PAD             12
#define ROW_H           40
#define SEND_H          48
#define STATUS_H        44

#define IRMODE_BIN      "/usr/sbin/irmode"
#define IR_BIN          "/usr/bin/ir"
#define FRAME_PATH      "/mnt/card/frame1.ir"
#define EDGES_PARAM     "/sys/module/piko_cir/parameters/rx_edges"
#define GAP_PARAM       "/sys/module/piko_cir/parameters/carrier_gap"

#define REFRESH_SECONDS 2.0

static Fl_Box            *g_status;
static Fl_Button         *g_irda;
static Fl_Button         *g_remote;
static Fl_Button         *g_off;
static Fl_Button         *g_send;
static Fl_Multiline_Output *g_output;

static std::string        g_status_text;
static std::string        g_mode;

static bool run_command(const char *command, std::string &output)
{
    FILE *pipe = popen(command, "r");
    char line[256];

    output.clear();

    if (!pipe) {
        output = "cannot run ";
        output += command;
        return false;
    }

    while (fgets(line, sizeof(line), pipe)) {
        output += line;
    }

    return pclose(pipe) == 0;
}

static std::string read_param(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[64];
    std::string value;

    if (!file) {
        return value;
    }

    if (fgets(line, sizeof(line), file)) {
        value = line;
        while (!value.empty()
               && (value[value.size() - 1] == '\n'
                   || value[value.size() - 1] == '\r')) {
            value.erase(value.size() - 1);
        }
    }

    fclose(file);

    return value;
}

static std::string current_mode(void)
{
    std::string output;
    std::string mode;
    size_t colon;

    run_command(IRMODE_BIN " status", output);

    colon = output.find(':');
    if (colon == std::string::npos) {
        return mode;
    }

    mode = output.substr(colon + 1);
    while (!mode.empty() && (mode[0] == ' ' || mode[0] == '\t')) {
        mode.erase(0, 1);
    }
    colon = mode.find('\n');
    if (colon != std::string::npos) {
        mode.erase(colon);
    }

    return mode;
}

static void refresh(void)
{
    std::string edges = read_param(EDGES_PARAM);
    std::string gap = read_param(GAP_PARAM);
    struct stat frame_stat;

    g_mode = current_mode();

    g_status_text = "Mode: " + (g_mode.empty() ? std::string("unknown") : g_mode);
    if (!edges.empty()) {
        g_status_text += "     Edges: " + edges;
    }
    if (!gap.empty()) {
        g_status_text += "     Gap: " + gap + " us";
    }
    g_status->label(g_status_text.c_str());

    if (g_mode == "remote" && stat(FRAME_PATH, &frame_stat) == 0) {
        g_send->activate();
    } else {
        g_send->deactivate();
    }

    g_irda->value(g_mode == "irda");
    g_remote->value(g_mode == "remote");
    g_off->value(g_mode == "off");
}

static void set_output(const std::string &text)
{
    g_output->value(text.c_str());
    g_output->position(0);
}

static void busy(const char *what)
{
    g_status_text = what;
    g_status->label(g_status_text.c_str());
    g_irda->deactivate();
    g_remote->deactivate();
    g_off->deactivate();
    g_send->deactivate();
    Fl::flush();
}

static void done(void)
{
    g_irda->activate();
    g_remote->activate();
    g_off->activate();
    refresh();
}

static void switch_mode(const char *mode, const char *busy_text)
{
    std::string command = std::string(IRMODE_BIN " ") + mode + " 2>&1";
    std::string output;

    busy(busy_text);
    run_command(command.c_str(), output);
    set_output(output);
    done();
}

static void irda_cb(Fl_Widget *, void *)
{
    switch_mode("irda", "Loading the IrDA stack...");
}

static void remote_cb(Fl_Widget *, void *)
{
    switch_mode("remote", "Loading the consumer IR stack...");
}

static void off_cb(Fl_Widget *, void *)
{
    switch_mode("off", "Unloading...");
}

static void send_cb(Fl_Widget *, void *)
{
    std::string output;
    bool sent;

    busy("Sending " FRAME_PATH "...");
    sent = run_command(IR_BIN " tx " FRAME_PATH " 2>&1", output);
    if (output.empty()) {
        output = sent ? "Sent " FRAME_PATH : "ir tx failed";
    }
    set_output(output);
    done();
}

static void tick(void *)
{
    refresh();
    Fl::repeat_timeout(REFRESH_SECONDS, tick);
}

int main(int argc, char **argv)
{
    int win_w = Fl::w();
    int win_h = Fl::h();
    int button_w = (win_w - PAD * 4) / 3;
    int row_y = HEADER_H + STATUS_H + PAD * 2;
    int send_y = row_y + ROW_H + PAD;
    int output_y = send_y + SEND_H + PAD;

    Fl_Double_Window win(win_w, win_h, "IR test");
    win.color(FL_BACKGROUND_COLOR);
    win.begin();

    Fl_Box *header = new Fl_Box(0, 0, win_w, HEADER_H);
    header->box(FL_FLAT_BOX);
    header->color(fl_rgb_color(0xF0, 0xF0, 0xEC));

    Fl_Box *title = new Fl_Box(PAD, 6, win_w - PAD * 2, 22, "IR test");
    title->labelfont(FL_HELVETICA_BOLD);
    title->labelsize(16);
    title->labelcolor(fl_rgb_color(0x20, 0x20, 0x20));
    title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    Fl_Box *subtitle = new Fl_Box(PAD, 28, win_w - PAD * 2, 16,
                                  "IrDA and consumer IR share one transceiver");
    subtitle->labelsize(11);
    subtitle->labelcolor(fl_rgb_color(0x60, 0x60, 0x60));
    subtitle->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    g_status = new Fl_Box(PAD, HEADER_H + PAD, win_w - PAD * 2, STATUS_H);
    g_status->box(FL_DOWN_BOX);
    g_status->labelsize(13);
    g_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    g_irda = new Fl_Button(PAD, row_y, button_w, ROW_H, "IrDA");
    g_irda->type(FL_RADIO_BUTTON);
    g_irda->callback(irda_cb);

    g_remote = new Fl_Button(PAD * 2 + button_w, row_y, button_w, ROW_H,
                             "Remote");
    g_remote->type(FL_RADIO_BUTTON);
    g_remote->callback(remote_cb);

    g_off = new Fl_Button(PAD * 3 + button_w * 2, row_y, button_w, ROW_H,
                          "Off");
    g_off->type(FL_RADIO_BUTTON);
    g_off->callback(off_cb);

    g_send = new Fl_Button(PAD, send_y, win_w - PAD * 2, SEND_H,
                           "Send " FRAME_PATH);
    g_send->labelfont(FL_HELVETICA_BOLD);
    g_send->callback(send_cb);
    g_send->deactivate();

    g_output = new Fl_Multiline_Output(PAD, output_y, win_w - PAD * 2,
                                       win_h - output_y - PAD);
    g_output->textsize(11);
    g_output->box(FL_DOWN_BOX);

    win.end();
    win.resizable(g_output);
    win.show(argc, argv);

    refresh();
    Fl::add_timeout(REFRESH_SECONDS, tick);

    return Fl::run();
}
