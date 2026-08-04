/*
 * piko-panel.H -- the contract a settings panel implements.
 *
 * A "panel" is one settings app's entire UI and logic packaged as an
 * Fl_Group subclass, so that exactly the same code serves both ways of
 * reaching it:
 *
 *   standalone   a thin main() puts the panel in a window of its own
 *                (userspace/src/mb-wallpaper-picker.cxx is the example --
 *                the whole program is about fifteen lines)
 *
 *   embedded     piko-settings constructs the same class inside its own
 *                window when its row is tapped, with a Back button in the
 *                header, XFCE-style
 *
 * This is the in-process answer to what XFCE does with GtkPlug/GtkSocket
 * and --socket-id. XFCE embeds a second *process* over XEmbed; we embed a
 * widget. On a 400MHz PXA255 with 52 MB of RAM that difference is the
 * whole point: no second FLTK process, no X reparenting, no hand-rolled
 * _XEMBED focus protocol (FLTK 1.3 implements none of it), and the code is
 * shared by construction rather than by both modes happening to run the
 * same binary.
 *
 * WRITING ONE
 *
 *   1. Subclass Fl_Group. Lay out inside the (X,Y,W,H) you are given and
 *      never assume it is the whole window -- embedded, you sit below
 *      piko-settings' 52px header.
 *   2. Override resize() and re-lay-out if the width changed. This device
 *      rotates live (flipd, docs/HOWTO-SCREEN-ROTATION.md), and it costs
 *      nothing to get right at construction time.
 *   3. Expose a factory of type PikoPanelFactory.
 *   4. Register it in the PANELS[] table in piko-settings.cxx, and put
 *      X-Piko-Settings-Panel=<name> in the .desktop file.
 *
 * An entry whose .desktop names a panel this build does not have falls
 * back to launching the program as a separate process, so a .desktop file
 * from a newer package cannot produce a dead row.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef PIKO_PANEL_H
#define PIKO_PANEL_H

#include <FL/Fl_Group.H>
#include <FL/Fl_Shared_Image.H>

/* Every panel is constructed the same way, so piko-settings can hold a
 * table of these and never know what any particular panel is. */
typedef Fl_Group *(*PikoPanelFactory)(int X, int Y, int W, int H);

/*
 * fl_register_images() exactly once per process.
 *
 * FLTK 1.3's is NOT idempotent -- src/fl_images_core.cxx is a bare
 * Fl_Shared_Image::add_handler(fl_check_images) with no guard -- so
 * calling it from both a host and a panel it embeds registers the handler
 * twice and every later image load runs it twice.
 *
 * Both callers genuinely need it independently (piko-settings decodes the
 * row icons before any panel exists; a standalone panel has no host at
 * all), so neither can simply defer to the other. An inline function's
 * function-local static is one object across all translation units, which
 * makes this the cheapest correct place to put the guard.
 */
inline void piko_register_images(void)
{
    static bool registered = false;

    if (!registered) {
        fl_register_images();
        registered = true;
    }
}

#endif /* PIKO_PANEL_H */
