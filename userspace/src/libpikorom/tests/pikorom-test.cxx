#include "pikorom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char *what)
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
    if (strcmp(got ? got : "(null)", want) != 0) {
        failures++;
        printf("  FAIL: %s\n        got  [%s]\n        want [%s]\n",
               what, got ? got : "(null)", want);
    }
}

static void write_bytes(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("  FAIL: cannot create fixture %s\n", path);
        failures++;
        return;
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

int main()
{
    printf("pikorom-test\n");

    check_str(pikorom_media_name(PIKOROM_MEDIA_SD), "SD", "SD media name");
    check_str(pikorom_media_name(PIKOROM_MEDIA_NAND), "NAND", "NAND media name");
    check_str(pikorom_media_name(PIKOROM_MEDIA_CF), "CF", "CF media name");

    check(pikorom_is_directive("@global") == 1, "@global is a directive");
    check(pikorom_is_directive("@backend:phoneme") == 1, "@backend is a directive");
    check(pikorom_is_directive("/mnt/card/Emulation/x.jar") == 0, "a real path is not");

    check_str(pikorom_backend_for("J2ME"), "phoneme", "J2ME runs on phoneme");
    check_str(pikorom_backend_for("SNES"), "", "SNES has no wired backend");
    check_str(pikorom_backend_for(0), "", "a null machine is not a crash");

    {
        char buf[64];
        check(pikorom_option_get("heavy=1,media=SD", "media", buf, sizeof(buf)) == 1,
              "an option is found");
        check_str(buf, "SD", "and read back");
        check(pikorom_option_get("heavy=1,media=SD", "rotate", buf, sizeof(buf)) == 0,
              "a missing option reports absent");
        check(pikorom_option_get(0, "media", buf, sizeof(buf)) == 0,
              "a null option string is not a crash");
    }

    {
        char tiny[4];
        pikorom_option_get("title=abcdefghijklmnop", "title", tiny, sizeof(tiny));
        check(strlen(tiny) == 3, "a short buffer truncates instead of overflowing");
        check(tiny[3] == '\0', "and stays terminated");
    }

    check(pikobezel_name_safe("phone.pkbz") == 1, "an ordinary bezel name is safe");
    check(pikobezel_name_safe("../../etc/passwd") == 0, "traversal is refused");
    check(pikobezel_name_safe("a/b.pkbz") == 0, "a slash is refused");
    check(pikobezel_name_safe("") == 0, "an empty name is refused");
    check(pikobezel_name_safe("..") == 0, "dot-dot is refused");
    check(pikobezel_name_safe(0) == 0, "a null name is not a crash");

    {
        char path[256];
        check(pikobezel_path_for(PIKOROM_MEDIA_SD, "../x", path, sizeof(path)) == 0,
              "an unsafe name yields no path");
    }

    {
        char tmpl[] = "/tmp/pikorom-testXXXXXX";
        char *dir = mkdtemp(tmpl);
        check(dir != 0, "scratch dir created");
        if (dir) {
            char jar[512], jad[512], snes[512], junk[512];
            snprintf(jar, sizeof(jar), "%s/game.jar", dir);
            snprintf(jad, sizeof(jad), "%s/game.jad", dir);
            snprintf(snes, sizeof(snes), "%s/game.smc", dir);
            snprintf(junk, sizeof(junk), "%s/game.bin", dir);

            write_bytes(jar, "PK\003\004rest of a zip", 18);
            const char *jadtext = "MIDlet-Name: Crosspix\nMIDlet-1: x,,y\n";
            write_bytes(jad, jadtext, strlen(jadtext));

            unsigned char rom[0x10000];
            memset(rom, 0, sizeof(rom));
            rom[0x7fdc] = 0x34;
            rom[0x7fdd] = 0x12;
            rom[0x7fde] = (unsigned char)~0x34;
            rom[0x7fdf] = (unsigned char)~0x12;
            write_bytes(snes, rom, sizeof(rom));

            write_bytes(junk, "not a rom at all", 16);

            char out[32];
            check(pikorom_detect_machine(jar, out, sizeof(out)) == 1
                  && strcmp(out, "J2ME") == 0, "a .jar is detected as J2ME");
            check(pikorom_detect_machine(jad, out, sizeof(out)) == 1
                  && strcmp(out, "J2ME") == 0, "a .jad is detected as J2ME");
            check(pikorom_detect_machine(snes, out, sizeof(out)) == 1
                  && strcmp(out, "SNES") == 0, "a valid SNES header is detected");
            check(pikorom_detect_machine(junk, out, sizeof(out)) == 0,
                  "an unrecognised file is not claimed");
            check(pikorom_detect_machine("/nonexistent/x.smc", out, sizeof(out)) == 0,
                  "a missing file is not claimed");
            check(pikorom_detect_machine(0, out, sizeof(out)) == 0,
                  "a null path is not a crash");

            pikorom_blob *b = pikorom_blob_read(junk);
            check(b != 0, "a blob reads back");
            check(pikorom_blob_size(b) == 16, "with its length");
            check(b && memcmp(pikorom_blob_data(b), "not a rom at all", 16) == 0,
                  "and its bytes");
            pikorom_blob_free(b);
            check(pikorom_blob_read("/nonexistent/blob") == 0, "a missing blob is null");
            pikorom_blob_free(0);

            unlink(jar); unlink(jad); unlink(snes); unlink(junk);
            rmdir(dir);
        }
    }

    {
        char path[512];
        check(pikorom_cfg_path_for("/mnt/card/Emulation/x.jar", path, sizeof(path)) == 1,
              "an SD rom resolves a cfg path");
        check_str(path, "/mnt/card/.zaurus/emulation.cfg", "on the card");
        pikorom_cfg_path_for("/mnt/cf/Emulation/x.jar", path, sizeof(path));
        check_str(path, "/mnt/cf/.zaurus/emulation.cfg", "on CF");
        pikorom_cfg_path_for("/usr/local/Emulation/x.jar", path, sizeof(path));
        check_str(path, "/usr/local/.zaurus/emulation.cfg", "and on internal storage");
        check(pikorom_cfg_path_for(0, path, sizeof(path)) == 0, "a null rom path is refused");
    }

    {
        char tmpl[] = "/tmp/pikoromcfgXXXXXX";
        char *dir = mkdtemp(tmpl);
        check(dir != 0, "cfg scratch dir created");
        if (dir) {
            char cfg[512];
            snprintf(cfg, sizeof(cfg), "%s/emulation.cfg", dir);
            const char *body =
                "/mnt/card/Emulation/game.jar|J2ME|phoneme|game.desktop|/x.png|canvas=240x320,heavy=1\n"
                "@backend:phoneme|-|-|-|-|bezel=J2ME_Grey.pkbz\n"
                "@global|-|-|-|-|bezel=black,title=Piko%2FEmu\n";
            write_bytes(cfg, body, strlen(body));

            char machine[64], backend[64], opts[1024];
            check(pikorom_entry_lookup(cfg, "/mnt/card/Emulation/game.jar",
                                       machine, sizeof(machine), backend, sizeof(backend),
                                       opts, sizeof(opts)) == 1, "an entry is found");
            check_str(machine, "J2ME", "with its machine");
            check_str(backend, "phoneme", "and its backend");
            check_str(opts, "canvas=240x320,heavy=1", "and its option string");

            check(pikorom_entry_lookup(cfg, "@backend:phoneme",
                                       machine, sizeof(machine), backend, sizeof(backend),
                                       opts, sizeof(opts)) == 1,
                  "a @backend directive is looked up the same way");
            check_str(opts, "bezel=J2ME_Grey.pkbz", "with its options");

            check(pikorom_entry_lookup(cfg, "@global",
                                       machine, sizeof(machine), backend, sizeof(backend),
                                       opts, sizeof(opts)) == 1, "so is @global");

            char v[256];
            pikorom_option_get(opts, "title", v, sizeof(v));
            check_str(v, "Piko%2FEmu", "option_get returns the escaped value");
            char un[256];
            pikorom_option_unescape(v, un, sizeof(un));
            check_str(un, "Piko/Emu", "and unescape decodes it");

            machine[0] = 'x'; backend[0] = 'x'; opts[0] = 'x';
            check(pikorom_entry_lookup(cfg, "/not/in/the/file",
                                       machine, sizeof(machine), backend, sizeof(backend),
                                       opts, sizeof(opts)) == 0, "a missing entry reports absent");
            check(machine[0] == '\0' && backend[0] == '\0' && opts[0] == '\0',
                  "and clears the out buffers instead of leaving them stale");

            check(pikorom_entry_lookup("/nonexistent/emulation.cfg", "@global",
                                       machine, sizeof(machine), backend, sizeof(backend),
                                       opts, sizeof(opts)) == 0, "a missing cfg file is not a crash");
            check(pikorom_entry_lookup(cfg, 0, machine, sizeof(machine),
                                       backend, sizeof(backend), opts, sizeof(opts)) == 0,
                  "a null key is not a crash");

            pikorom_option_unescape(0, un, sizeof(un));
            check_str(un, "", "unescaping null yields empty");

            unlink(cfg);
            rmdir(dir);
        }
    }

    {
        struct pikorom_jar_meta meta;
        check(pikorom_read_jar_meta("/nonexistent/x.jar", &meta) == 0,
              "jar meta of a missing file fails cleanly");
        check(pikorom_read_jar_meta(0, &meta) == 0, "a null jar path is not a crash");
        pikorom_free_jar_meta(0);
    }

    {
        pikorom_db *db = pikorom_db_open();
        check(db != 0, "the db opens even with no media mounted");
        int n = pikorom_db_count(db);
        check(n >= 0, "and reports a count");
        check(pikorom_db_at(db, -1) == 0, "a negative index is null");
        check(pikorom_db_at(db, n) == 0, "one past the end is null");
        check(pikorom_db_find(db, "/definitely/not/here.smc") == -1, "an absent rom is -1");
        check(pikorom_db_find(db, 0) == -1, "a null lookup is -1");
        for (int i = 0; i < n; i++) {
            const struct pikorom_entry *e = pikorom_db_at(db, i);
            check(e != 0 && e->path != 0 && e->machine != 0 && e->options != 0,
                  "every listed entry has non-null strings");
        }
        pikorom_db_close(db);
        pikorom_db_close(0);
        check(pikorom_db_count(0) == 0, "counting a null db is 0");
    }

    {
        char err[256];
        err[0] = '\0';
        check(pikorom_install(0, "J2ME", "", err, sizeof(err)) == 0,
              "installing a null path fails");
        check(err[0] != '\0', "and says why");

        err[0] = '\0';
        check(pikorom_install("/tmp/x.smc", "SNES", "", err, sizeof(err)) == 0,
              "installing a machine with no backend fails");
        check(strstr(err, "no emulator backend") != 0, "with the backend reason");

        err[0] = '\0';
        check(pikorom_set_option("", "media", "SD", err, sizeof(err)) == 0,
              "setting an option on an empty path fails");
        check(pikorom_remove(0, err, sizeof(err)) == 0, "removing a null path fails");
    }

    {
        pikorom_bezel_list *l = pikobezel_list();
        check(l != 0, "the bezel list opens with no media");
        int n = pikobezel_count(l);
        check(pikobezel_at(l, -1) == 0, "a negative bezel index is null");
        check(pikobezel_at(l, n) == 0, "one past the end is null");
        for (int i = 0; i < n; i++) {
            const struct pikorom_bezel *b = pikobezel_at(l, i);
            check(b != 0 && b->name != 0 && b->source != 0, "every bezel has its strings");
        }
        pikobezel_list_free(l);
        pikobezel_list_free(0);
        check(pikobezel_count(0) == 0, "counting a null list is 0");
        check(pikobezel_read("../escape") == 0, "an unsafe bezel read is refused");
        check(pikobezel_remove("../escape") == 0, "an unsafe bezel delete is refused");
        check(pikobezel_set_rect("../escape", 0, 0, 1, 1) == 0,
              "an unsafe bezel rect patch is refused");
        check(pikobezel_write(PIKOROM_MEDIA_SD, "../escape", "x", 1, 0, 0) == 0,
              "an unsafe bezel write is refused");
    }

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("PIKOROM-TEST: FAIL\n");
        return 1;
    }
    printf("PIKOROM-TEST: PASS\n");
    return 0;
}
