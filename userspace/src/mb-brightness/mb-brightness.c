/* mb-brightness - the brightness on-screen display for the Matchbox panel
 *
 * An applet with nothing to show. It docks no tray icon, draws no button
 * and has no menu; its entire job is to own the brightness OSD -- the bar
 * that appears at the top of the screen when Fn+3/Fn+4 change the
 * backlight.
 *
 * WHY AN APPLET AT ALL
 * -------------------
 * Because it needs an X connection, a main loop, and something to be
 * woken up by, and libmatchbox's tray app already provides all three --
 * including the select() over an extra fd this uses for its control FIFO
 * (see FIFO below). Being in the panel's applet list also means it starts
 * and stops with the session, which is what you want for something that
 * only draws over the session.
 *
 * INVISIBLE
 * ---------
 * mb_tray_app_hide() is called BEFORE mb_tray_app_main(), which is the
 * same trick mb-applet-card uses to start with an empty slot:
 * _init_docking() early-returns when is_hidden is set, so no tray window
 * is ever created and the panel never reserves a pixel for it. This
 * applet then never unhides -- unlike mb-applet-card, which comes and
 * goes with the card.
 *
 * That has one consequence worth knowing before changing anything here:
 *
 *   *** A hidden applet receives NO X EVENTS. ***
 *
 * mb_tray_handle_xevent() (libmb/mbtray.c) guards the whole dispatch on
 * "win_tray != None && !is_hidden", so xevent_cb, and with it every
 * Expose, ConfigureNotify and theme change, is never delivered. Only the
 * poll callback still runs, because get_xevent_timed() sits below that
 * guard in the main loop. Everything here is therefore driven from
 * poll_callback() and nothing waits on an X event.
 *
 * Two things fall out of it:
 *   - The OSD cannot repaint on Expose at all. It is painted when it is
 *     shown and whenever the level changes, and that is the whole story;
 *     if something ever obscures and then reveals it, it stays stale
 *     until the next change. Repainting on a timer instead was tried and
 *     removed -- see the comment in poll_callback() for why that is not
 *     worth its cost.
 *   - Theme colours are read when the OSD window is built rather than on
 *     a theme-change callback that will never arrive. A theme change
 *     while this is running is picked up at the next screen-size change
 *     or applet restart, which is a fair trade for a popup that lives
 *     1.5 seconds.
 *
 * BRIGHTNESS IS NOT OURS
 * ----------------------
 * /usr/sbin/brightd owns the backlight -- the level ladder, the idle
 * dimming, the lid, bl_power, all of it (docs/HOWTO-BRIGHTNESS.md). This
 * applet never writes to any of that. It READS
 * /sys/class/backlight/corgi_bl to find out what brightd just did, and
 * draws it. Two processes writing the same backlight would fight over an
 * SSP bus that is also carrying the touchscreen and the battery ADC.
 *
 * So brightd pokes this applet after a change, and this applet then asks
 * the kernel what the level actually is, rather than being told a number
 * it would have to trust. That also means it cannot drift: whatever is
 * on screen came from sysfs a millisecond ago.
 *
 *   *** Reads "brightness", NEVER "actual_brightness". ***
 *
 * corgi_bl_set_intensity() adds 0x10 to any intensity above 0x10 and
 * stores the ADJUSTED value, which is exactly what .get_brightness --
 * i.e. actual_brightness -- returns. At the driver's own default of 31,
 * actual_brightness already reads max_brightness while the panel sits at
 * two thirds. Anything reading it concludes there is no headroom left.
 * This is measured, documented hardware behaviour, not a suspicion; see
 * "The two traps" in docs/HOWTO-BRIGHTNESS.md.
 *
 * FIFO
 * ----
 * /tmp/mb-brightness.fifo, one byte per message, same shape as brightd's
 * own channel and mb-volume's:
 *
 *   's'  show the OSD, re-reading the level from sysfs first
 *   'h'  hide it now -- sent by the OTHER OSD when it wants the space
 *
 * Unknown bytes are ignored rather than guessed at, so the protocol can
 * grow without breaking an older applet, and so a stray newline from
 * "echo s > /tmp/mb-brightness.fifo" is harmless.
 *
 * ONE OSD AT A TIME
 * -----------------
 * mb-volume draws the same bar in the same place for volume. Two popups
 * at (OSD_MARGIN, OSD_MARGIN) would sit exactly on top of each other, and
 * whichever hid first would uncover a stale one underneath. So before
 * showing, each writes 'h' to the other's FIFO. It is one non-blocking
 * open that fails harmlessly with ENOENT/ENXIO when the other applet is
 * not running, it cannot ping-pong (hiding never notifies anyone), and it
 * needs no shared state.
 *
 * An X selection would be the textbook answer to "only one of these at a
 * time", and is what this would use if the applet were visible -- but
 * SelectionClear is an X event, and see INVISIBLE above.
 *
 * Copyright 2026 the piko project. GPL v2 or later, matching matchbox-panel.
 */

#include <libmb/mb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define _(x) (x)

/* The video-display artwork, the same monitor icon pikalibrate uses.
 * Shipped under this applet's own name rather than as "video-display" so
 * it follows the one-icon-per-applet convention the other applets use
 * (mb-volume.png, mb-applet-card.png). */
#define ICON_NAME "mb-brightness.png"
#define ICON_SIZE 32

#define BL_DIR "/sys/class/backlight/corgi_bl"

#define FIFO_PATH     "/tmp/mb-brightness.fifo"
#define PEER_FIFO     "/tmp/mb-volume.fifo"  /* the other OSD -- see above */

/* Geometry, identical to mb-volume's OSD on purpose: the two are the same
 * widget showing different things, and a user should not be able to tell
 * them apart except by the icon and the number. */
#define OSD_MARGIN      10
#define OSD_PAD          8
#define OSD_BAR_H       10
#define OSD_ICON_INSET   2
#define OSD_TIMEOUT_MS 1500
#define OSD_TICK_MS     150

#define OSD_WIDEST_PCT "100%"

#define DEFAULT_PANEL_BGCOL "#dadad5"
#define DEFAULT_PANEL_FGCOL "#000000"
#define MSG_FONT_SPEC       "Sans 14px"

/* ------------------------------------------------------------------ */
/* global state                                                        */

static MBTrayApp     *App;
static MBPixbuf      *Pb;
static MBPixbufImage *Icon, *IconOsd;
static MBFont        *MsgFont;

static int CurrentPct = 0;

static Window      OsdWin = None;
static Bool        OsdMapped = False;
static MBDrawable *OsdDrw;
static int         OsdW, OsdH;
static MBColor    *OsdBgCol, *OsdFgCol;
static unsigned long OsdHideAt;

static int FifoFd = -1;
static int FifoWriteFd = -1;

/* ------------------------------------------------------------------ */
/* sysfs                                                               */

static int
read_int (const char *name)
{
  char  path[128];
  char  buf[32];
  FILE *f;
  int   val = -1;

  snprintf (path, sizeof (path), "%s/%s", BL_DIR, name);
  f = fopen (path, "r");
  if (f == NULL)
    return -1;

  if (fgets (buf, sizeof (buf), f))
    val = atoi (buf);
  fclose (f);

  return val;
}

/* Returns 0..100, or -1 if the backlight is not there at all (no
 * corgi_lcd bound -- possible on a development host, and not worth
 * crashing over). "brightness", never "actual_brightness"; see the file
 * header for the measured reason why. */
static int
read_brightness_pct (void)
{
  int cur = read_int ("brightness");
  int max = read_int ("max_brightness");

  if (cur < 0 || max <= 0)
    return -1;
  if (cur > max)
    cur = max;

  return (cur * 100) / max;
}

/* ------------------------------------------------------------------ */
/* theme colours -- PanelBgColor/PanelFgColor, the keys matchbox-panel
 * paints ITSELF with, so the OSD reads as a piece of the panel that has
 * floated to the top of the screen rather than as a message balloon. */

static void
free_col (MBColor **col)
{
  if (*col)
    {
      mb_col_unref (*col);
      *col = NULL;
    }
}

static void
load_theme_colours (Display *dpy, Window root)
{
  Atom mb_theme, real_type;
  int  format;
  unsigned long n, extra;
  unsigned char *value = NULL;
  char theme_desktop[560];
  MBDotDesktop *theme = NULL;
  struct stat st;

  free_col (&OsdBgCol);
  free_col (&OsdFgCol);

  mb_theme = XInternAtom (dpy, "_MB_THEME", False);

  if (XGetWindowProperty (dpy, root, mb_theme, 0L, 512L, False,
			  AnyPropertyType, &real_type, &format, &n, &extra,
			  &value) == Success
      && value != NULL && *value != 0 && n > 0)
    {
      snprintf (theme_desktop, sizeof (theme_desktop), "%s/theme.desktop",
		(char *) value);
      if (stat (theme_desktop, &st) == 0)
	theme = mb_dotdesktop_new_from_file (theme_desktop);
    }
  if (value)
    XFree (value);

  if (theme != NULL)
    {
      /* mb_dotdesktop_get() returns unsigned char* (it is meant for UTF8
       * text); mb_col_new_from_spec() wants plain char* -- same bytes,
       * just a signedness cast. */
      char *bg = (char *) mb_dotdesktop_get (theme, "PanelBgColor");
      char *fg = (char *) mb_dotdesktop_get (theme, "PanelFgColor");

      OsdBgCol = mb_col_new_from_spec (Pb, bg ? bg : DEFAULT_PANEL_BGCOL);
      OsdFgCol = mb_col_new_from_spec (Pb, fg ? fg : DEFAULT_PANEL_FGCOL);
      mb_dotdesktop_free (theme);
    }
  else
    {
      OsdBgCol = mb_col_new_from_spec (Pb, DEFAULT_PANEL_BGCOL);
      OsdFgCol = mb_col_new_from_spec (Pb, DEFAULT_PANEL_FGCOL);
    }
}

/* ------------------------------------------------------------------ */
/* drawing primitives                                                   */

static void
fill_rect (MBPixbufImage *img, int x, int y, int w, int h,
	   unsigned char r, unsigned char g, unsigned char b)
{
  int px, py;

  for (py = y; py < y + h; py++)
    for (px = x; px < x + w; px++)
      mb_pixbuf_img_plot_pixel (Pb, img, px, py, r, g, b);
}

static void
draw_rect_border (MBPixbufImage *img, int x, int y, int w, int h,
		  unsigned char r, unsigned char g, unsigned char b)
{
  int i;

  for (i = x; i < x + w; i++)
    {
      mb_pixbuf_img_plot_pixel (Pb, img, i, y, r, g, b);
      mb_pixbuf_img_plot_pixel (Pb, img, i, y + h - 1, r, g, b);
    }
  for (i = y; i < y + h; i++)
    {
      mb_pixbuf_img_plot_pixel (Pb, img, x, i, r, g, b);
      mb_pixbuf_img_plot_pixel (Pb, img, x + w - 1, i, r, g, b);
    }
}

/* ------------------------------------------------------------------ */
/* the OSD                                                              */

/* Compared as a SIGNED difference at the call site so the deadline still
 * works across the ~49-day wrap of a 32-bit unsigned long on this board. */
static unsigned long
now_ms (void)
{
  struct timeval tv;

  gettimeofday (&tv, NULL);
  return (unsigned long) tv.tv_sec * 1000UL
       + (unsigned long) (tv.tv_usec / 1000);
}

/* The panel works its own height out the same way (panel.c:1766) -- 36px
 * above 320px of screen height, 20 below. Mirrored rather than hardcoded
 * so the OSD keeps matching the panel on a smaller screen. */
static int
osd_height (Display *dpy, int scr)
{
  return (DisplayHeight (dpy, scr) > 320) ? 36 : 20;
}

static int
osd_icon_size (void)
{
  return OsdH - (2 * OSD_ICON_INSET);
}

static void
osd_scale_icon (void)
{
  int sz = osd_icon_size ();

  if (IconOsd != NULL)
    {
      mb_pixbuf_img_free (Pb, IconOsd);
      IconOsd = NULL;
    }
  if (Icon != NULL && sz > 0)
    IconOsd = mb_pixbuf_img_scale (Pb, Icon, sz, sz);
}

static void
osd_redraw (void)
{
  MBPixbufImage *img;
  unsigned char bg_r, bg_g, bg_b, fg_r, fg_g, fg_b;
  int icon_sz, icon_x, icon_y;
  int pct_w, pct_x, bar_x, bar_w, bar_y, text_y, widest_w;
  char pct_text[8];

  if (!OsdMapped || OsdDrw == NULL)
    return;

  bg_r = mb_col_red (OsdBgCol); bg_g = mb_col_green (OsdBgCol); bg_b = mb_col_blue (OsdBgCol);
  fg_r = mb_col_red (OsdFgCol); fg_g = mb_col_green (OsdFgCol); fg_b = mb_col_blue (OsdFgCol);

  snprintf (pct_text, sizeof (pct_text), "%d%%", CurrentPct);

  pct_w = mb_font_get_txt_width (MsgFont, (unsigned char *) pct_text,
				 strlen (pct_text), MB_ENCODING_UTF8);
  widest_w = mb_font_get_txt_width (MsgFont, (unsigned char *) OSD_WIDEST_PCT,
				    strlen (OSD_WIDEST_PCT), MB_ENCODING_UTF8);

  icon_sz = osd_icon_size ();
  icon_x  = OSD_PAD;
  icon_y  = (OsdH - icon_sz) / 2;

  /* The text is right-aligned against the right padding, but the BAR is
   * laid out against the width of OSD_WIDEST_PCT rather than the current
   * text, so its right-hand end does not jump about as the number goes
   * 9% -> 10% -> 100%. */
  pct_x = OsdW - OSD_PAD - pct_w;
  bar_x = icon_x + icon_sz + OSD_PAD;
  bar_w = (OsdW - OSD_PAD - widest_w - OSD_PAD) - bar_x;
  bar_y = (OsdH - OSD_BAR_H) / 2;

  img = mb_pixbuf_img_rgba_new (Pb, OsdW, OsdH);
  mb_pixbuf_img_fill (Pb, img, bg_r, bg_g, bg_b, 255);
  draw_rect_border (img, 0, 0, OsdW, OsdH, fg_r, fg_g, fg_b);

  if (IconOsd != NULL)
    mb_pixbuf_img_copy_composite (Pb, img, IconOsd, 0, 0,
				  mb_pixbuf_img_get_width (IconOsd),
				  mb_pixbuf_img_get_height (IconOsd),
				  icon_x, icon_y);

  if (bar_w > 2)
    {
      draw_rect_border (img, bar_x, bar_y, bar_w, OSD_BAR_H,
			fg_r, fg_g, fg_b);
      if (CurrentPct > 0)
	{
	  int fill_w = ((bar_w - 2) * CurrentPct) / 100;
	  if (fill_w > 0)
	    fill_rect (img, bar_x + 1, bar_y + 1, fill_w, OSD_BAR_H - 2,
		       fg_r, fg_g, fg_b);
	}
    }

  mb_pixbuf_img_render_to_drawable (Pb, img, mb_drawable_pixmap (OsdDrw), 0, 0);
  mb_pixbuf_img_free (Pb, img);

  /* After the image lands on the pixmap, not before -- MBFont renders via
   * Xft straight to the Drawable and would otherwise be overwritten. */
  text_y = (OsdH - mb_font_get_height (MsgFont)) / 2;
  if (text_y < 0)
    text_y = 0;

  mb_font_set_color (MsgFont, OsdFgCol);
  mb_font_render_simple (MsgFont, OsdDrw, pct_x, text_y, pct_w,
			 (unsigned char *) pct_text, MB_ENCODING_UTF8, 0);

  XCopyArea (mb_tray_app_xdisplay (App), mb_drawable_pixmap (OsdDrw),
	     OsdWin, DefaultGC (mb_tray_app_xdisplay (App),
				mb_tray_app_xscreen (App)),
	     0, 0, OsdW, OsdH, 0, 0);
}

static void
osd_destroy (void)
{
  if (OsdWin == None)
    return;

  if (OsdDrw)
    {
      mb_drawable_unref (OsdDrw);
      OsdDrw = NULL;
    }
  XDestroyWindow (mb_tray_app_xdisplay (App), OsdWin);
  OsdWin = None;
  OsdMapped = False;
}

static void
osd_hide (void)
{
  if (!OsdMapped)
    return;

  XUnmapWindow (mb_tray_app_xdisplay (App), OsdWin);
  XFlush (mb_tray_app_xdisplay (App));
  OsdMapped = False;

  /* Back to a select() with no timeout: nothing needs waking up until the
   * next poke, and an idle applet on a battery device should cost
   * nothing. */
  mb_tray_app_set_poll_timeout (App, NULL);
}

/* Asks the other OSD to get out of the way. Non-blocking and entirely
 * best-effort: ENOENT (never started) and ENXIO (not running) are the
 * normal cases and mean there is nothing to move. */
static void
poke_peer_hide (void)
{
  int fd = open (PEER_FIFO, O_WRONLY | O_NONBLOCK);

  if (fd < 0)
    return;

  /* Return value deliberately ignored: this message is droppable by
   * design, and the worst case if it never lands is a moment of overlap
   * between two popups that both disappear in a second and a half. */
  (void) write (fd, "h", 1);
  close (fd);
}

static void
osd_show (void)
{
  Display *dpy = mb_tray_app_xdisplay (App);
  int      scr = mb_tray_app_xscreen (App);
  Window   root = mb_tray_app_xrootwin (App);
  int      want_w, want_h;

  want_h = osd_height (dpy, scr);
  want_w = DisplayWidth (dpy, scr) - (2 * OSD_MARGIN);
  if (want_w < 1)
    want_w = DisplayWidth (dpy, scr);

  /* The screen really does change size under us: the swivel hinge drives
   * a live portrait <-> landscape resize rather than a mode switch
   * (docs/HOWTO-SCREEN-ROTATION.md), so a window sized for the old
   * orientation would come back hanging off the screen. Rebuilding also
   * re-reads the theme, which is the only chance this applet gets -- a
   * hidden applet never sees a theme-change event. */
  if (OsdWin != None && (OsdW != want_w || OsdH != want_h))
    {
      osd_hide ();
      osd_destroy ();
    }

  if (OsdWin == None)
    {
      XSetWindowAttributes attr;
      Atom type_atom, splash_atom;

      load_theme_colours (dpy, root);

      OsdH = want_h;
      OsdW = want_w;
      osd_scale_icon ();

      /* Exposure is selected for form's sake only -- a hidden applet
       * never gets it delivered (see INVISIBLE in the file header). It
       * stays here so that if this applet is ever made visible, Expose
       * handling is one callback away rather than a rediscovery. */
      attr.event_mask = ExposureMask;
      attr.background_pixel = mb_col_xpixel (OsdBgCol);
      attr.override_redirect = True;

      OsdWin = XCreateWindow (dpy, root, OSD_MARGIN, OSD_MARGIN,
			      OsdW, OsdH, 0,
			      CopyFromParent, CopyFromParent, CopyFromParent,
			      CWBackPixel | CWEventMask | CWOverrideRedirect,
			      &attr);

      type_atom   = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE", False);
      splash_atom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
      XChangeProperty (dpy, OsdWin, type_atom, XA_ATOM, 32, PropModeReplace,
		       (unsigned char *) &splash_atom, 1);

      OsdDrw = mb_drawable_new (Pb, OsdW, OsdH);
    }

  if (!OsdMapped)
    {
      struct timeval tv;

      poke_peer_hide (); /* only one OSD on screen at a time */

      XMapRaised (dpy, OsdWin);
      OsdMapped = True;

      /* Arm the tick only while the OSD is up. The callback itself is
       * registered once, in main(). */
      tv.tv_sec  = 0;
      tv.tv_usec = OSD_TICK_MS * 1000;
      mb_tray_app_set_poll_timeout (App, &tv);
    }
  else
    {
      XRaiseWindow (dpy, OsdWin);
    }

  OsdHideAt = now_ms () + OSD_TIMEOUT_MS;
  osd_redraw ();
}

/* ------------------------------------------------------------------ */
/* control FIFO                                                         */

static void
fifo_command (char c)
{
  int pct;

  switch (c)
    {
    case 's':
      /* Ask the kernel rather than trusting a number off the wire -- see
       * BRIGHTNESS IS NOT OURS in the file header. */
      pct = read_brightness_pct ();
      if (pct < 0)
	return; /* no backlight to report on; say nothing rather than lie */
      CurrentPct = pct;
      osd_show ();
      break;

    case 'h':
      osd_hide ();
      break;

    default:
      break; /* ignored, so the protocol can grow and stray newlines are free */
    }
}

/* Called by the main loop both when the FIFO is readable and when the
 * tick armed by osd_show() expires -- libmatchbox uses one callback for
 * both, so this handles either having happened. */
static void
poll_callback (MBTrayApp *app)
{
  char buf[32];
  int  n, i;

  (void) app;

  if (FifoFd >= 0)
    {
      while ((n = read (FifoFd, buf, sizeof (buf))) > 0)
	for (i = 0; i < n; i++)
	  fifo_command (buf[i]);
    }

  /* Deadline only. This deliberately does NOT repaint: an earlier version
   * redrew here every tick to stand in for the Expose a hidden applet can
   * never receive, and that meant a full image rebuild, an Xft text
   * render and an XCopyArea roughly seven times per appearance, for a
   * picture that had not changed. On an unaccelerated 400MHz server that
   * is a steady stream of requests bought for nothing.
   *
   * It is also a suspect in a session-wide "XIO: fatal IO error 11
   * (Resource temporarily unavailable)" seen on hardware 2026-08-02,
   * which killed every X client at once -- matchbox-panel and
   * matchbox-desktop included, neither of which this project had
   * touched. That was NOT root-caused, and the same failure is already
   * recorded, equally unexplained, in docs/HOWTO-BRIGHTNESS.md ("the X
   * session twice ended up dead after a manual launch"). So this is not
   * a proven cause -- but it was pointless traffic either way, and the
   * cheapest thing to remove while the real cause is still open.
   *
   * The cost of dropping it: if something ever does obscure and then
   * reveal the OSD, it stays stale until the next change. The OSD is
   * override-redirect and raised on every show, and lives a second and a
   * half, so that window is small. */
  if (OsdMapped && (long) (now_ms () - OsdHideAt) >= 0)
    osd_hide ();
}

static void
fifo_init (void)
{
  if (mkfifo (FIFO_PATH, 0666) != 0 && errno != EEXIST)
    {
      fprintf (stderr, "mb-brightness: mkfifo %s: %s\n",
	       FIFO_PATH, strerror (errno));
      return;
    }

  FifoFd = open (FIFO_PATH, O_RDONLY | O_NONBLOCK);
  if (FifoFd < 0)
    {
      fprintf (stderr, "mb-brightness: open %s: %s\n",
	       FIFO_PATH, strerror (errno));
      return;
    }

  /* Our own write end, held open and never written to. A FIFO whose last
   * writer closes goes to permanent EOF -- read() returns 0 and select()
   * reports it readable forever -- which would spin this loop at 100% CPU
   * the moment brightd exited. brightd holds its own FIFO open for
   * exactly the same reason. */
  FifoWriteFd = open (FIFO_PATH, O_WRONLY | O_NONBLOCK);
  if (FifoWriteFd < 0)
    fprintf (stderr, "mb-brightness: warning: no write end on %s: %s\n",
	     FIFO_PATH, strerror (errno));
}

/* ------------------------------------------------------------------ */
/* MBTrayApp callbacks -- all three are required by mb_tray_app_new() and
 * none of them can ever fire: this applet has no tray window to resize,
 * paint or re-theme. They exist so the library has something to call. */

static void
resize_callback (MBTrayApp *app, int w, int h)
{
  (void) app; (void) w; (void) h;
}

static void
paint_callback (MBTrayApp *app, Drawable drw)
{
  (void) app; (void) drw;
}

static void
load_icon (void)
{
  char *icon_path;

  icon_path = mb_dot_desktop_icon_get_full_path (NULL, ICON_SIZE, ICON_NAME);
  if (icon_path == NULL
      || (Icon = mb_pixbuf_img_new_from_file (Pb, icon_path)) == NULL)
    {
      /* Not fatal, unlike in a visible applet: with no icon the OSD still
       * shows the bar and the number, which is the information. */
      fprintf (stderr, "mb-brightness: failed to load icon '%s'\n", ICON_NAME);
      Icon = NULL;
    }

  if (icon_path)
    free (icon_path);
}

/* ------------------------------------------------------------------ */

int
main (int argc, char *argv[])
{
  struct timeval tv;

  App = mb_tray_app_new ((unsigned char *) _("Brightness"),
			 resize_callback,
			 paint_callback,
			 &argc,
			 &argv);
  if (App == NULL)
    {
      fprintf (stderr, "mb-brightness: failed to create tray app\n");
      exit (1);
    }

  Pb = mb_pixbuf_new (mb_tray_app_xdisplay (App), mb_tray_app_xscreen (App));
  if (Pb == NULL)
    {
      fprintf (stderr, "mb-brightness: failed to create pixbuf\n");
      exit (1);
    }

  MsgFont = mb_font_new_from_string (mb_tray_app_xdisplay (App), MSG_FONT_SPEC);

  load_icon ();

  fifo_init ();
  if (FifoFd < 0)
    {
      /* Without the FIFO nothing can ever ask for the OSD, so this
       * process would sit in select() forever achieving nothing. Say so
       * and stop, rather than look like it is working. */
      fprintf (stderr, "mb-brightness: no control FIFO -- nothing to do\n");
      exit (1);
    }

  /* set_timeout_callback is the only way to register poll_callback, and
   * it memcpy()s from the timeval unconditionally, so NULL is not an
   * option there. Register with a throwaway value and clear the timeout
   * immediately: the callback stays installed for the FIFO, but an idle
   * applet blocks in select() with no timer. osd_show() arms a real tick
   * when it needs one. */
  tv.tv_sec  = 0;
  tv.tv_usec = OSD_TICK_MS * 1000;
  mb_tray_app_set_timeout_callback (App, poll_callback, &tv);
  mb_tray_app_set_poll_timeout (App, NULL);
  mb_tray_app_set_poll_fd (App, FifoFd);

  /* Hide BEFORE the main loop, which is what makes this applet invisible
   * rather than briefly visible: _init_docking() early-returns on
   * is_hidden, so no tray window is created and the panel reserves no
   * space. Same trick mb-applet-card uses to start with an empty slot --
   * the difference is that this one never unhides. See INVISIBLE in the
   * file header for what that costs. */
  mb_tray_app_hide (App);

  mb_tray_app_main (App);

  return 0;
}
