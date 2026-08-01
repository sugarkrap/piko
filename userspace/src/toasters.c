/*
 * toasters -- a "flying toasters" screensaver for the Zaurus.
 *
 * No GL: this board's w100 has no DRM/DRI driver at all (see the comment
 * on w100fb_blank() in modules/w100/w100fb.c for the same "no hardware
 * acceleration path" story), so a real xscreensaver-style GL hack is not
 * an option. This draws plain 2D sprites with core Xlib onto an offscreen
 * pixmap and XCopyArea()s the result to a fullscreen override-redirect
 * window once per frame -- cheap enough for a 400MHz PXA255.
 *
 * This program does not decide WHEN to run. brightd (userspace/src/
 * brightd.c) already has reliable, hardware-proven idle detection
 * (see its own "WHY NOT X" header comment) and launches this after
 * toast_secs of idle, killing it with SIGTERM the moment there is
 * activity or it is time to actually blank the backlight. No signal
 * handler here for that: the default SIGTERM disposition just ends the
 * process, and the X server releases the grabs and destroys the window
 * on its own once the connection closes.
 *
 * It also grabs the keyboard and pointer and exits the moment either
 * sees anything, purely as a safety net for running it by hand -- the
 * intended dismissal path is still brightd noticing activity itself.
 */

#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

#define N_TOASTERS   7
#define SPRITE_SIZE  40   /* generous bounding box, for wrap-around math */
#define FRAME_MS     120  /* ~8fps -- plenty for this animation */

typedef struct {
	double x, y;    /* top-left of the sprite's bounding box */
	double dx, dy;  /* velocity, px per frame */
	int kind;       /* 0 = toaster, 1 = flying toast */
} Sprite;

static Display *dpy;
static int screen;
static Window win;
static Pixmap backbuf;
static GC gc;
static int width, height;

static unsigned long col_bg, col_body, col_body_dark, col_slot,
	col_toast, col_toast_dark, col_wing;

static unsigned long
alloc_col(int r, int g, int b)
{
	XColor c;

	c.red = (unsigned short)(r << 8);
	c.green = (unsigned short)(g << 8);
	c.blue = (unsigned short)(b << 8);
	if (!XAllocColor(dpy, DefaultColormap(dpy, screen), &c))
		return BlackPixel(dpy, screen);
	return c.pixel;
}

/* Small triangular wings either side of a body rect, in one of two
 * positions depending on "up" -- alternating this each frame is the
 * whole of the flapping animation. */
static void
draw_wings(int x, int y, int w, int up)
{
	XPoint left[3], right[3];

	if (up) {
		left[0].x = x;         left[0].y = y;
		left[1].x = x - w;     left[1].y = y - w / 2;
		left[2].x = x;         left[2].y = y - w / 3;
		right[0].x = x + w;    right[0].y = y;
		right[1].x = x + 2 * w; right[1].y = y - w / 2;
		right[2].x = x + w;    right[2].y = y - w / 3;
	} else {
		left[0].x = x;         left[0].y = y;
		left[1].x = x - w;     left[1].y = y + w / 2;
		left[2].x = x;         left[2].y = y + w / 3;
		right[0].x = x + w;    right[0].y = y;
		right[1].x = x + 2 * w; right[1].y = y + w / 2;
		right[2].x = x + w;    right[2].y = y + w / 3;
	}
	XSetForeground(dpy, gc, col_wing);
	XFillPolygon(dpy, backbuf, gc, left, 3, Convex, CoordModeOrigin);
	XFillPolygon(dpy, backbuf, gc, right, 3, Convex, CoordModeOrigin);
}

static void
draw_toaster(int x, int y, int flap)
{
	XSetForeground(dpy, gc, col_body);
	XFillRectangle(dpy, backbuf, gc, x, y + 14, 22, 12);
	XSetForeground(dpy, gc, col_body_dark);
	XDrawRectangle(dpy, backbuf, gc, x, y + 14, 22, 12);

	XSetForeground(dpy, gc, col_slot);
	XFillRectangle(dpy, backbuf, gc, x + 3, y + 16, 5, 3);
	XFillRectangle(dpy, backbuf, gc, x + 12, y + 16, 5, 3);

	draw_wings(x + 3, y + 20, 8, flap);
}

static void
draw_toast(int x, int y, int flap)
{
	XSetForeground(dpy, gc, col_toast);
	XFillRectangle(dpy, backbuf, gc, x, y, 14, 10);
	XSetForeground(dpy, gc, col_toast_dark);
	XDrawRectangle(dpy, backbuf, gc, x, y, 14, 10);

	draw_wings(x + 2, y + 5, 6, flap);
}

static void
init_sprites(Sprite *sprites)
{
	int i;

	srand((unsigned)time(NULL) ^ (unsigned)getpid());
	for (i = 0; i < N_TOASTERS; i++) {
		sprites[i].x = rand() % (width > 1 ? width : 1);
		sprites[i].y = rand() % (height > 1 ? height : 1);
		sprites[i].dx = 1.0 + (rand() % 20) / 10.0;    /* 1.0 .. 2.9 */
		sprites[i].dy = -(1.0 + (rand() % 15) / 10.0); /* -1.0 .. -2.4 */
		if (rand() % 2)
			sprites[i].dx = -sprites[i].dx;
		if (rand() % 2)
			sprites[i].dy = -sprites[i].dy;
		/* Mostly toasters, a third or so flying toast on its own --
		 * matches the original joke better than an even split. */
		sprites[i].kind = (i % 3 == 0) ? 1 : 0;
	}
}

static void
step_sprite(Sprite *s)
{
	s->x += s->dx;
	s->y += s->dy;
	if (s->x < -SPRITE_SIZE)
		s->x = width;
	if (s->x > width)
		s->x = -SPRITE_SIZE;
	if (s->y < -SPRITE_SIZE)
		s->y = height;
	if (s->y > height)
		s->y = -SPRITE_SIZE;
}

int
main(void)
{
	XSetWindowAttributes attrs;
	XGCValues gcv;
	Sprite sprites[N_TOASTERS];
	int i, frame = 0, xfd;

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "toasters: cannot open display (is X running?)\n");
		return 1;
	}
	screen = DefaultScreen(dpy);
	width  = DisplayWidth(dpy, screen);
	height = DisplayHeight(dpy, screen);

	col_bg         = alloc_col(10, 10, 24);
	col_body       = alloc_col(190, 195, 200);
	col_body_dark  = alloc_col(90, 95, 100);
	col_slot       = alloc_col(50, 50, 55);
	col_toast      = alloc_col(196, 148, 87);
	col_toast_dark = alloc_col(120, 84, 40);
	col_wing       = alloc_col(230, 230, 235);

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

	init_sprites(sprites);
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

		XSetForeground(dpy, gc, col_bg);
		XFillRectangle(dpy, backbuf, gc, 0, 0, (unsigned)width,
			       (unsigned)height);

		for (i = 0; i < N_TOASTERS; i++) {
			step_sprite(&sprites[i]);
			if (sprites[i].kind)
				draw_toast((int)sprites[i].x, (int)sprites[i].y,
					   frame % 2);
			else
				draw_toaster((int)sprites[i].x, (int)sprites[i].y,
					     frame % 2);
		}

		XCopyArea(dpy, backbuf, win, gc, 0, 0, (unsigned)width,
			  (unsigned)height, 0, 0);
		XFlush(dpy);
		frame++;

		FD_ZERO(&rfds);
		FD_SET(xfd, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = FRAME_MS * 1000;
		select(xfd + 1, &rfds, NULL, NULL, &tv);
	}
}
