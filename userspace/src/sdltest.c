/*
 * sdltest -- dummy smoke test for the cross-built libSDL-1.2.so.0
 * (tools/build-sdl.sh) on the Zaurus's w100 framebuffer (/dev/fb0,
 * 640x480, 16bpp -- see userspace/src/fbtest.c for the equivalent
 * raw-fbdev test this mirrors).
 *
 * Does the minimum needed to prove SDL itself can drive the hardware:
 * init video, set the native mode, draw a simple pattern so a framebuffer
 * capture (tools/fbgrab, decode-fb.py) can tell a working blit from a
 * black screen, hold it on screen, then quit cleanly. No input, no
 * audio, no assets -- see tools/build-sdl.sh's header for why those are
 * out of scope for this first pass.
 *
 * Usage: sdltest [seconds-to-hold, default 5]
 * Exit codes: 0 ok, 1 SDL_Init/SetVideoMode failed (message on stderr).
 */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

static void fill_rect(SDL_Surface *s, int x, int y, int w, int h, Uint32 c)
{
    SDL_Rect r;
    r.x = (Sint16)x;
    r.y = (Sint16)y;
    r.w = (Uint16)w;
    r.h = (Uint16)h;
    SDL_FillRect(s, &r, c);
}

int main(int argc, char **argv)
{
    int hold_seconds = 5;
    SDL_Surface *screen;
    int w, h, i, band;
    Uint32 colors[6];
    char driver_name[64];

    if (argc > 1) {
        hold_seconds = atoi(argv[1]);
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "sdltest: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    screen = SDL_SetVideoMode(640, 480, 16, SDL_SWSURFACE);
    if (screen == NULL) {
        fprintf(stderr, "sdltest: SDL_SetVideoMode failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_WM_SetCaption("sdltest", "sdltest");

    w = screen->w;
    h = screen->h;
    SDL_VideoDriverName(driver_name, sizeof(driver_name));
    fprintf(stderr, "sdltest: video driver=%s mode=%dx%d bpp=%d\n",
            driver_name, w, h, screen->format->BitsPerPixel);

    /* Six vertical color bars, classic test-pattern layout, same idea as
     * fbtest.c's pattern -- easy to eyeball or diff a captured frame
     * against. */
    colors[0] = SDL_MapRGB(screen->format, 255, 255, 255);
    colors[1] = SDL_MapRGB(screen->format, 255, 255, 0);
    colors[2] = SDL_MapRGB(screen->format, 0, 255, 255);
    colors[3] = SDL_MapRGB(screen->format, 0, 255, 0);
    colors[4] = SDL_MapRGB(screen->format, 255, 0, 255);
    colors[5] = SDL_MapRGB(screen->format, 255, 0, 0);

    fill_rect(screen, 0, 0, w, h, SDL_MapRGB(screen->format, 0, 0, 0));
    band = w / 6;
    for (i = 0; i < 6; i++) {
        fill_rect(screen, i * band, 0, (i == 5) ? (w - i * band) : band, h, colors[i]);
    }

    /* A white border so a slightly-off video mode (wrong pitch/offset)
     * is obvious even if the color bars alone look plausible. */
    fill_rect(screen, 0, 0, w, 4, colors[0]);
    fill_rect(screen, 0, h - 4, w, 4, colors[0]);
    fill_rect(screen, 0, 0, 4, h, colors[0]);
    fill_rect(screen, w - 4, 0, 4, h, colors[0]);

    SDL_UpdateRect(screen, 0, 0, 0, 0);

    fprintf(stderr, "sdltest: drew color bars on %dx%d, holding %d second(s)\n",
            w, h, hold_seconds);
    SDL_Delay((Uint32)hold_seconds * 1000);

    SDL_Quit();
    return 0;
}
