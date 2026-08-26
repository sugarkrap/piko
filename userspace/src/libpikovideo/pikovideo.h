#ifndef PIKOVIDEO_H
#define PIKOVIDEO_H

#ifdef __cplusplus
extern "C" {
#endif

enum pikovideo_mode {
    PIKOVIDEO_QVGA_NORMAL = 0,
    PIKOVIDEO_QVGA_FAST,
    PIKOVIDEO_VGA_NORMAL,
    PIKOVIDEO_VGA_FAST,
    PIKOVIDEO_MODE_COUNT
};

enum pikovideo_driver {
    PIKOVIDEO_DRIVER_FB = 0,
    PIKOVIDEO_DRIVER_X11
};

const char *pikovideo_mode_key(enum pikovideo_mode m);
const char *pikovideo_mode_label(enum pikovideo_mode m);
int         pikovideo_mode_from_key(const char *key, enum pikovideo_mode *out);
int         pikovideo_mode_is_qvga(enum pikovideo_mode m);
int         pikovideo_mode_is_fast_pll(enum pikovideo_mode m);
enum pikovideo_mode pikovideo_mode_from_flags(int qvga, int fast_pll);
void        pikovideo_mode_to_flags(enum pikovideo_mode m, int *qvga, int *fast_pll);

void pikovideo_mode_size(enum pikovideo_mode m, int *w, int *h);

void pikovideo_set_trace(void (*fn)(const char *msg));

int  pikovideo_apply(enum pikovideo_mode m, enum pikovideo_driver drv);
void pikovideo_restore(void);
int  pikovideo_current_xres(void);

enum pikovideo_mode pikovideo_load_config(void);
int  pikovideo_save_config(enum pikovideo_mode m);

#ifdef __cplusplus
}
#endif

#endif
