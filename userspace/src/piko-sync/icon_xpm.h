/*
 * icon_xpm.h -- the app icon (userspace/desktop/piko-sync-server.png,
 * converted with `magick megasynching.png icon_xpm.h`'s XPM output,
 * pasted in below), shared by piko-sync-client and piko-sync-server as
 * their FLTK window icon.
 *
 * XPM, not Fl_PNG_Image: the staged libfltk.so.1.3 this project cross-
 * builds against turned out to have no PNG codec compiled in at all
 * (found 2026-08-03 -- config.h claims HAVE_LIBPNG, but the actual
 * makeinclude only lists jpeg/ in IMAGEDIRS and the linked .so has no
 * Fl_PNG_Image symbols whatsoever; stale/inconsistent from an earlier
 * configure run). XPM support is compiled into FLTK's core unconditionally
 * -- not an optional image codec -- so embedding the icon as XPM sidesteps
 * that dependency entirely rather than chasing a FLTK rebuild. A 32x32
 * icon this size is also small enough that shipping it as source beats a
 * separate runtime asset + install-path resolution.
 */

#ifndef PIKO_SYNC_ICON_XPM_H
#define PIKO_SYNC_ICON_XPM_H

/* XPM */
static const char *piko_sync_icon_xpm[] = {
/* columns rows colors chars-per-pixel */
"32 32 11 1 ",
"  c None",
". c black",
"X c #336666",
"o c #339966",
"O c #FF66CC",
"+ c #33FF99",
"@ c gray60",
"# c #A0A0A4",
"$ c #FFCC99",
"% c #DDDDDD",
"& c white",
/* pixels */
"                                ",
"  ..................... .....   ",
" .+++.+++.%%%%%%%%%%%%..+++++.  ",
" .+oo.ooo.%%%%%%%....%.++ooooX. ",
" .+oo.ooo.%%%%%%%.oo.%.ooooooX.#",
" .+oo.ooo.%%%%%%%.oo.%.ooooooX.#",
" .+oo.ooo.%%%%%%%.oo.%.ooooooX.#",
" .+oo.ooo.%%%%%%%.oo.%.ooooooX.#",
" .+oo.ooo.%%%%%%%.oo.%.ooooooX.#",
" .+oo.ooo.%%%%%%%....%.ooooooX.#",
" .+oo.ooo.%%%%%%%%%%%%.ooooooX.#",
" .+ooo................oooooooX.#",
" .+ooooooooooooooooooooooooooX.#",
" .+ooooooooooooooooooooooooooX.#",
" .+ooo....................oooX.#",
" .+oo.&&&&&&&&&&&&&&&&&&&&.ooX.#",
" .+oo.&&&&&&&&&&&&&&&&&&&&.ooX.#",
" .+oo.&&&&&..&&&&&&..&&&&&.ooX.#",
" .+oo.&&&&.&&.&&&&.&&.&&&&.ooX.#",
" .+oo.&&&&.&&.&&&&.&&.&&&&.ooX.#",
" .+oo.&&&&.&&.&&&&.&&.&&&&.ooX.#",
" .+oo.&&&&&&&&&&&&&&&&&&&&.ooX.#",
" .+oo.&$$&&&&&&&&&&&&&&$$&.ooX.#",
" .+oo.$$$$&..........&$$$$.ooX.#",
" .+oo.$$$$&.OOOOOOOO.&$$$$.ooX.#",
" .+oo.&$$&&.OOOOOOOO.&&$$&.ooX.#",
" .+oo.&&&&&&.OOOOOO.&&&&&&.ooX.#",
" .+oo.&&&&&&&......&&&&&&&.ooX.#",
" .+oo.&&&&&&&&&&&&&&&&&&&&.ooX.#",
" .XXX.@@@@@@@@@@@@@@@@@@@@.XXX.#",
"  ............................##",
"   ############################ "
};

#endif /* PIKO_SYNC_ICON_XPM_H */
