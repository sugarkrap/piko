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
 * hack is not an option. Sprites are XPutImage'd through a clip mask onto
 * an offscreen pixmap, and the pixmap is XCopyArea'd to a fullscreen
 * override-redirect window once per frame.
 *
 * FRAME RATE is deliberately ~10fps, not upstream's 60. Every frame copies
 * the whole 480x640x16bpp panel across an unaccelerated framebuffer; at
 * 60fps that is ~35MB/s of blitting on a part that cannot do it, and the
 * animation would tear and eat the CPU an idle machine is supposed to be
 * saving. Sprites move further per frame to compensate, so the apparent
 * speed is unchanged.
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

static XImage *toaster_img[TOASTER_FRAMES];
static Pixmap  toaster_mask[TOASTER_FRAMES];
static XImage *toast_img;
static Pixmap  toast_mask;

/* XpmCreateImageFromData hands back the shape mask as an XImage; a clip
 * mask has to be a depth-1 Pixmap, so push it into one. Same shape as
 * upstream's loadSprites(), minus its per-call XCreateGC leak. */
static int
load_sprite(char **data, XImage **img, Pixmap *mask)
{
	XImage *shape = NULL;
	GC mgc;

	if (XpmCreateImageFromData(dpy, data, img, &shape, NULL) != XpmSuccess)
		return -1;
	if (!shape) {
		/* No transparent colour in the sheet: no mask needed. */
		*mask = None;
		return 0;
	}
	*mask = XCreatePixmap(dpy, win, shape->width, shape->height,
			      shape->depth);
	mgc = XCreateGC(dpy, *mask, 0, NULL);
	XPutImage(dpy, *mask, mgc, shape, 0, 0, 0, 0,
		  shape->width, shape->height);
	XFreeGC(dpy, mgc);
	XDestroyImage(shape);
	return 0;
}

static void
draw_sprite(XImage *img, Pixmap mask, int x, int y)
{
	if (mask != None) {
		XSetClipMask(dpy, gc, mask);
		XSetClipOrigin(dpy, gc, x, y);
	}
	XPutImage(dpy, backbuf, gc, img, 0, 0, x, y,
		  SPRITE_SIZE, SPRITE_SIZE);
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

		XSetForeground(dpy, gc, col_bg);
		XFillRectangle(dpy, backbuf, gc, 0, 0, (unsigned)width,
			       (unsigned)height);

		for (i = 0; i < N_TOASTERS + N_TOAST; i++) {
			Sprite *s = &sprites[i];

			s->x -= s->speed;
			s->y += s->speed;
			if (s->x <= -SPRITE_SIZE || s->y >= height)
				respawn(s, 0);

			if (s->toast) {
				draw_sprite(toast_img, toast_mask, s->x, s->y);
			} else {
				if (tick % FLAP_EVERY == 0)
					s->frame = (s->frame + 1) % TOASTER_FRAMES;
				draw_sprite(toaster_img[s->frame],
					    toaster_mask[s->frame], s->x, s->y);
			}
		}

		XCopyArea(dpy, backbuf, win, gc, 0, 0, (unsigned)width,
			  (unsigned)height, 0, 0);
		XFlush(dpy);
		tick++;

		FD_ZERO(&rfds);
		FD_SET(xfd, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = FRAME_MS * 1000;
		select(xfd + 1, &rfds, NULL, NULL, &tv);
	}
}
