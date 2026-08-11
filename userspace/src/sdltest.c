
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
