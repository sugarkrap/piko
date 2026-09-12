#ifndef PIKOROM_H
#define PIKOROM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pikorom_media {
    PIKOROM_MEDIA_NAND = 0,
    PIKOROM_MEDIA_SD   = 1,
    PIKOROM_MEDIA_CF   = 2
};

struct pikorom_entry {
    const char *path;
    const char *machine;
    const char *backend;
    const char *desktop;
    const char *icon;
    const char *options;
};

struct pikorom_jar_meta {
    const char *title;
    const char *icon_name;
    const void *icon_png;
    size_t      icon_png_len;
};

struct pikorom_bezel {
    const char  *name;
    int          media;
    unsigned int size;
    unsigned int width, height;
    unsigned int screen_x, screen_y, screen_w, screen_h;
    const char  *source;
};

typedef struct pikorom_db pikorom_db;
typedef struct pikorom_blob pikorom_blob;
typedef struct pikorom_bezel_list pikorom_bezel_list;
typedef struct pikorom_bg_list pikorom_bg_list;

struct pikorom_background {
    const char  *name;
    int          media;
    unsigned int width;
    unsigned int height;
};

const char *pikorom_media_name(int media);
int         pikorom_media_of_path(const char *path);
int         pikorom_media_present(int media);

pikorom_db          *pikorom_db_open(void);
void                 pikorom_db_close(pikorom_db *db);
int                  pikorom_db_count(const pikorom_db *db);
const struct pikorom_entry *pikorom_db_at(const pikorom_db *db, int index);
int                  pikorom_db_find(const pikorom_db *db, const char *rom_path);
pikorom_blob        *pikorom_db_records(const pikorom_db *db);

int  pikorom_cfg_path_for(const char *rom_path, char *out, size_t outlen);
int  pikorom_media_root_for(const char *rom_path, char *out, size_t outlen);
int  pikorom_entry_lookup(const char *cfg_path, const char *key,
                          char *machine, size_t machinelen,
                          char *backend, size_t backendlen,
                          char *options, size_t optionslen);
void pikorom_option_unescape(const char *in, char *out, size_t outlen);

int         pikorom_detect_machine(const char *path, char *out, size_t outlen);
const char *pikorom_backend_for(const char *machine);
int         pikorom_is_directive(const char *rom_path);

int pikorom_install(const char *rom_path, const char *machine, const char *options,
                    char *status, size_t statuslen);
int pikorom_remove(const char *rom_path, char *err, size_t errlen);
int pikorom_set_icon(const char *rom_path, const void *png, size_t len,
                     char *err, size_t errlen);
int pikorom_set_option(const char *rom_path, const char *key, const char *value,
                       char *err, size_t errlen);
int pikorom_set_backend(const char *rom_path, const char *backend,
                        char *err, size_t errlen);
int pikorom_apply(const char *rom_path, const char *backend,
                  const char *const *keys, const char *const *values, int option_count,
                  const void *icon, size_t icon_len,
                  char *err, size_t errlen);
int pikorom_option_get(const char *options, const char *key, char *out, size_t outlen);

int  pikorom_sync_launchers(void);
void pikorom_migrate_legacy(void);

int pikorom_read_jar_meta(const char *path, struct pikorom_jar_meta *out);
void pikorom_free_jar_meta(struct pikorom_jar_meta *meta);

pikorom_blob *pikorom_blob_read(const char *path);
const void   *pikorom_blob_data(const pikorom_blob *b);
size_t        pikorom_blob_size(const pikorom_blob *b);
void          pikorom_blob_free(pikorom_blob *b);

pikorom_bezel_list         *pikobezel_list(void);
int                         pikobezel_count(const pikorom_bezel_list *l);
const struct pikorom_bezel *pikobezel_at(const pikorom_bezel_list *l, int index);
void                        pikobezel_list_free(pikorom_bezel_list *l);

int           pikobezel_name_safe(const char *name);
int           pikobezel_media_of(const char *name);
int           pikobezel_path_for(int media, const char *name, char *out, size_t outlen);
pikorom_blob *pikobezel_read(const char *name);
pikorom_blob *pikobezel_records(const pikorom_bezel_list *l);
int           pikobezel_write(int media, const char *name, const void *data, size_t len,
                              char *err, size_t errlen);
int           pikobezel_remove(const char *name);
int           pikobezel_set_rect(const char *name, unsigned int x, unsigned int y,
                                 unsigned int w, unsigned int h);

pikorom_bg_list                 *pikobg_list(void);
int                              pikobg_count(const pikorom_bg_list *l);
const struct pikorom_background *pikobg_at(const pikorom_bg_list *l, int index);
void                             pikobg_list_free(pikorom_bg_list *l);

int           pikobg_name_safe(const char *name);
int           pikobg_path_for(const char *name, char *out, size_t outlen);
pikorom_blob *pikobg_read(const char *name);
pikorom_blob *pikobg_records(const pikorom_bg_list *l);
int           pikobg_decode_header(const void *head, size_t len, unsigned int *w,
                                   unsigned int *h, size_t *pixel_offset);
int           pikobg_for_bezel(const char *bezel, char *out, size_t outlen);

#ifdef __cplusplus
}
#endif

#endif
