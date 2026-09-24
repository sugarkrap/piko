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

#define PAD		8
#define ROW_H		38
#define STATUS_H	26
#define PAGE_H		28
#define IR_MAX_ROWS	32
#define GAP		6
#define CELL_H		52

#define LIRC_DEV	"/dev/lirc0"
#define IRMODE_BIN	"/usr/sbin/irmode"

static Fl_Choice	*g_devices;
static Fl_Scroll	*g_grid;
static Fl_Box		*g_status;
static Fl_Button	*g_rename;
static Fl_Button	*g_delete;
static Fl_Button	*g_main;
static Fl_Button	*g_custom;
static Fl_Button	*g_add;
static Fl_Window	*g_win;

enum { PAGE_MAIN, PAGE_CUSTOM };
static int		 g_page = PAGE_MAIN;

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

static int landscape(void)
{
	return g_grid->w() > g_grid->h();
}

static int slot_pos(const struct ir_slot *s, int *row, int *col, int *span)
{
	if (landscape()) {
		*row = s->l_row;
		*col = s->l_col;
		*span = s->l_w;
	} else {
		*row = s->p_row;
		*col = s->p_col;
		*span = s->p_w;
	}

	return *row >= 0 && *col >= 0;
}

static void style_button(Fl_Button *btn, const struct ir_button *b)
{
	char safe[IR_NAME_MAX + IR_GLYPH_MAX + 4];

	if (b->glyph[0])
		snprintf(safe, sizeof(safe), "%s  %s", b->glyph, b->label);
	else
		fl_safe(b->label, safe, sizeof(safe));

	btn->copy_label(safe);
	btn->labelsize(13);

	if (b->major) {
		btn->color(fl_rgb_color(0xC0, 0x54, 0x44));
		btn->labelcolor(FL_WHITE);
		btn->labelfont(FL_HELVETICA_BOLD);
	}

	btn->callback(button_cb, (void *)b->key);
}

static void rebuild_grid(void)
{
	int cols = landscape() ? IR_COLS_LANDSCAPE : IR_COLS_PORTRAIT;
	int inner = g_grid->w() - Fl::scrollbar_size() - GAP * 2;
	int cell_w = (inner - (cols - 1) * GAP) / cols;
	int x0 = g_grid->x() + GAP;
	int y0 = g_grid->y() + GAP;
	unsigned int i;
	int placed = 0;
	char used[IR_MAX_ROWS];
	int rowmap[IR_MAX_ROWS];
	int shown = 0;

	memset(used, 0, sizeof(used));

	if (g_dev && g_page == PAGE_MAIN) {
		for (i = 0; i < g_dev->count; i++) {
			const struct ir_slot *sl = ir_slot_for(g_dev->buttons[i].key);
			int row, col, span;

			if (!sl || !slot_pos(sl, &row, &col, &span))
				continue;
			if (row >= 0 && row < IR_MAX_ROWS)
				used[row] = 1;
		}
	}

	for (i = 0; i < IR_MAX_ROWS; i++)
		rowmap[i] = used[i] ? shown++ : -1;

	g_grid->clear();
	g_grid->begin();

	if (!g_dev) {
		Fl_Box *hint = new Fl_Box(x0, y0, inner, CELL_H,
					  "no device yet -- tap New");
		hint->labelsize(12);
		hint->labelcolor(fl_rgb_color(0x70, 0x70, 0x70));
		g_grid->end();
		g_grid->redraw();
		return;
	}

	for (i = 0; i < g_dev->count; i++) {
		struct ir_button *b = &g_dev->buttons[i];
		const struct ir_slot *s = ir_slot_for(b->key);
		int row, col, span;
		Fl_Button *btn;

		if (g_page == PAGE_MAIN) {
			if (!s || !slot_pos(s, &row, &col, &span))
				continue;
			if (row < 0 || row >= IR_MAX_ROWS
			    || rowmap[row] < 0)
				continue;
			row = rowmap[row];
		} else {
			if (s)
				continue;
			row = placed / cols;
			col = placed % cols;
			span = 1;
		}

		btn = new Fl_Button(x0 + col * (cell_w + GAP),
				    y0 + row * (CELL_H + GAP),
				    cell_w * span + GAP * (span - 1), CELL_H);
		style_button(btn, b);
		placed++;
	}

	if (!placed) {
		Fl_Box *hint = new Fl_Box(x0, y0, inner, CELL_H,
					  g_page == PAGE_MAIN
					  ? "no standard buttons learned yet"
					  : "no custom buttons yet");
		hint->labelsize(12);
		hint->labelcolor(fl_rgb_color(0x70, 0x70, 0x70));
	}

	g_grid->end();
	g_grid->redraw();
}

static void page_cb(Fl_Widget *w, void *)
{
	g_page = (w == (Fl_Widget *)g_custom) ? PAGE_CUSTOM : PAGE_MAIN;
	g_main->value(g_page == PAGE_MAIN);
	g_custom->value(g_page == PAGE_CUSTOM);
	rebuild_grid();
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

static void relayout(void)
{
	int w, h, y;
	int edit_w = 56;
	int choice_w;
	int page_w = 96;

	if (!g_win || !g_grid || !g_status)
		return;

	w = g_win->w();
	h = g_win->h();
	y = PAD;
	choice_w = w - PAD * 2 - GAP * 3 - edit_w * 3;

	g_devices->resize(PAD, y, choice_w, ROW_H);
	g_add->resize(PAD + choice_w + GAP, y, edit_w, ROW_H);
	g_rename->resize(PAD + choice_w + GAP * 2 + edit_w, y, edit_w, ROW_H);
	g_delete->resize(PAD + choice_w + GAP * 3 + edit_w * 2, y, edit_w,
			 ROW_H);

	y += ROW_H + GAP;
	g_main->resize(PAD, y, page_w, PAGE_H);
	g_custom->resize(PAD + page_w + GAP, y, page_w, PAGE_H);

	y += PAGE_H + GAP;
	g_grid->resize(PAD, y, w - PAD * 2, h - y - STATUS_H - PAD);
	g_status->resize(PAD, h - STATUS_H, w - PAD * 2, STATUS_H - 2);

	rebuild_grid();
}

class RemoteWindow : public Fl_Double_Window {
public:
	RemoteWindow(int w, int h, const char *l)
		: Fl_Double_Window(w, h, l) { }

	void resize(int X, int Y, int W, int H) {
		Fl_Double_Window::resize(X, Y, W, H);
		relayout();
	}
};

int main(int argc, char **argv)
{
	RemoteWindow win(Fl::w(), Fl::h(), "Remote");

	g_win = &win;
	win.begin();

	g_devices = new Fl_Choice(PAD, PAD, 100, ROW_H);
	g_devices->callback(device_cb);

	g_add = new Fl_Button(0, 0, 10, ROW_H, "New");
	g_add->callback(new_cb);

	g_rename = new Fl_Button(0, 0, 10, ROW_H, "Name");
	g_rename->callback(rename_cb);

	g_delete = new Fl_Button(0, 0, 10, ROW_H, "Del");
	g_delete->callback(delete_cb);

	g_main = new Fl_Button(0, 0, 10, PAGE_H, "Buttons");
	g_main->type(FL_RADIO_BUTTON);
	g_main->labelsize(12);
	g_main->value(1);
	g_main->callback(page_cb);

	g_custom = new Fl_Button(0, 0, 10, PAGE_H, "Custom");
	g_custom->type(FL_RADIO_BUTTON);
	g_custom->labelsize(12);
	g_custom->callback(page_cb);

	g_grid = new Fl_Scroll(PAD, PAD, 100, 100);
	g_grid->box(FL_DOWN_BOX);
	g_grid->type(Fl_Scroll::VERTICAL);
	g_grid->end();

	g_status = new Fl_Box(0, 0, 10, STATUS_H);
	g_status->box(FL_FLAT_BOX);
	g_status->labelsize(11);
	g_status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

	win.end();
	win.resizable(g_grid);
	win.show(argc, argv);

	relayout();

	if (ir_store_ensure())
		statusf("cannot open %s -- is the card mounted?",
			ir_store_dir());
	else
		reload_devices(NULL);

	claim();
	if (g_lirc < 0)
		statusf("no %s yet -- run  irmode remote", LIRC_DEV);

	return Fl::run();
}
