#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what)
{
    checks++;
    if (strcmp(got, want) != 0) {
        failures++;
        printf("  FAIL: %s\n        got  [%s]\n        want [%s]\n", what, got, want);
    }
}

static void check_int(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s\n        got  %d\n        want %d\n", what, got, want);
    }
}

static void put(const char *path, const char *body)
{
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        printf("  FAIL: cannot write %s\n", path);
        failures++;
        return;
    }
    fputs(body, f);
    fclose(f);
}

int main(void)
{
    char dir[] = "/tmp/pikoemucfgXXXXXX";
    char entry[512], root[512];
    struct pikoemu_cfg c;

    printf("pikoemu-config-test\n");

    if (mkdtemp(dir) == NULL) {
        printf("cannot create scratch dir\n");
        return 1;
    }
    snprintf(entry, sizeof(entry), "%s/emulation.cfg", dir);
    snprintf(root, sizeof(root), "%s/root.cfg", dir);

    put(entry,
        "/mnt/card/Emulation/portrait.jar|J2ME|phoneme|a.desktop|/i.png|canvas=240x320\n"
        "/mnt/card/Emulation/land.jar|J2ME|phoneme|b.desktop|/i.png|canvas=320x240,title=Wide%20One\n"
        "/mnt/card/Emulation/square.jar|J2ME|phoneme|c.desktop|/i.png|canvas=240x240\n"
        "/mnt/card/Emulation/own.jar|J2ME|phoneme|d.desktop|/i.png|bezel=own.pkbz\n"
        "/mnt/card/Emulation/none.jar|J2ME|phoneme|e.desktop|/i.png|bezel=none\n"
        "/mnt/card/Emulation/rot.jar|J2ME|phoneme|f.desktop|/i.png|rotate=1,video=qvga\n");

    put(root,
        "@backend:phoneme|-|-|-|-|bezel=backend.pkbz\n"
        "@global|-|-|-|-|bezel=global.pkbz\n");

    setenv("EMULATION_CFG", entry, 1);
    setenv("PIKOEMU_ROOT_CFG", root, 1);

    check(pikoemu_load("/mnt/card/Emulation/portrait.jar", &c) == 1, "a listed rom is found");
    check_str(c.machine, "J2ME", "machine comes from field 1");
    check_str(c.backend, "phoneme", "backend comes from field 2");
    check_int(c.canvas_w, 240, "the declared canvas width is read");
    check_int(c.canvas_h, 320, "and its height");
    pikoemu_resolve(&c, 640, 480);
    check_int(c.canvas_w, 320, "a portrait canvas is swapped to landscape");
    check_int(c.canvas_h, 240, "landscape is always the resolved shape");

    check(pikoemu_load("/mnt/card/Emulation/land.jar", &c) == 1, "a landscape rom is found");
    pikoemu_resolve(&c, 640, 480);
    check_int(c.canvas_w, 320, "a landscape canvas is left alone");
    check_int(c.canvas_h, 240, "in both axes");
    check_str(c.title, "Wide One", "a %-escaped title is decoded");

    check(pikoemu_load("/mnt/card/Emulation/square.jar", &c) == 1, "a square rom is found");
    pikoemu_resolve(&c, 640, 480);
    check_int(c.canvas_w, 240, "a square canvas is never rotated");
    check_int(c.canvas_h, 240, "and keeps both sides equal");

    check(pikoemu_load("/mnt/card/Emulation/own.jar", &c) == 1, "a rom with its own bezel loads");
    check_int(c.has_bezel, 1, "and has one");
    check_str(c.bezel_image, "own.pkbz", "its own bezel wins over the directives");

    check(pikoemu_load("/mnt/card/Emulation/none.jar", &c) == 1, "bezel=none loads");
    check_int(c.has_bezel, 1, "bezel=none clears only the ENTRY bezel -- directives still apply");
    check_str(c.bezel_image, "backend.pkbz", "so the @backend bezel is inherited anyway");

    check(pikoemu_load("/mnt/card/Emulation/rot.jar", &c) == 1, "a rom with no bezel loads");
    check_int(c.rotate, 1, "rotate=1 is read");
    check_str(c.video_key, "qvga", "video=qvga is read");
    check_int(c.has_bezel, 1, "and it inherits a bezel from the directives");
    check_str(c.bezel_image, "backend.pkbz", "@backend wins over @global");

    put(root, "@global|-|-|-|-|bezel=global.pkbz\n");
    check(pikoemu_load("/mnt/card/Emulation/rot.jar", &c) == 1, "reload with no @backend line");
    check_str(c.bezel_image, "global.pkbz", "@global is the fallback");

    put(root, "");
    check(pikoemu_load("/mnt/card/Emulation/rot.jar", &c) == 1, "reload with an empty root cfg");
    check_int(c.has_bezel, 0, "an empty root cfg leaves no bezel at all");
    check(pikoemu_load("/mnt/card/Emulation/none.jar", &c) == 1, "and bezel=none reloads");
    check_int(c.has_bezel, 0, "bezel=none plus an empty root cfg is the only way to run naked");

    check(pikoemu_load("/mnt/card/Emulation/absent.jar", &c) == 0, "an unlisted rom is not found");
    check_str(c.rom, "/mnt/card/Emulation/absent.jar", "but its path is still recorded");
    pikoemu_resolve(&c, 640, 480);
    check_int(c.canvas_w, 320, "an unknown rom still gets the default landscape canvas");
    check_int(c.canvas_h, 240, "at QVGA size");

    unsetenv("EMULATION_CFG");
    check(pikoemu_load("/mnt/card/Emulation/portrait.jar", &c) == 0,
          "without EMULATION_CFG the real card path is consulted, not the fixture");

    {
        char zroot[64];
        pikorom_media_root_for("/mnt/card/x.jar", zroot, sizeof(zroot));
        check_str(zroot, "/mnt/card/.zaurus", "SD media root");
        pikorom_media_root_for("/mnt/cf/x.jar", zroot, sizeof(zroot));
        check_str(zroot, "/mnt/cf/.zaurus", "CF media root");
        pikorom_media_root_for("/usr/local/x.jar", zroot, sizeof(zroot));
        check_str(zroot, "/usr/local/.zaurus", "internal media root");
    }

    unlink(entry);
    unlink(root);
    rmdir(dir);

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("PIKOEMU-CONFIG-TEST: FAIL\n");
        return 1;
    }
    printf("PIKOEMU-CONFIG-TEST: PASS\n");
    return 0;
}
