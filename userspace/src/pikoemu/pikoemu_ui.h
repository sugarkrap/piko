#ifndef PIKOEMU_UI_H
#define PIKOEMU_UI_H

#include "SDL.h"

#define PIKOEMU_UI_DIR    "/usr/local/share/piko/ui"
#define PIKOEMU_UI_MAX_W  250
#define PIKOEMU_UI_MAX    1

typedef void (*ui_restore_fn)(void *user, int x, int y, int w, int h);

int  ui_load(const char *dir);
void ui_attach(SDL_Surface *surface, ui_restore_fn restore, void *user);
void ui_set_rotated(int rotated);
int  ui_rotated(void);

void ui_notify(const char *title, const char *desc, const char *icon,
               unsigned int now_ms);
int  ui_notify_pen(int x, int y);
int  ui_notify_tick(unsigned int now_ms);
int  ui_notify_repaint_over(int x, int y, int w, int h,
                            SDL_Rect *out, int max);
int  ui_notify_active(void);

#endif
