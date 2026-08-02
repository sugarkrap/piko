/*
 * toasters -- the "flying toasters" screensaver, for the Zaurus.
 *
 * The artwork and the motion are from torunar/flying-toasters-xscreensaver
 * (MIT, Copyright (c) 2023 Mikhail Shchekotov), vendored as the submodule
 * userspace/src/flying-toasters -- see its LICENSE. We include its two XPM
 * sprite sheets directly (a six-frame wing flap plus the winged toast) and
 * keep upstream's diagonal down-and-left drift. What is NOT reused is its
 * windowing: upstream draws into the real root window via vroot.h, or into
 * a hardcoded 1920x1080 window with -windowed, and animates at 60fps.
 * Neither survives contact with a 480x640 panel on a 400MHz PXA255, and
 * the root-window path in particular is wrong here -- see below.
 *
 * No GL, and not for want of trying: this board's w100 has no DRM/DRI
 * driver at all (same "no hardware acceleration path" story as the comment
 * on w100fb_blank() in modules/w100/w100fb.c), which is why upstream's
 * plain-Xlib approach is a good fit and xscreensaver's own GL flyingtoasters
 * hack is not an option. Sprites are drawn through a clip mask onto an
 * offscreen pixmap, which is XCopyArea'd to a fullscreen override-redirect
 * window.
 *
 * FRAME RATE is deliberately ~10fps, not upstream's 60. Sprites move
 * further per frame to compensate, so the apparent speed is unchanged.
 *
 * DIRTY-RECTANGLE rendering, not a full-screen clear+blit every frame: the
 * 8 sprites cover roughly a tenth of the 480x640 panel at any moment, so
 * touching the untouched nine tenths on an unaccelerated framebuffer every
 * ~100ms was pure waste. Each sprite instead erases just its own previous
 * 64x64 footprint, is redrawn at its new position, and only the union of
 * those two squares is XCopyArea'd to the window -- see the loop in
 * main(). This is safe even where sprites overlap: every sprite is
 * unconditionally erased-then-redrawn every frame in the same fixed
 * (array) order the old full-repaint used, so a later sprite still ends
 * up drawn on top of an earlier one, and any transient clobbering of a
 * neighbour's pixels earlier in the loop is corrected when that
 * neighbour's own turn comes later in the same frame. The backbuf
 * converges to exactly the image a full repaint would have produced.
 *
 * Sprites are pre-rendered to server-side Pixmaps once at startup (see
 * load_sprite()) rather than kept as client-side XImages and XPutImage'd
 * every frame: XPutImage re-sends the raw 64x64 pixel data over the X
 * connection and redoes format conversion on every single draw, 8 times a
 * frame. XCopyArea from an already-server-side Pixmap skips all of that.
 *
 * This program does not decide WHEN to run. brightd (userspace/src/
 * brightd.c) already has reliable, hardware-proven idle detection (see its
 * own "WHY NOT X" header comment) and launches this after toast_secs of
 * idle, killing it with SIGTERM the moment there is activity or it is time
 * to actually blank the backlight. No signal handler here for that: the
 * default SIGTERM disposition just ends the process, and the X server
 * releases the grabs and destroys the window on its own once the
 * connection closes.
 *
 * It also grabs the keyboard and pointer and exits the moment either sees
 * anything, purely as a safety net for running it by hand -- the intended
 * dismissal path is still brightd noticing activity itself. That is the
 * other reason not to use upstream's root-window drawing: painting the
 * root leaves whatever is on screen visible around the sprites and gives
 * nothing to grab input on, so a stray tap would reach the desktop
 * underneath instead of dismissing the screensaver.
 */

#include <X11/Xlib.h>
#include <X11/xpm.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>

/* Upstream's sprite sheets, included as data. toasterXpm is [6][80]: six
 * wing positions, each 64x64 with a 15-colour palette. toastXpm is one
 * 64x64 frame -- the toast does not flap, it just sails. */
#include "flying-toasters/img/toaster.xpm"
#include "flying-toasters/img/toast.xpm"

#define SPRITE_SIZE     64
#define TOASTER_FRAMES  6

/* Upstream flies 10 toasters and 6 toast on a 1920x1080 desktop. This
 * panel is 480x640 -- one seventh the area, with the SAME 64px sprites, so
 * they are proportionally seven times bigger. Keeping upstream's counts
 * would wallpaper the screen; these are scaled to roughly the same sense
 * of "a few, with room between them". */
#define N_TOASTERS      5
#define N_TOAST         3

#define FRAME_MS        100   /* ~10fps -- see the header on why not 60 */
#define FLAP_EVERY      2     /* advance the wing frame every N frames */
#define MIN_SPEED       4     /* px per frame, so 40..80 px/s at 10fps */
#define MAX_SPEED       8

typedef struct {
	int x, y;
	int speed;
	int frame;   /* wing position; unused for toast */
	int toast;   /* 0 = toaster, 1 = winged toast */
} Sprite;

static Display *dpy;
static int screen;
static Window win;
static Pixmap backbuf;
static GC gc;
static int width, height;
static unsigned long col_bg;

static Pixmap toaster_img[TOASTER_FRAMES];
static Pixmap toaster_mask[TOASTER_FRAMES];
static Pixmap toast_img;
static Pixmap toast_mask;

/* Decode one XPM sheet with XpmCreateImageFromData() and push both the
 * pixel data and the shape mask it hands back straight into server-side
 * Pixmaps, via one throwaway GC each. The image is never touched again
 * after this, so paying the XPutImage cost once here -- instead of once
 * per sprite per frame -- is the whole point. Same shape as upstream's
 * loadSprites(), minus its per-call XCreateGC leak. */
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

/* Enter from the top or right edge and drift down-left, which is the
 * direction the original After Dark toasters flew and the one upstream
 * kept. On the first call, scatter across the whole screen instead: the
 * screensaver should not open on an empty panel and then wait ten seconds
 * for the first toaster to sail in. */
static void
respawn(Sprite *s, int initial)
{
	if (initial) {
		s->x = rand() % (width > 1 ? width : 1);
		s->y = rand() % (height > 1 ? height : 1);
	} else if (rand() % 2) {
		s->x = width;
		s->y = rand() % (height + SPRITE_SIZE) - SPRITE_SIZE;
	} else {
		s->x = rand() % (width + SPRITE_SIZE);
		s->y = -SPRITE_SIZE;
	}
	s->speed = MIN_SPEED + rand() % (MAX_SPEED - MIN_SPEED + 1);
	s->frame = rand() % TOASTER_FRAMES;
}

int
main(void)
{
	XSetWindowAttributes attrs;
	XGCValues gcv;
	Sprite sprites[N_TOASTERS + N_TOAST];
	int i, tick = 0, xfd;

	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "toasters: cannot open display (is X running?)\n");
		return 1;
	}
	screen = DefaultScreen(dpy);
	width  = DisplayWidth(dpy, screen);
	height = DisplayHeight(dpy, screen);
	col_bg = BlackPixel(dpy, screen);

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
	/* Foreground stays col_bg for the whole run -- draw_sprite() only
	 * ever touches the clip mask/origin, never the foreground, so this
	 * does not need resetting per frame or per sprite. */
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
		respawn(&sprites[i], 1);
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
			int old_x = s->x, old_y = s->y;
			int dx0, dy0, dx1, dy1;

			s->x -= s->speed;
			s->y += s->speed;
			if (s->x <= -SPRITE_SIZE || s->y >= height)
				respawn(s, 0);

			/* Erase this sprite's previous footprint -- see the
			 * DIRTY-RECTANGLE header comment for why touching
			 * only this 64x64 square (not the whole backbuf) is
			 * still always correct. */
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

			/* Blit only the union of the old and new footprint
			 * -- the only pixels this sprite could have changed
			 * this frame. */
			dx0 = old_x < s->x ? old_x : s->x;
			dy0 = old_y < s->y ? old_y : s->y;
			dx1 = (old_x > s->x ? old_x : s->x) + SPRITE_SIZE;
			dy1 = (old_y > s->y ? old_y : s->y) + SPRITE_SIZE;
			XCopyArea(dpy, backbuf, win, gc, dx0, dy0,
				  (unsigned)(dx1 - dx0), (unsigned)(dy1 - dy0),
				  dx0, dy0);
		}

		XFlush(dpy);
		tick++;

		FD_ZERO(&rfds);
		FD_SET(xfd, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = FRAME_MS * 1000;
		select(xfd + 1, &rfds, NULL, NULL, &tv);
	}
}
