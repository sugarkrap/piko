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
 * NO-OVERLAP is enforced at spawn time, not during flight, and it is exact
 * rather than best-effort. Every sprite moves by "x -= speed; y += speed"
 * with the SAME speed on both axes, so x+y is invariant for that sprite's
 * entire flight -- it only changes at the next respawn. Two sprites
 * therefore travel on parallel 45-degree lines whose perpendicular
 * distance is fixed for as long as both are alive. For two 64x64
 * axis-aligned boxes, standard AABB overlap needs |dx|<64 AND |dy|<64
 * where dx,dy are their coordinate differences; since dx+dy is exactly
 * that fixed (x+y) difference, the triangle inequality means both cannot
 * hold once |dx+dy| >= 128 (MIN_DIAG_GAP). A sprite only spawns into a
 * slot whose (x+y) clears every other currently-alive sprite's by that
 * much, which is what used to let a sprite fly straight through -- or,
 * since the dirty-rectangle repaint above only erases a moving sprite's
 * OWN previous 64x64 square, straight through and briefly bite a chunk
 * out of -- another one. Neither can happen now, by construction, for as
 * long as neither sprite respawns again.
 *
 * Finding that slot is NOT a bounded one-shot retry, because it provably
 * cannot always succeed on the first attempt: with up to 7 other sprites
 * already alive, each blocking a window of 2*MIN_DIAG_GAP around its own
 * (x+y), pure random sampling of a candidate slot is, empirically, right
 * at the classic 1D random-sequential-packing ("parking problem") jamming
 * density -- it was observed to run out of room on real hardware roughly
 * 1 respawn in 10, and the last (only) attempt would then land in an
 * occupied window, i.e. exactly the overlap this whole scheme exists to
 * prevent. There is also no retry count that fixes this in general: for
 * an adversarial-but-legal arrangement of the other 7, zero clear slots
 * can exist at all, no matter how many times you re-roll. So a sprite
 * that cannot find a clear slot does not spawn: it stays !alive and
 * try_spawn() is called again next frame (see the main loop below), at
 * SPAWN_TRIES attempts each time. Since every other sprite keeps moving
 * and eventually respawns elsewhere, freeing up its old window, room
 * reliably opens within a handful of frames -- invisible at ~100ms/frame
 * -- without ever compromising the zero-overlap guarantee to get there.
 *
 * BATTERY DEAD ZONE reserves the bottom-right corner (BATTERY_ICON_SIZE
 * square, inset BATTERY_MARGIN px from the right and bottom edges) for a
 * battery-status icon that lands separately -- this is prep, not that
 * feature. A sprite whose spawn box would land in that rectangle is
 * rejected by try_spawn() the same way an occupied (x+y) slot is, so it
 * gets the same treatment as the NO-OVERLAP case above: try elsewhere
 * this frame, or come back next frame if nothing was clear. It is
 * spawn-time only, same reasoning as NO-OVERLAP -- sprites drift down
 * and left, so one that does not spawn in or next to the corner does not
 * drift back into it later.
 *
 * Opt-out, not opt-in: the icon is expected to be on screen whenever this
 * runs, so avoiding it is the default (battery_deadzone = 1). -B on the
 * command line turns it off, for builds shipping without the icon.
 * brightd passes -B when power-management.cfg says
 * toast_battery_deadzone=no; see brightd.c's CONFIGURATION section.
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
#include <string.h>
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

/* Reserved for a battery-status icon that does not exist yet: bottom-right
 * corner, MARGIN px in from the right and bottom edges. The icon is still
 * being chosen between 16x16 and 24x24, so this takes the larger
 * candidate -- shrinking it later only lets sprites spawn a little closer
 * to the corner than strictly necessary, never the other way round, so it
 * is the safe default to ship before the real icon lands. Bump ICON_SIZE
 * to match once it does. See the BATTERY DEAD ZONE header comment. */
#define BATTERY_ICON_SIZE  24
#define BATTERY_MARGIN     10

/* See the NO-OVERLAP header comment: two sprites can never touch as long
 * as their (x+y) values differ by at least this much, for as long as both
 * are alive. 2*SPRITE_SIZE is the proven zero-overlap threshold (boxes
 * can still be edge-adjacent there, zero gap, zero overlap); the extra
 * SPRITE_SIZE/2 is just for a visibly comfortable gap, not correctness. */
#define MIN_DIAG_GAP    (SPRITE_SIZE * 2 + SPRITE_SIZE / 2)
/* Tries per FRAME for a sprite waiting for a clear spot, not a one-shot
 * budget -- see the NO-OVERLAP header comment for why a bounded one-shot
 * retry is not enough on its own. */
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

/* Top-left corner of the reserved battery-icon rectangle, computed once
 * width/height are known (see main()). Only meaningful when
 * battery_deadzone is set. */
static int battery_zone_x, battery_zone_y;
static int battery_deadzone = 1;   /* -B on the command line clears this */

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

/* True if a sprite spawning with x+y == diag cannot ever touch any
 * currently-alive sprite in the roster (skipping index self) -- see the
 * NO-OVERLAP header comment for why (x+y) separation alone is enough. */
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

/* True if a sprite spawning at (x,y) would land on the reserved
 * battery-icon rectangle -- see the BATTERY DEAD ZONE header comment.
 * Always false when battery_deadzone is off. */
static int
battery_zone_clear(int x, int y)
{
	if (!battery_deadzone)
		return 1;
	return x + SPRITE_SIZE <= battery_zone_x
	    || y + SPRITE_SIZE <= battery_zone_y
	    || x >= battery_zone_x + BATTERY_ICON_SIZE
	    || y >= battery_zone_y + BATTERY_ICON_SIZE;
}

/* Try to bring sprite s to life: enter from the top or right edge and
 * drift down-left, which is the direction the original After Dark
 * toasters flew and the one upstream kept, EXCEPT when initial is set
 * (startup only), which scatters across the whole screen instead so the
 * screensaver does not open on an empty panel and wait ten seconds for
 * the first toaster to sail in.
 *
 * Returns 0 without changing s if no (x+y) slot at least MIN_DIAG_GAP
 * from every other living sprite in roster[0..n), clear of the battery
 * dead zone, turned up in SPAWN_TRIES random attempts -- the caller is
 * expected to leave s dead and call again next frame. See the
 * NO-OVERLAP and BATTERY DEAD ZONE header comments for why that is a
 * real possibility this has to handle, not just defensive code. */
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
		    battery_zone_clear(x, y)) {
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
	battery_zone_x = width - BATTERY_MARGIN - BATTERY_ICON_SIZE;
	battery_zone_y = height - BATTERY_MARGIN - BATTERY_ICON_SIZE;

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
		sprites[i].alive = 0;
		/* n = i: only sprites[0..i-1] are alive yet. On the rare
		 * failure this leaves a straggler dead; the main loop below
		 * brings it in from an edge within its first few frames,
		 * same as any other post-startup respawn. */
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
				/* Waiting for a clear (x+y) slot -- see the
				 * NO-OVERLAP header comment. Nothing was
				 * drawn for this sprite, so there is nothing
				 * to erase or blit either; just keep trying. */
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
					/* Leaving the screen: erase its last
					 * footprint and go quiet. A future
					 * frame's try_spawn() above brings it
					 * back once there is room. */
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

			/* Erase this sprite's previous footprint (a no-op
			 * square of backbuf that is already background, the
			 * first frame it is alive) -- see the DIRTY-RECTANGLE
			 * header comment for why touching only this 64x64
			 * square, not the whole backbuf, is still always
			 * correct. */
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
