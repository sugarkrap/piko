/*
 * mb-irda -- matchbox tray applet for the infrared port.
 *
 * The bubble is a remix of mb-volume's: same one-shot redraw into an
 * MBPixbufImage, same theme colours, same checkbox primitive. mb-volume has
 * one checkbox; here there are three exclusive mode rows drawn as radios
 * (same box, filled centre when selected) plus one ordinary checkbox for
 * discovery.
 *
 * The applet owns no hardware. Mode changes are single lines written to ird
 * on a throwaway connection; a second, long-lived connection sits subscribed
 * and joins the tray's select() through mb_tray_app_set_poll_fd(), so an
 * external change (irctl, another app) redraws us without polling.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <libmb/mb.h>

#define ICON_NAME    "mb-irda.png"
#define ICON_SIZE    32

#define SOCK_DEFAULT "/var/run/ird.sock"

#define BUBBLE_W       184
#define BUBBLE_MARGIN   10
#define ROW_SPACING      6
#define BOX_SIZE        14
#define BOX_TEXT_GAP     6

#define DEFAULT_MSG_BGCOL "yellow"
#define DEFAULT_MSG_FGCOL "black"
#define MSG_FONT_SPEC     "Sans 14px"

enum { MODE_OFF, MODE_IRDA, MODE_BLASTER, MODE_COUNT };

static const char *ModeVerb[MODE_COUNT]  = { "off", "irda", "blaster" };
static const char *ModeLabel[MODE_COUNT] = { "Off", "IrDA", "Blaster" };

static MBTrayApp     *App;
static MBPixbuf      *Pb;
static MBPixbufImage *Icon, *IconScaled;
static MBFont        *MsgFont;
static char          *ThemeName;

static MBColor   *BubbleBgCol, *BubbleFgCol;
static Window     BubbleWin = None;
static MBDrawable *BubbleDrw;
static Bool       BubbleOpen = False;
static int        BubbleH;
static int        RowY[MODE_COUNT];
static int        DiscoveryY;

static int  CurMode = MODE_OFF;
static int  CurDiscovery;
static int  EventFd = -1;

static const char *
sock_path (void)
{
  const char *env = getenv ("IRD_SOCK");

  return (env && *env) ? env : SOCK_DEFAULT;
}

static int
ird_connect (void)
{
  struct sockaddr_un addr;
  int fd = socket (AF_UNIX, SOCK_STREAM, 0);

  if (fd < 0)
    return -1;

  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  snprintf (addr.sun_path, sizeof (addr.sun_path), "%s", sock_path ());

  if (connect (fd, (struct sockaddr *) &addr, sizeof (addr)) != 0)
    {
      close (fd);
      return -1;
    }

  return fd;
}

/* One command, one reply, one connection. ird is happy with several clients
 * and a command is a single line, so there is nothing to keep open. */
static int
ird_ask (const char *cmd, char *reply, size_t n)
{
  int     fd = ird_connect ();
  ssize_t got;
  char    line[256];

  reply[0] = '\0';

  if (fd < 0)
    return -1;

  snprintf (line, sizeof (line), "%s\n", cmd);

  if (write (fd, line, strlen (line)) < 0)
    {
      close (fd);
      return -1;
    }

  got = read (fd, reply, n - 1);
  close (fd);

  if (got <= 0)
    return -1;

  reply[got] = '\0';

  return 0;
}

/* ird's status line is "ok mode=<m> intent=<m> discovery=<0|1> scout=<s>".
 * Parse only what we draw; anything we cannot read leaves the old value. */
static void
parse_status (const char *reply)
{
  const char *p;
  int i;

  p = strstr (reply, "mode=");
  if (p != NULL)
    {
      p += 5;
      for (i = 0; i < MODE_COUNT; i++)
	{
	  size_t len = strlen (ModeVerb[i]);

	  if (!strncmp (p, ModeVerb[i], len)
	      && (p[len] == ' ' || p[len] == '\n' || p[len] == '\0'))
	    {
	      CurMode = i;
	      break;
	    }
	}
    }

  p = strstr (reply, "discovery=");
  if (p != NULL)
    CurDiscovery = (p[10] == '1');
}

static void
refresh_state (void)
{
  char reply[256];

  if (ird_ask ("status", reply, sizeof (reply)) == 0)
    parse_status (reply);
}

static void bubble_redraw (void);

static void
set_mode (int mode)
{
  char reply[256];

  if (mode < 0 || mode >= MODE_COUNT)
    return;

  if (ird_ask (ModeVerb[mode], reply, sizeof (reply)) == 0)
    parse_status (reply);
  else
    refresh_state ();
}

static void
toggle_discovery (void)
{
  char reply[256];
  char cmd[32];

  snprintf (cmd, sizeof (cmd), "discovery %s",
	    CurDiscovery ? "off" : "on");

  if (ird_ask (cmd, reply, sizeof (reply)) == 0)
    parse_status (reply);
  else
    refresh_state ();
}

/* ------------------------------------------------------------------ */
/* bubble drawing -- primitives lifted from mb-volume                   */

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

/* A radio is mb-volume's checkbox with its corners knocked off and a
 * smaller filled centre, so the two read as different controls without
 * introducing a second drawing style. */
static void
draw_box (MBPixbufImage *img, int y, int filled, int radio,
	  unsigned char r, unsigned char g, unsigned char b)
{
  int x = BUBBLE_MARGIN;

  draw_rect_border (img, x, y, BOX_SIZE, BOX_SIZE, r, g, b);

  if (radio)
    {
      mb_pixbuf_img_plot_pixel (Pb, img, x, y, 0, 0, 0);
      mb_pixbuf_img_plot_pixel (Pb, img, x + BOX_SIZE - 1, y, 0, 0, 0);
      mb_pixbuf_img_plot_pixel (Pb, img, x, y + BOX_SIZE - 1, 0, 0, 0);
      mb_pixbuf_img_plot_pixel (Pb, img, x + BOX_SIZE - 1, y + BOX_SIZE - 1,
				0, 0, 0);
    }

  if (filled)
    fill_rect (img, x + (radio ? 4 : 3), y + (radio ? 4 : 3),
	       BOX_SIZE - (radio ? 8 : 6), BOX_SIZE - (radio ? 8 : 6),
	       r, g, b);
}

static void
render_row_text (int y, const char *text)
{
  mb_font_render_simple (MsgFont, BubbleDrw,
			 BUBBLE_MARGIN + BOX_SIZE + BOX_TEXT_GAP, y - 1,
			 BUBBLE_W - (2 * BUBBLE_MARGIN) - BOX_SIZE
			   - BOX_TEXT_GAP,
			 (unsigned char *) text, MB_ENCODING_UTF8, 0);
}

static void
bubble_redraw (void)
{
  MBPixbufImage *img;
  unsigned char bg_r, bg_g, bg_b, fg_r, fg_g, fg_b;
  int i;

  if (!BubbleOpen || BubbleDrw == NULL)
    return;

  bg_r = mb_col_red (BubbleBgCol);
  bg_g = mb_col_green (BubbleBgCol);
  bg_b = mb_col_blue (BubbleBgCol);
  fg_r = mb_col_red (BubbleFgCol);
  fg_g = mb_col_green (BubbleFgCol);
  fg_b = mb_col_blue (BubbleFgCol);

  img = mb_pixbuf_img_rgba_new (Pb, BUBBLE_W, BubbleH);
  mb_pixbuf_img_fill (Pb, img, bg_r, bg_g, bg_b, 255);
  draw_rect_border (img, 0, 0, BUBBLE_W, BubbleH, fg_r, fg_g, fg_b);

  for (i = 0; i < MODE_COUNT; i++)
    draw_box (img, RowY[i], i == CurMode, 1, fg_r, fg_g, fg_b);

  draw_box (img, DiscoveryY, CurDiscovery, 0, fg_r, fg_g, fg_b);

  mb_pixbuf_img_render_to_drawable (Pb, img, mb_drawable_pixmap (BubbleDrw),
				    0, 0);
  mb_pixbuf_img_free (Pb, img);

  mb_font_set_color (MsgFont, BubbleFgCol);

  mb_font_render_simple (MsgFont, BubbleDrw, BUBBLE_MARGIN, BUBBLE_MARGIN,
			 BUBBLE_W - (2 * BUBBLE_MARGIN),
			 (unsigned char *) "Infrared", MB_ENCODING_UTF8, 0);

  for (i = 0; i < MODE_COUNT; i++)
    render_row_text (RowY[i], ModeLabel[i]);

  render_row_text (DiscoveryY, "Discovery");

  XCopyArea (mb_tray_app_xdisplay (App), mb_drawable_pixmap (BubbleDrw),
	     BubbleWin, DefaultGC (mb_tray_app_xdisplay (App),
				   mb_tray_app_xscreen (App)),
	     0, 0, BUBBLE_W, BubbleH, 0, 0);
}

/* ------------------------------------------------------------------ */
/* theme colours, taken the way mb-volume takes them                    */

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

  free_col (&BubbleBgCol);
  free_col (&BubbleFgCol);

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
      char *bg = (char *) mb_dotdesktop_get (theme, "PanelMsgBgCol");
      char *fg = (char *) mb_dotdesktop_get (theme, "PanelMsgFgCol");

      BubbleBgCol = mb_col_new_from_spec (Pb, bg ? bg : DEFAULT_MSG_BGCOL);
      BubbleFgCol = mb_col_new_from_spec (Pb, fg ? fg : DEFAULT_MSG_FGCOL);
      mb_dotdesktop_free (theme);
    }
  else
    {
      BubbleBgCol = mb_col_new_from_spec (Pb, DEFAULT_MSG_BGCOL);
      BubbleFgCol = mb_col_new_from_spec (Pb, DEFAULT_MSG_FGCOL);
    }
}

/* ------------------------------------------------------------------ */
/* bubble lifecycle                                                     */

static void
close_bubble (void)
{
  Display *dpy = mb_tray_app_xdisplay (App);

  if (!BubbleOpen)
    return;

  XUngrabKeyboard (dpy, CurrentTime);
  XUngrabPointer (dpy, CurrentTime);

  if (BubbleDrw)
    {
      mb_drawable_unref (BubbleDrw);
      BubbleDrw = NULL;
    }
  XDestroyWindow (dpy, BubbleWin);
  BubbleWin = None;
  BubbleOpen = False;
}

static void
open_bubble (void)
{
  Display *dpy = mb_tray_app_xdisplay (App);
  int      scr = mb_tray_app_xscreen (App);
  Window   root = mb_tray_app_xrootwin (App);
  XSetWindowAttributes attr;
  long winmask;
  Atom type_atom, splash_atom;
  int  abs_x, abs_y, bx, by, row_h, y, i;

  if (BubbleOpen)
    {
      close_bubble ();
      return;
    }

  refresh_state ();
  load_theme_colours (dpy, root);

  row_h = mb_font_get_height (MsgFont);

  y = BUBBLE_MARGIN + row_h + ROW_SPACING;
  for (i = 0; i < MODE_COUNT; i++)
    {
      RowY[i] = y;
      y += BOX_SIZE + ROW_SPACING;
    }
  y += ROW_SPACING;
  DiscoveryY = y;
  BubbleH = DiscoveryY + BOX_SIZE + BUBBLE_MARGIN;

  mb_tray_app_get_absolute_coords (App, &abs_x, &abs_y);

  if (mb_tray_app_tray_is_vertical (App))
    {
      by = abs_y;
      bx = (abs_x > DisplayWidth (dpy, scr) / 2)
	? abs_x - BUBBLE_W - 2
	: abs_x + mb_tray_app_width (App) + 2;
    }
  else
    {
      bx = abs_x;
      by = (abs_y > DisplayHeight (dpy, scr) / 2)
	? abs_y - BubbleH - 2
	: abs_y + mb_tray_app_height (App) + 2;
    }

  if (bx < 0) bx = 0;
  if (bx + BUBBLE_W > DisplayWidth (dpy, scr))
    bx = DisplayWidth (dpy, scr) - BUBBLE_W;
  if (by < 0) by = 0;

  attr.event_mask = ButtonPressMask | ButtonReleaseMask | ExposureMask
    | KeyPressMask;
  attr.background_pixel = mb_col_xpixel (BubbleBgCol);
  attr.override_redirect = True;
  winmask = CWBackPixel | CWEventMask | CWOverrideRedirect;

  BubbleWin = XCreateWindow (dpy, root, bx, by, BUBBLE_W, BubbleH, 0,
			     CopyFromParent, CopyFromParent, CopyFromParent,
			     winmask, &attr);

  type_atom   = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE", False);
  splash_atom = XInternAtom (dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
  XChangeProperty (dpy, BubbleWin, type_atom, XA_ATOM, 32, PropModeReplace,
		   (unsigned char *) &splash_atom, 1);

  BubbleDrw = mb_drawable_new (Pb, BUBBLE_W, BubbleH);

  XMapRaised (dpy, BubbleWin);

  XGrabPointer (dpy, root, True,
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
		GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
  XGrabKeyboard (dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime);

  BubbleOpen = True;
  bubble_redraw ();
}

static void
bubble_button_press (int x, int y)
{
  int i;

  if (x < BUBBLE_MARGIN || x >= BUBBLE_W - BUBBLE_MARGIN)
    return;

  for (i = 0; i < MODE_COUNT; i++)
    {
      if (y >= RowY[i] && y < RowY[i] + BOX_SIZE)
	{
	  set_mode (i);
	  bubble_redraw ();
	  return;
	}
    }

  if (y >= DiscoveryY && y < DiscoveryY + BOX_SIZE)
    {
      toggle_discovery ();
      bubble_redraw ();
    }
}

/* ------------------------------------------------------------------ */
/* ird event subscription                                               */

/* A second, long-lived connection that only ever receives. Registering its
 * fd with the tray means an external mode change (irctl, the remote app)
 * redraws us out of the same select() the applet already blocks in -- no
 * timer, no polling. */
static void
events_connect (void)
{
  const char *cmd = "subscribe\n";

  if (EventFd >= 0)
    {
      mb_tray_app_set_poll_fd (App, -1);
      close (EventFd);
      EventFd = -1;
    }

  EventFd = ird_connect ();
  if (EventFd < 0)
    return;

  if (write (EventFd, cmd, strlen (cmd)) < 0)
    {
      close (EventFd);
      EventFd = -1;
      return;
    }

  mb_tray_app_set_poll_fd (App, EventFd);
}

static void
poll_callback (MBTrayApp *app)
{
  char buf[512];
  ssize_t got;

  (void) app;

  if (EventFd < 0)
    return;

  got = read (EventFd, buf, sizeof (buf) - 1);

  if (got <= 0)
    {
      /* ird went away or restarted. Drop the fd rather than spin on a dead
       * one; the next bubble open re-queries over a fresh connection and
       * re-subscribes. */
      mb_tray_app_set_poll_fd (App, -1);
      close (EventFd);
      EventFd = -1;
      return;
    }

  buf[got] = '\0';

  refresh_state ();
  bubble_redraw ();
}

/* ------------------------------------------------------------------ */
/* tray callbacks                                                       */

static void
load_icon (MBTrayApp *app)
{
  char *icon_path;

  (void) app;

  if (Icon != NULL)
    {
      mb_pixbuf_img_free (Pb, Icon);
      Icon = NULL;
    }

  icon_path = mb_dot_desktop_icon_get_full_path (ThemeName, ICON_SIZE,
						 ICON_NAME);
  if (icon_path == NULL
      || (Icon = mb_pixbuf_img_new_from_file (Pb, icon_path)) == NULL)
    {
      fprintf (stderr, "mb-irda: failed to load icon '%s'\n", ICON_NAME);
      exit (1);
    }

  free (icon_path);
}

static void
resize_callback (MBTrayApp *app, int w, int h)
{
  (void) app;

  if (IconScaled != NULL)
    mb_pixbuf_img_free (Pb, IconScaled);

  IconScaled = mb_pixbuf_img_scale (Pb, Icon, w, h);
}

static void
paint_callback (MBTrayApp *app, Drawable drw)
{
  MBPixbufImage *img_bg;
  int icon_w, icon_h;

  img_bg = mb_tray_app_get_background (app, Pb);
  if (img_bg == NULL)
    return;

  if (IconScaled != NULL)
    {
      icon_w = mb_pixbuf_img_get_width (IconScaled);
      icon_h = mb_pixbuf_img_get_height (IconScaled);

      mb_pixbuf_img_copy_composite (Pb, img_bg, IconScaled,
				    0, 0, icon_w, icon_h,
				    mb_tray_app_tray_is_vertical (app)
				      ? (mb_pixbuf_img_get_width (img_bg) - icon_w) / 2
				      : 0,
				    mb_tray_app_tray_is_vertical (app)
				      ? 0
				      : (mb_pixbuf_img_get_height (img_bg) - icon_h) / 2);
    }

  mb_pixbuf_img_render_to_drawable (Pb, img_bg, drw, 0, 0);
  mb_pixbuf_img_free (Pb, img_bg);
}

static void
theme_change_callback (MBTrayApp *app, char *theme_name)
{
  (void) app;

  if (theme_name == NULL)
    return;

  if (ThemeName != NULL)
    free (ThemeName);
  ThemeName = strdup (theme_name);

  load_icon (App);
  mb_tray_app_repaint (App);
}

static void
button_callback (MBTrayApp *app, int x, int y, Bool is_released)
{
  (void) app;
  (void) x;
  (void) y;

  if (!is_released)
    return;

  if (EventFd < 0)
    events_connect ();

  open_bubble ();
}

static void
xevent_callback (MBTrayApp *app, XEvent *ev)
{
  switch (ev->type)
    {
    case Expose:
      if (ev->xexpose.window == BubbleWin)
	bubble_redraw ();
      break;

    case ButtonPress:
      if (ev->xbutton.window == BubbleWin)
	bubble_button_press (ev->xbutton.x, ev->xbutton.y);
      else if (ev->xbutton.window != mb_tray_app_xwin (app))
	close_bubble ();
      break;

    case KeyPress:
      if (XLookupKeysym (&ev->xkey, 0) == XK_Escape)
	close_bubble ();
      break;

    default:
      break;
    }
}

int
main (int argc, char *argv[])
{
  struct timeval tv;

  App = mb_tray_app_new ((unsigned char *) "Infrared",
			 resize_callback,
			 paint_callback,
			 &argc,
			 &argv);
  if (App == NULL)
    {
      fprintf (stderr, "mb-irda: failed to create tray app\n");
      exit (1);
    }

  Pb = mb_pixbuf_new (mb_tray_app_xdisplay (App), mb_tray_app_xscreen (App));
  if (Pb == NULL)
    {
      fprintf (stderr, "mb-irda: failed to create pixbuf\n");
      exit (1);
    }

  MsgFont = mb_font_new_from_string (mb_tray_app_xdisplay (App),
				     MSG_FONT_SPEC);

  load_icon (App);

  mb_tray_app_set_button_callback (App, button_callback);
  mb_tray_app_set_xevent_callback (App, xevent_callback);
  mb_tray_app_set_theme_change_callback (App, theme_change_callback);

  /* Same dance mb-volume does: poll_callback can only be registered
   * through set_timeout_callback, which memcpy()s from its tv pointer
   * unconditionally -- NULL there is a segfault, not "no timeout". Register
   * with a throwaway value, then clear the timer with set_poll_timeout, so
   * the callback stays installed for the ird fd and an idle applet blocks
   * in select() with no timer at all. */
  tv.tv_sec  = 0;
  tv.tv_usec = 0;
  mb_tray_app_set_timeout_callback (App, poll_callback, &tv);
  mb_tray_app_set_poll_timeout (App, NULL);

  refresh_state ();
  events_connect ();

  mb_tray_app_main (App);

  return 0;
}
