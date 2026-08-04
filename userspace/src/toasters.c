/*
 * toasters -- the "flying toasters" screensaver, for the Zaurus.
 *
 * The artwork and the motion are from torunar/flying-toasters-xscreensaver
 * (MIT, Copyright (c) 2023 Mikhail Shchekotov), vendored as the submodule
 * userspace/src/flying-toasters -- see its LICENSE.
 */

#include <X11/Xlib.h>
#include <X11/xpm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#include "flying-toasters/img/toaster.xpm"
#include "flying-toasters/img/toast.xpm"

#define SPRITE_SIZE     64
#define TOASTER_FRAMES  6

#define N_TOASTERS      5
#define N_TOAST         3

#define FRAME_MS        100   /* ~10fps -- see the header on why not 60 */
#define FLAP_EVERY      2     /* advance the wing frame every N frames */
#define MIN_SPEED       4     /* px per frame, so 40..80 px/s at 10fps */
#define MAX_SPEED       8

#define BATTERY_ICON_W         32
#define BATTERY_ICON_H         14
#define BATTERY_MARGIN         10
#define BATTERY_BORDER_INSET   2    /* border-to-fill gap on each side */
#define BATTERY_CHECK_TICKS    20   /* ~2s at FRAME_MS=100 -- matches
				      * mb-applet-battery.c's own poll rate */

#define MIN_DIAG_GAP    (SPRITE_SIZE * 2 + SPRITE_SIZE / 2)
#define SPAWN_TRIES     40

typedef struct {
	int x, y;
	int speed;
	int frame;   /* wing position; unused for toast */
	int toast;   /* 0 = toaster, 1 = winged toast */
	int alive;   /* 0 while waiting for a clear (x+y) slot to open up */
} Sprite;

static Display *dpy;
static int screen;
static Window win;
static Pixmap backbuf;
static GC gc;
static int width, height;
static unsigned long col_bg;

static int battery_zone_x, battery_zone_y;
static int battery_deadzone = 1;   /* -B on the command line clears this */

static Pixmap toaster_img[TOASTER_FRAMES];
static Pixmap toaster_mask[TOASTER_FRAMES];
static Pixmap toast_img;
static Pixmap toast_mask;

static int
load_sprite(char **data, Pixmap *pix, Pixmap *mask)
{
	XImage *img = NULL, *shape = NULL;
	GC tgc;

	if (XpmCreateImageFromData(dpy, data, &img, &shape, NULL) != XpmSuccess)
		return -1;

	*pix = XCreatePixmap(dpy, win, img->width, img->height,
			     (unsigned)DefaultDepth(dpy, screen));
	tgc = XCreateGC(dpy, *pix, 0, NULL);
	XPutImage(dpy, *pix, tgc, img, 0, 0, 0, 0, img->width, img->height);
	XFreeGC(dpy, tgc);
	XDestroyImage(img);

	if (!shape) {
		/* No transparent colour in the sheet: no mask needed. */
		*mask = None;
		return 0;
	}
	*mask = XCreatePixmap(dpy, win, shape->width, shape->height,
			      shape->depth);
	tgc = XCreateGC(dpy, *mask, 0, NULL);
	XPutImage(dpy, *mask, tgc, shape, 0, 0, 0, 0,
		  shape->width, shape->height);
	XFreeGC(dpy, tgc);
	XDestroyImage(shape);
	return 0;
}

static void
draw_sprite(Pixmap pix, Pixmap mask, int x, int y)
{
	if (mask != None) {
		XSetClipMask(dpy, gc, mask);
		XSetClipOrigin(dpy, gc, x, y);
	}
	XCopyArea(dpy, pix, backbuf, gc, 0, 0, SPRITE_SIZE, SPRITE_SIZE, x, y);
	if (mask != None)
		XSetClipMask(dpy, gc, None);
}

static int
diag_clear(int diag, const Sprite *roster, int n, int self)
{
	int i, d;

	for (i = 0; i < n; i++) {
		if (i == self || !roster[i].alive)
			continue;
		d = diag - (roster[i].x + roster[i].y);
		if (d < 0)
			d = -d;
		if (d < MIN_DIAG_GAP)
			return 0;
	}
	return 1;
}

static int
battery_zone_clear(int diag)
{
	int delta;

	if (!battery_deadzone)
		return 1;
	delta = diag - (battery_zone_x + battery_zone_y);
	return delta <= -2 * SPRITE_SIZE
	    || delta >= BATTERY_ICON_W + BATTERY_ICON_H;
}

static int
read_battery(int *percent, int *ac)
{
	FILE *f;
	char line[256];
	char units[16];
	int ac_raw = -1, batt_status = 0, batt_flag = 0, pct = -1,
	    time_left = -1;

	f = fopen("/proc/apm", "r");
	if (!f)
		return 0;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);

	units[0] = '\0';
	if (sscanf(line, "%*s %*s %*x %x %x %x %d%% %d %15s",
		   &ac_raw, &batt_status, &batt_flag, &pct, &time_left,
		   units) < 4)
		return 0;

	*percent = pct;
	*ac = (ac_raw == 1);
	return 1;
}

static void
draw_pill(int x, int y, int w, int h)
{
	XFillArc(dpy, backbuf, gc, x, y, (unsigned)h, (unsigned)h,
		 90 * 64, 180 * 64);
	XFillArc(dpy, backbuf, gc, x + w - h, y, (unsigned)h, (unsigned)h,
		 -90 * 64, 180 * 64);
	if (w > h)
		XFillRectangle(dpy, backbuf, gc, x + h / 2, y,
			       (unsigned)(w - h), (unsigned)h);
}

static void
draw_fill_bar(int x, int y, int w, int h, int fill_w)
{
	int r = h / 2;

	if (fill_w <= 0)
		return;
	if (fill_w >= w) {
		draw_pill(x, y, w, h);
		return;
	}
	XFillArc(dpy, backbuf, gc, x, y, (unsigned)h, (unsigned)h,
		 90 * 64, 180 * 64);
	if (fill_w > r)
		XFillRectangle(dpy, backbuf, gc, x + r, y,
			       (unsigned)(fill_w - r), (unsigned)h);
}

static void
draw_battery(void)
{
	static int last_percent = -2, last_ac = -1;
	int percent, ac;
	int in_x, in_y, in_w, in_h, fill_w;
	unsigned long fill;
	XColor c;

	if (!read_battery(&percent, &ac))
		return;
	if (percent == last_percent && ac == last_ac)
		return;
	last_percent = percent;
	last_ac = ac;

	in_x = battery_zone_x + BATTERY_BORDER_INSET;
	in_y = battery_zone_y + BATTERY_BORDER_INSET;
	in_w = BATTERY_ICON_W - 2 * BATTERY_BORDER_INSET;
	in_h = BATTERY_ICON_H - 2 * BATTERY_BORDER_INSET;

	c.flags = DoRed | DoGreen | DoBlue;
	c.red = 0xcccc; c.green = 0xcccc; c.blue = 0xcccc;
	XAllocColor(dpy, DefaultColormap(dpy, screen), &c);
	XSetForeground(dpy, gc, c.pixel);
	draw_pill(battery_zone_x, battery_zone_y, BATTERY_ICON_W,
		  BATTERY_ICON_H);

	if (ac) {
		c.red = 0xffff; c.green = 0xffff; c.blue = 0x0000;
		XAllocColor(dpy, DefaultColormap(dpy, screen), &c);
		XSetForeground(dpy, gc, c.pixel);
	} else {
		XSetForeground(dpy, gc, col_bg);
	}
	draw_pill(in_x, in_y, in_w, in_h);

	if (percent > 0 && percent <= 100) {
		if (percent <= 25) {
			c.red = 0xffff; c.green = 0x0000; c.blue = 0x0000;
		} else if (percent <= 50) {
			c.red = 0xffff; c.green = 0x9999; c.blue = 0x3333;
		} else {
			c.red = 0x6666; c.green = 0xffff; c.blue = 0x3333;
		}
		XAllocColor(dpy, DefaultColormap(dpy, screen), &c);
		fill = c.pixel;

		fill_w = percent * in_w / 100;
		XSetForeground(dpy, gc, fill);
		draw_fill_bar(in_x, in_y, in_w, in_h, fill_w);
	}

	XCopyArea(dpy, backbuf, win, gc, battery_zone_x, battery_zone_y,
		  BATTERY_ICON_W, BATTERY_ICON_H, battery_zone_x,
		  battery_zone_y);

	XSetForeground(dpy, gc, col_bg);
}

static int
try_spawn(Sprite *s, int initial, const Sprite *roster, int n, int self)
{
	int tries, x, y;

	for (tries = 0; tries < SPAWN_TRIES; tries++) {
		if (initial) {
			x = rand() % (width > 1 ? width : 1);
			y = rand() % (height > 1 ? height : 1);
		} else if (rand() % 2) {
			x = width;
			y = rand() % (height + SPRITE_SIZE) - SPRITE_SIZE;
		} else {
			x = rand() % (width + SPRITE_SIZE);
			y = -SPRITE_SIZE;
		}
		if (diag_clear(x + y, roster, n, self) &&
		    battery_zone_clear(x + y)) {
			s->x = x;
			s->y = y;
			s->speed = MIN_SPEED + rand() % (MAX_SPEED - MIN_SPEED + 1);
			s->frame = rand() % TOASTER_FRAMES;
			s->alive = 1;
			return 1;
		}
	}
	return 0;
}

int
main(int argc, char **argv)
{
	XSetWindowAttributes attrs;
	XGCValues gcv;
	Sprite sprites[N_TOASTERS + N_TOAST];
	int i, tick = 0, xfd;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-B"))
			battery_deadzone = 0;
	}

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "toasters: cannot open display (is X running?)\n");
		return 1;
	}
	screen = DefaultScreen(dpy);
	width  = DisplayWidth(dpy, screen);
	height = DisplayHeight(dpy, screen);
	col_bg = BlackPixel(dpy, screen);
	battery_zone_x = width - BATTERY_MARGIN - BATTERY_ICON_W;
	battery_zone_y = height - BATTERY_MARGIN - BATTERY_ICON_H;

	attrs.override_redirect = True;
	attrs.background_pixel = col_bg;
	attrs.event_mask = KeyPressMask | ButtonPressMask;
	win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0,
			    (unsigned)width, (unsigned)height, 0,
			    CopyFromParent, InputOutput, CopyFromParent,
			    CWOverrideRedirect | CWBackPixel | CWEventMask,
			    &attrs);
	XMapRaised(dpy, win);

	/* Best-effort: run by hand under something that already holds a
	 * grab, this still dismisses on whatever it DOES receive. */
	XGrabKeyboard(dpy, win, True, GrabModeAsync, GrabModeAsync, CurrentTime);
	XGrabPointer(dpy, win, True, ButtonPressMask, GrabModeAsync,
		     GrabModeAsync, None, None, CurrentTime);

	backbuf = XCreatePixmap(dpy, win, (unsigned)width, (unsigned)height,
				(unsigned)DefaultDepth(dpy, screen));
	gcv.graphics_exposures = False;
	gc = XCreateGC(dpy, win, GCGraphicsExposures, &gcv);
	XSetForeground(dpy, gc, col_bg);
	XFillRectangle(dpy, backbuf, gc, 0, 0, (unsigned)width, (unsigned)height);

	for (i = 0; i < TOASTER_FRAMES; i++) {
		if (load_sprite(toasterXpm[i], &toaster_img[i],
				&toaster_mask[i]) != 0) {
			fprintf(stderr, "toasters: cannot build toaster frame %d\n", i);
			return 1;
		}
	}
	if (load_sprite(toastXpm, &toast_img, &toast_mask) != 0) {
		fprintf(stderr, "toasters: cannot build the toast sprite\n");
		return 1;
	}

	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	for (i = 0; i < N_TOASTERS + N_TOAST; i++) {
		sprites[i].toast = (i >= N_TOASTERS);
		sprites[i].alive = 0;
		try_spawn(&sprites[i], 1, sprites, i, i);
	}

	xfd = ConnectionNumber(dpy);

	for (;;) {
		fd_set rfds;
		struct timeval tv;

		while (XPending(dpy)) {
			XEvent ev;

			XNextEvent(dpy, &ev);
			if (ev.type == KeyPress || ev.type == ButtonPress) {
				XCloseDisplay(dpy);
				return 0;
			}
		}

		for (i = 0; i < N_TOASTERS + N_TOAST; i++) {
			Sprite *s = &sprites[i];
			int old_x, old_y, dx0, dy0, dx1, dy1;
			int just_spawned = 0;

			if (!s->alive) {
				if (!try_spawn(s, 0, sprites,
					       N_TOASTERS + N_TOAST, i))
					continue;
				just_spawned = 1;
			}

			old_x = s->x;
			old_y = s->y;

			if (!just_spawned) {
				s->x -= s->speed;
				s->y += s->speed;
				if (s->x <= -SPRITE_SIZE || s->y >= height) {
					XFillRectangle(dpy, backbuf, gc, old_x,
						       old_y, SPRITE_SIZE,
						       SPRITE_SIZE);
					XCopyArea(dpy, backbuf, win, gc, old_x,
						  old_y, SPRITE_SIZE,
						  SPRITE_SIZE, old_x, old_y);
					s->alive = 0;
					continue;
				}
			}

			XFillRectangle(dpy, backbuf, gc, old_x, old_y,
				       SPRITE_SIZE, SPRITE_SIZE);

			if (s->toast) {
				draw_sprite(toast_img, toast_mask, s->x, s->y);
			} else {
				if (tick % FLAP_EVERY == 0)
					s->frame = (s->frame + 1) % TOASTER_FRAMES;
				draw_sprite(toaster_img[s->frame],
					    toaster_mask[s->frame], s->x, s->y);
			}

			dx0 = old_x < s->x ? old_x : s->x;
			dy0 = old_y < s->y ? old_y : s->y;
			dx1 = (old_x > s->x ? old_x : s->x) + SPRITE_SIZE;
			dy1 = (old_y > s->y ? old_y : s->y) + SPRITE_SIZE;
			XCopyArea(dpy, backbuf, win, gc, dx0, dy0,
				  (unsigned)(dx1 - dx0), (unsigned)(dy1 - dy0),
				  dx0, dy0);
		}

		if (battery_deadzone && tick % BATTERY_CHECK_TICKS == 0)
			draw_battery();

		XFlush(dpy);
		tick++;

		FD_ZERO(&rfds);
		FD_SET(xfd, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = FRAME_MS * 1000;
		select(xfd + 1, &rfds, NULL, NULL, &tv);
	}
}
