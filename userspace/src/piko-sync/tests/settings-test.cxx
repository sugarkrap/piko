
#include "../settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

using namespace piko_sync;

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

static void check_str(const std::string &got, const std::string &want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s\n        got  [%s]\n        want [%s]\n",
               what, got.c_str(), want.c_str());
    }
}

static void check_int(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s\n        got  [%d]\n        want [%d]\n", what, got, want);
    }
}

static std::string g_scratch;

static std::string scratch(const char *leaf)
{
    return g_scratch + "/" + leaf;
}

static void write_file(const std::string &path, const char *contents)
{
    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        printf("  FAIL: could not create %s\n", path.c_str());
        failures++;
        return;
    }
    fputs(contents, f);
    fclose(f);
}

static std::string read_file(const std::string &path)
{
    std::string out;
    FILE *f = fopen(path.c_str(), "r");
    if (!f)
        return out;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

static void test_roundtrip()
{
    std::string path = scratch("roundtrip.cfg");

    Settings out;
    out.set("build.repo_root", "/mnt/data/Code/piko");
    out.set("build.jobs", "");
    out.set_int("build.answer", 42);
    out.set_bool("build.no_backup", true);
    out.set_bool("build.skip_st", false);
    check(out.save_to(path), "save_to() reports success");

    Settings in;
    check(in.load_from(path), "load_from() reports success");
    check_str(in.get("build.repo_root"), "/mnt/data/Code/piko", "string value survives");
    check_str(in.get("build.jobs", "fallback"), "", "an empty value is a value, not a miss");
    check_int(in.get_int("build.answer", 0), 42, "int value survives");
    check(in.get_bool("build.no_backup", false), "true bool survives");
    check(!in.get_bool("build.skip_st", true), "false bool survives");
}

static void test_missing_file_is_not_an_error_for_callers()
{
    Settings s;
    check(!s.load_from(scratch("does-not-exist.cfg")), "load of a missing file returns false");
    check_str(s.get("anything", "default"), "default", "every get() falls back after a failed load");
    check(!s.has("anything"), "has() is false after a failed load");
}

static void test_set_replaces_rather_than_appends()
{
    std::string path = scratch("replace.cfg");

    Settings s;
    s.set("transfer.address", "10.208.47.2");
    s.set("transfer.address", "192.168.1.5");
    check(s.save_to(path), "save_to() reports success");

    std::string text = read_file(path);
    size_t first = text.find("transfer.address");
    check(first != std::string::npos, "the key is present in the file");
    check(text.find("transfer.address", first + 1) == std::string::npos,
          "the key appears exactly once in the file");

    Settings in;
    in.load_from(path);
    check_str(in.get("transfer.address"), "192.168.1.5", "the later set() won");
}

static void test_parsing_tolerates_hand_editing()
{
    std::string path = scratch("handedited.cfg");
    write_file(path,
               "# a comment\n"
               "; another comment style\n"
               "\n"
               "   spaced.key   =   spaced value   \n"
               "empty.value =\n"
               "no.equals.sign here\n"
               "= headless value\n"
               "path.with.equals = /tmp/a=b\n"
               "last.key = kept\n");

    Settings s;
    check(s.load_from(path), "a hand-edited file loads");
    check_str(s.get("spaced.key"), "spaced value", "key and value are trimmed");
    check_str(s.get("empty.value", "fallback"), "", "an empty value parses as empty");
    check(!s.has("no.equals.sign"), "a line without '=' is skipped");
    check(!s.has(""), "a line with an empty key is skipped");
    check_str(s.get("path.with.equals"), "/tmp/a=b", "only the FIRST '=' separates");
    check_str(s.get("last.key"), "kept",
              "parsing continues past malformed lines to the end of the file");
}

static void test_unknown_keys_survive_a_save()
{
    std::string path = scratch("unknown.cfg");
    write_file(path, "build.from_the_future = keep me\ntransfer.address = 10.208.47.2\n");

    Settings s;
    s.load_from(path);
    s.set("transfer.address", "10.0.0.9");
    check(s.save_to(path), "save_to() reports success");

    Settings in;
    in.load_from(path);
    check_str(in.get("build.from_the_future"), "keep me",
              "a key this build never heard of is written back, not dropped");
    check_str(in.get("transfer.address"), "10.0.0.9", "the known key still updated");
}

static void test_get_int_rejects_junk()
{
    std::string path = scratch("junk.cfg");
    write_file(path,
               "a = 12\n"
               "b = 12x\n"
               "c = yes\n"
               "d =\n"
               "e = -3\n");

    Settings s;
    s.load_from(path);
    check_int(s.get_int("a", 7), 12, "a clean integer parses");
    check_int(s.get_int("b", 7), 7, "trailing junk falls back");
    check_int(s.get_int("c", 7), 7, "a non-number falls back");
    check_int(s.get_int("d", 7), 7, "an empty value falls back");
    check_int(s.get_int("e", 7), -3, "a negative integer parses");
    check_int(s.get_int("missing", 7), 7, "a missing key falls back");
    check(s.get_bool("c", true), "get_bool falls back rather than reading junk as false");
}

static void test_save_creates_the_directory_tree()
{
    std::string dir = scratch("deep/er/still");
    std::string path = dir + "/settings.cfg";

    Settings s;
    s.set("k", "v");
    check(s.save_to(path), "save_to() creates missing parent directories");

    struct stat st;
    check(stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode), "the file exists afterwards");
    check(stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode), "the directory tree exists");
}

static void test_save_leaves_no_temp_file_behind()
{
    std::string path = scratch("atomic.cfg");
    Settings s;
    s.set("k", "v");
    s.save_to(path);

    struct stat st;
    check(stat((path + ".tmp").c_str(), &st) != 0,
          "the .tmp used for the atomic rename is gone after a successful save");
}

static void test_save_failure_is_reported_not_fatal()
{
    std::string blocker = scratch("iam-a-file");
    write_file(blocker, "not a directory\n");

    Settings s;
    s.set("k", "v");
    check(!s.save_to(blocker + "/sub/settings.cfg"),
          "save_to() returns false when the directory can't be created");
    check(!s.save_to(""), "save_to() returns false for an empty path");
}

static void test_default_path_follows_home_and_xdg()
{
    std::string home = scratch("fakehome");
    std::string xdg = scratch("fakexdg");

    setenv("HOME", home.c_str(), 1);
    unsetenv("XDG_CONFIG_HOME");
    check_str(Settings::file_path(), home + "/.config/piko-sync/settings.cfg",
              "$HOME/.config/piko-sync/settings.cfg by default");

    setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
    check_str(Settings::file_path(), xdg + "/piko-sync/settings.cfg",
              "an absolute XDG_CONFIG_HOME wins over $HOME");

    setenv("XDG_CONFIG_HOME", "relative/path", 1);
    check_str(Settings::file_path(), home + "/.config/piko-sync/settings.cfg",
              "a relative XDG_CONFIG_HOME is ignored");

    unsetenv("XDG_CONFIG_HOME");
    unsetenv("HOME");
    check(Settings::file_path().empty(), "no HOME and no XDG means no path at all");
    check(!Settings().save(), "save() to an empty path fails rather than writing somewhere odd");

    setenv("HOME", home.c_str(), 1);
}

static void test_default_path_roundtrip()
{
    std::string home = scratch("fakehome2");
    setenv("HOME", home.c_str(), 1);
    unsetenv("XDG_CONFIG_HOME");

    Settings out;
    out.set("transfer.address", "10.208.47.2");
    check(out.save(), "save() to the default path succeeds");

    struct stat st;
    check(stat((home + "/.config/piko-sync/settings.cfg").c_str(), &st) == 0,
          "the file lands at $HOME/.config/piko-sync/settings.cfg");

    Settings in;
    check(in.load(), "load() from the default path succeeds");
    check_str(in.get("transfer.address"), "10.208.47.2", "the value survives the default path");
}

int main()
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";

    char tmpl[512];
    snprintf(tmpl, sizeof(tmpl), "%s/piko-sync-settings-test-XXXXXX", tmp);
    if (!mkdtemp(tmpl)) {
        printf("could not create a scratch directory under %s\n", tmp);
        return 1;
    }
    g_scratch = tmpl;

    const char *real_home = getenv("HOME");
    std::string saved_home = real_home ? real_home : "";

    test_roundtrip();
    test_missing_file_is_not_an_error_for_callers();
    test_set_replaces_rather_than_appends();
    test_parsing_tolerates_hand_editing();
    test_unknown_keys_survive_a_save();
    test_get_int_rejects_junk();
    test_save_creates_the_directory_tree();
    test_save_leaves_no_temp_file_behind();
    test_save_failure_is_reported_not_fatal();
    test_default_path_follows_home_and_xdg();
    test_default_path_roundtrip();

    if (saved_home.empty()) unsetenv("HOME");
    else                    setenv("HOME", saved_home.c_str(), 1);

    std::string cleanup = "rm -rf '" + g_scratch + "'";
    if (system(cleanup.c_str()) != 0)
        printf("  note: could not clean up %s\n", g_scratch.c_str());

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("SETTINGS-TEST: FAIL\n");
        return 1;
    }
    printf("SETTINGS-TEST: PASS\n");
    return 0;
}
