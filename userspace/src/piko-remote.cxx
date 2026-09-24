#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_ask.H>

#include <fcntl.h>
#include <stdarg.h>
#include <linux/lirc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "irstore.h"

#define HEADER_H	52
#define PAD		12
#define ROW_H		40
#define STATUS_H	36
#define GAP		8
#define COLS		5
#define CELL_H		56

#define LIRC_DEV	"/dev/lirc0"
#define IRMODE_BIN	"/usr/sbin/irmode"

static Fl_Choice	*g_devices;
static Fl_Scroll	*g_grid;
static Fl_Box		*g_status;
static Fl_Button	*g_rename;
static Fl_Button	*g_delete;

static struct ir_device	*g_dev;
static int		 g_lirc = -1;
static char		 g_slugs[IR_MAX_DEVICES][IR_NAME_MAX];
static int		 g_nslugs;

static void fl_safe(const char *in, char *out, size_t n)
{
	if (in && *in == '@')
		snprintf(out, n, "@%s", in);
	else
		snprintf(out, n, "%s", in ? in : "");
}

static void status(const char *text)
{
	g_status->copy_label(text);
	g_status->redraw();
}

static void statusf(const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	status(buf);
}

static int lirc_open_lease(void)
{
	unsigned int mode = LIRC_MODE_PULSE;
	int fd = open(LIRC_DEV, O_WRONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;

	if (ioctl(fd, LIRC_SET_SEND_MODE, &mode) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static void claim(void)
{
	if (g_lirc >= 0)
		return;

	g_lirc = lirc_open_lease();
}

static int send_button(const struct ir_button *b)
{
	ssize_t want;

	if (g_lirc < 0) {
		claim();
		if (g_lirc < 0)
			return -1;
	}

	want = (ssize_t)(b->count * sizeof(b->edges[0]));

	return write(g_lirc, b->edges, want) == want ? 0 : -1;
}

static int cmp_slug(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

static void rebuild_grid(void);

static void button_cb(Fl_Widget *w, void *data)
{
	const char *key = (const char *)data;
	struct ir_button *b = ir_device_find(g_dev, key);

	(void)w;

	if (!b) {
		status("that button is gone");
		return;
	}

	if (send_button(b)) {
		statusf("cannot send: is %s loaded? try  irmode remote", LIRC_DEV);
		return;
	}

	statusf("sent %s (%u edges)", b->label, b->count);
}

static void rebuild_grid(void)
{
	int w = g_grid->w() - Fl::scrollbar_size() - GAP;
	int cell_w = (w - (COLS - 1) * GAP) / COLS;
	int x0 = g_grid->x() + GAP / 2;
	int y0 = g_grid->y() + GAP / 2;
	unsigned int i;

	g_grid->clear();
	g_grid->begin();

	if (!g_dev || !g_dev->count) {
		Fl_Box *hint = new Fl_Box(x0, y0, w, CELL_H,
					  g_dev ? "no buttons learned yet"
						: "no device yet -- tap New");
		hint->labelsize(12);
		hint->labelcolor(fl_rgb_color(0x70, 0x70, 0x70));
		g_grid->end();
		g_grid->redraw();
		return;
	}

	for (i = 0; i < g_dev->count; i++) {
		struct ir_button *b = &g_dev->buttons[i];
		int col = (int)(i % COLS);
		int row = (int)(i / COLS);
		Fl_Button *btn = new Fl_Button(x0 + col * (cell_w + GAP),
					       y0 + row * (CELL_H + GAP),
					       cell_w, CELL_H);

		char safe[IR_NAME_MAX + 2];

		fl_safe(b->label, safe, sizeof(safe));
		btn->copy_label(safe);
		btn->labelsize(13);
		btn->callback(button_cb, (void *)b->key);
	}

	g_grid->end();
	g_grid->redraw();
}

static void select_device(const char *slug)
{
	ir_device_free(g_dev);
	g_dev = slug ? ir_device_load(slug) : NULL;

	if (g_dev)
		statusf("%s -- %u button%s", g_dev->label, g_dev->count,
			g_dev->count == 1 ? "" : "s");

	g_rename->activate();
	g_delete->activate();

	if (!g_dev) {
		g_rename->deactivate();
		g_delete->deactivate();
	}

	rebuild_grid();
}

static void reload_devices(const char *want)
{
	char slugs[IR_MAX_DEVICES][IR_NAME_MAX];
	int n = ir_store_list(slugs, IR_MAX_DEVICES);
	int pick = 0;
	int i;

	qsort(slugs, (size_t)n, IR_NAME_MAX, cmp_slug);

	g_devices->clear();
	g_nslugs = n;

	for (i = 0; i < n; i++) {
		struct ir_device *d = ir_device_load(slugs[i]);
		int idx;

		snprintf(g_slugs[i], IR_NAME_MAX, "%s", slugs[i]);

		char safe[IR_NAME_MAX + 2];

		fl_safe(d && d->label[0] ? d->label : slugs[i], safe,
			sizeof(safe));

		idx = g_devices->add("?", 0, (Fl_Callback *)0);
		g_devices->replace(idx, safe);

		if (want && !strcmp(slugs[i], want))
			pick = i;

		ir_device_free(d);
	}

	if (!n) {
		g_devices->value(-1);
		select_device(NULL);
		return;
	}

	g_devices->value(pick);
	select_device(g_slugs[pick]);
}

static void device_cb(Fl_Widget *, void *)
{
	int v = g_devices->value();

	if (v < 0 || v >= g_nslugs)
		return;

	select_device(g_slugs[v]);
}

static int slug_taken(const char *slug)
{
	char slugs[IR_MAX_DEVICES][IR_NAME_MAX];
	int n = ir_store_list(slugs, IR_MAX_DEVICES);
	int i;

	for (i = 0; i < n; i++) {
		if (!strcmp(slugs[i], slug))
			return 1;
	}

	return 0;
}

static void unique_slug(const char *label, char *out, size_t n)
{
	char base[IR_NAME_MAX - 4];
	int tries = 2;

	ir_slug(label, base, sizeof(base));
	snprintf(out, n, "%s", base);

	while (slug_taken(out) && tries < 100)
		snprintf(out, n, "%s-%d", base, tries++);
}

static void new_cb(Fl_Widget *, void *)
{
	const char *name = fl_input("%s", "hi-fi", "Name the device");
	struct ir_device *dev;

	if (!name || !*name)
		return;

	dev = ir_device_new(name);
	if (!dev) {
		status("out of memory");
		return;
	}

	unique_slug(name, dev->slug, sizeof(dev->slug));

	if (ir_device_save(dev)) {
		statusf("cannot write to %s", ir_store_dir());
		ir_device_free(dev);
		return;
	}

	reload_devices(dev->slug);
	statusf("created %s", dev->label);
	ir_device_free(dev);
}

static void rename_cb(Fl_Widget *, void *)
{
	const char *name;

	if (!g_dev)
		return;

	name = fl_input("%s", g_dev->label, "Rename the device");
	if (!name || !*name)
		return;

	ir_device_relabel(g_dev, name);

	if (ir_device_save(g_dev)) {
		statusf("cannot write to %s", ir_store_dir());
		return;
	}

	reload_devices(g_dev->slug);
}

static void delete_cb(Fl_Widget *, void *)
{
	char slug[IR_NAME_MAX];

	if (!g_dev)
		return;

	if (fl_choice("Delete %s and everything it has learned?",
		      "Cancel", "Delete", 0, g_dev->label) != 1)
		return;

	snprintf(slug, sizeof(slug), "%s", g_dev->slug);

	if (ir_device_delete(slug)) {
		status("could not delete it");
		return;
	}

	reload_devices(NULL);
	statusf("deleted %s", slug);
}

int main(int argc, char **argv)
{
	int win_w = Fl::w();
	int win_h = Fl::h();
	int row_y = HEADER_H + PAD;
	int grid_y = row_y + ROW_H + PAD;
	int grid_h = win_h - grid_y - STATUS_H - PAD;
	int edit_w = (win_w - PAD * 2 - GAP * 3) / 6;
	int choice_w = win_w - PAD * 2 - GAP * 3 - edit_w * 3;

	Fl_Double_Window win(win_w, win_h, "Remote");
	win.begin();

	Fl_Box *header = new Fl_Box(0, 0, win_w, HEADER_H);
	header->box(FL_FLAT_BOX);
	header->color(fl_rgb_color(0xF0, 0xF0, 0xEC));

	Fl_Box *title = new Fl_Box(PAD, 6, win_w - PAD * 2, 22, "Remote");
	title->labelfont(FL_HELVETICA_BOLD);
	title->labelsize(16);
	title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	Fl_Box *sub = new Fl_Box(PAD, 28, win_w - PAD * 2, 16,
				 "learned infrared, one file per device");
	sub->labelsize(11);
	sub->labelcolor(fl_rgb_color(0x60, 0x60, 0x60));
	sub->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	g_devices = new Fl_Choice(PAD, row_y, choice_w, ROW_H);
	g_devices->callback(device_cb);

	Fl_Button *add = new Fl_Button(PAD + choice_w + GAP, row_y, edit_w,
				       ROW_H, "New");
	add->callback(new_cb);

	g_rename = new Fl_Button(PAD + choice_w + GAP * 2 + edit_w, row_y,
				 edit_w, ROW_H, "Name");
	g_rename->callback(rename_cb);

	g_delete = new Fl_Button(PAD + choice_w + GAP * 3 + edit_w * 2, row_y,
				 edit_w, ROW_H, "Del");
	g_delete->callback(delete_cb);

	g_grid = new Fl_Scroll(PAD, grid_y, win_w - PAD * 2, grid_h);
	g_grid->box(FL_DOWN_BOX);
	g_grid->type(Fl_Scroll::VERTICAL);
	g_grid->end();

	g_status = new Fl_Box(PAD, win_h - STATUS_H, win_w - PAD * 2,
			      STATUS_H - PAD / 2);
	g_status->box(FL_FLAT_BOX);
	g_status->labelsize(12);
	g_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	win.end();
	win.resizable(g_grid);
	win.show(argc, argv);

	if (ir_store_ensure()) {
		statusf("cannot open %s -- is the card mounted?",
			ir_store_dir());
	} else {
		reload_devices(NULL);
	}

	claim();
	if (g_lirc < 0)
		statusf("no %s yet -- run  irmode remote", LIRC_DEV);

	return Fl::run();
}
