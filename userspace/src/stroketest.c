#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_DEFAULT	"/mnt/card/.zaurus/stroketesting.log"

#define MAX_POINTS	8192
#define POINTS_MARK	50
#define BAR_FULLSCALE	400
#define BAR_HEIGHT	12
#define BAR_MARGIN	8

typedef struct {
	int x, y;
	unsigned long t;
} Point;

static Display *dpy;
static int screen;
static Window win;
static GC gc_ink, gc_bar, gc_mark;
static XftDraw *xftdraw;
static XftFont *xftfont;
static XftColor xftcolor;
static Atom wm_delete;

static int win_w, win_h;

static Point pts[MAX_POINTS];
static int n_stored;
static int n_seen;
static int tracking;
static int stroke_n;

static int last_seen;
static int last_stored;
static unsigned long last_ms;
static int have_last;

static FILE *logf;

static int
log_open(const char *path)
{
	char dir[512];
	char *slash;

	if (strlen(path) >= sizeof(dir)) {
		fprintf(stderr, "stroketest: log path too long: %s\n", path);
		return -1;
	}
	strcpy(dir, path);
	slash = strrchr(dir, '/');
	if (slash && slash != dir) {
		*slash = '\0';
		mkdir(dir, 0755);
	}

	logf = fopen(path, "a");
	if (!logf) {
		perror(path);
		return -1;
	}
	return 0;
}

static void
log_devices(void)
{
	FILE *f;
	char line[256];

	f = fopen("/proc/bus/input/devices", "r");
	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		char *nl;

		if (strncmp(line, "N: Name=", 8) != 0)
			continue;
		nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		fprintf(logf, "input %s\n", line + 8);
	}
	fclose(f);
}

static void
log_session(void)
{
	fprintf(logf, "session %ld screen %dx%d depth %d\n",
		(long)time(NULL),
		DisplayWidth(dpy, screen), DisplayHeight(dpy, screen),
		DefaultDepth(dpy, screen));
	log_devices();
	fflush(logf);
}

static void
log_stroke(void)
{
	int min_x = pts[0].x, max_x = pts[0].x;
	int min_y = pts[0].y, max_y = pts[0].y;
	unsigned long t0 = pts[0].t;
	int i;

	for (i = 1; i < n_stored; i++) {
		if (pts[i].x < min_x)
			min_x = pts[i].x;
		if (pts[i].x > max_x)
			max_x = pts[i].x;
		if (pts[i].y < min_y)
			min_y = pts[i].y;
		if (pts[i].y > max_y)
			max_y = pts[i].y;
	}

	fprintf(logf, "stroke %d points %d stored %d ms %lu bbox %d %d %d %d\n",
		stroke_n, n_seen, n_stored,
		pts[n_stored - 1].t - t0,
		min_x, min_y, max_x, max_y);

	for (i = 0; i < n_stored; i++)
		fprintf(logf, "p %lu %d %d\n",
			pts[i].t - t0, pts[i].x, pts[i].y);

	fflush(logf);
	fsync(fileno(logf));
}

static void
add_point(int x, int y, unsigned long t)
{
	n_seen++;
	if (n_stored >= MAX_POINTS)
		return;
	pts[n_stored].x = x;
	pts[n_stored].y = y;
	pts[n_stored].t = t;
	n_stored++;
}

static void
draw_readout(void)
{
	int bar_w, bar_x, bar_y, mark_x;
	char text[64];

	if (!have_last)
		return;

	bar_y = win_h - BAR_MARGIN - BAR_HEIGHT;
	bar_x = BAR_MARGIN;
	bar_w = (win_w - 2 * BAR_MARGIN) * last_seen / BAR_FULLSCALE;
	if (bar_w > win_w - 2 * BAR_MARGIN)
		bar_w = win_w - 2 * BAR_MARGIN;
	if (bar_w < 1)
		bar_w = 1;
	mark_x = bar_x + (win_w - 2 * BAR_MARGIN) * POINTS_MARK / BAR_FULLSCALE;

	XFillRectangle(dpy, win, gc_bar, bar_x, bar_y, (unsigned)bar_w,
		       BAR_HEIGHT);
	XDrawLine(dpy, win, gc_mark, mark_x, bar_y - 4, mark_x,
		  bar_y + BAR_HEIGHT + 4);

	if (!xftfont)
		return;

	if (last_stored < last_seen)
		snprintf(text, sizeof(text), "%d pts  %lu ms  stored %d",
			 last_seen, last_ms, last_stored);
	else
		snprintf(text, sizeof(text), "%d pts  %lu ms", last_seen,
			 last_ms);

	XftDrawStringUtf8(xftdraw, &xftcolor, xftfont, BAR_MARGIN,
			  bar_y - 8, (const FcChar8 *)text,
			  (int)strlen(text));
}

static void
redraw(void)
{
	int i;

	XClearWindow(dpy, win);
	for (i = 1; i < n_stored; i++)
		XDrawLine(dpy, win, gc_ink, pts[i - 1].x, pts[i - 1].y,
			  pts[i].x, pts[i].y);
	if (n_stored == 1)
		XDrawPoint(dpy, win, gc_ink, pts[0].x, pts[0].y);
	draw_readout();
}

static void
stroke_begin(int x, int y, unsigned long t)
{
	n_stored = 0;
	n_seen = 0;
	tracking = 1;
	XClearWindow(dpy, win);
	add_point(x, y, t);
}

static void
stroke_extend(int x, int y, unsigned long t)
{
	int prev = n_stored - 1;

	if (n_stored > 0 && n_stored < MAX_POINTS)
		XDrawLine(dpy, win, gc_ink, pts[prev].x, pts[prev].y, x, y);
	add_point(x, y, t);
}

static void
stroke_end(void)
{
	tracking = 0;
	if (n_stored == 0)
		return;

	stroke_n++;
	log_stroke();

	last_seen = n_seen;
	last_stored = n_stored;
	last_ms = pts[n_stored - 1].t - pts[0].t;
	have_last = 1;

	draw_readout();
}

static void
setup_gcs(void)
{
	XGCValues v;

	v.foreground = BlackPixel(dpy, screen);
	v.line_width = 2;
	gc_ink = XCreateGC(dpy, win, GCForeground | GCLineWidth, &v);

	v.foreground = BlackPixel(dpy, screen);
	v.line_width = 1;
	gc_bar = XCreateGC(dpy, win, GCForeground | GCLineWidth, &v);

	v.foreground = BlackPixel(dpy, screen);
	v.line_width = 1;
	v.line_style = LineOnOffDash;
	gc_mark = XCreateGC(dpy, win,
			    GCForeground | GCLineWidth | GCLineStyle, &v);
}

static void
setup_font(void)
{
	XRenderColor rc;

	xftfont = XftFontOpenName(dpy, screen, "DejaVu Sans-9");
	if (!xftfont)
		return;

	xftdraw = XftDrawCreate(dpy, win, DefaultVisual(dpy, screen),
				DefaultColormap(dpy, screen));
	if (!xftdraw) {
		XftFontClose(dpy, xftfont);
		xftfont = NULL;
		return;
	}

	rc.red = 0;
	rc.green = 0;
	rc.blue = 0;
	rc.alpha = 0xffff;
	if (!XftColorAllocValue(dpy, DefaultVisual(dpy, screen),
				DefaultColormap(dpy, screen), &rc, &xftcolor)) {
		XftDrawDestroy(xftdraw);
		XftFontClose(dpy, xftfont);
		xftdraw = NULL;
		xftfont = NULL;
	}
}

int
main(int argc, char **argv)
{
	const char *path = LOG_DEFAULT;
	XSetWindowAttributes attr;
	XTextProperty name;
	char *title = (char *)"stroketest";
	int running = 1;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [logfile]\n", argv[0]);
		return 2;
	}
	if (argc == 2)
		path = argv[1];

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "stroketest: cannot open display\n");
		return 1;
	}
	screen = DefaultScreen(dpy);

	if (log_open(path) < 0) {
		XCloseDisplay(dpy);
		return 1;
	}

	win_w = DisplayWidth(dpy, screen);
	win_h = DisplayHeight(dpy, screen);

	attr.background_pixel = WhitePixel(dpy, screen);
	attr.event_mask = ButtonPressMask | ButtonReleaseMask |
			  Button1MotionMask | ExposureMask | KeyPressMask |
			  StructureNotifyMask;
	win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0,
			    (unsigned)win_w, (unsigned)win_h, 0,
			    CopyFromParent, InputOutput, CopyFromParent,
			    CWBackPixel | CWEventMask, &attr);

	if (XStringListToTextProperty(&title, 1, &name)) {
		XSetWMName(dpy, win, &name);
		XFree(name.value);
	}

	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);

	setup_gcs();
	setup_font();

	log_session();

	XMapWindow(dpy, win);

	while (running) {
		XEvent ev;

		XNextEvent(dpy, &ev);

		switch (ev.type) {
		case ConfigureNotify:
			if (ev.xconfigure.width != win_w ||
			    ev.xconfigure.height != win_h) {
				win_w = ev.xconfigure.width;
				win_h = ev.xconfigure.height;
				fprintf(logf, "resize %dx%d\n", win_w, win_h);
				fflush(logf);
			}
			break;
		case Expose:
			if (ev.xexpose.count == 0)
				redraw();
			break;
		case ButtonPress:
			if (ev.xbutton.button == Button1)
				stroke_begin(ev.xbutton.x, ev.xbutton.y,
					     ev.xbutton.time);
			break;
		case MotionNotify:
			if (tracking)
				stroke_extend(ev.xmotion.x, ev.xmotion.y,
					      ev.xmotion.time);
			break;
		case ButtonRelease:
			if (ev.xbutton.button == Button1 && tracking) {
				stroke_extend(ev.xbutton.x, ev.xbutton.y,
					      ev.xbutton.time);
				stroke_end();
			}
			break;
		case KeyPress:
			if (XLookupKeysym(&ev.xkey, 0) == XK_q)
				running = 0;
			break;
		case ClientMessage:
			if ((Atom)ev.xclient.data.l[0] == wm_delete)
				running = 0;
			break;
		}
	}

	fprintf(logf, "done %d strokes\n", stroke_n);
	fclose(logf);

	if (xftfont) {
		XftColorFree(dpy, DefaultVisual(dpy, screen),
			     DefaultColormap(dpy, screen), &xftcolor);
		XftDrawDestroy(xftdraw);
		XftFontClose(dpy, xftfont);
	}
	XCloseDisplay(dpy);
	return 0;
}
