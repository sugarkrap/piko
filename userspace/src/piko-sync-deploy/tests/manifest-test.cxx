
#include "../manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <string>

using namespace piko_sync;
using namespace piko_sync::deploy;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const std::string &what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("  FAIL: %s\n", what.c_str());
    }
}

static void check_str(const std::string &got, const std::string &want, const std::string &what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL: %s\n        got  [%s]\n        want [%s]\n",
               what.c_str(), got.c_str(), want.c_str());
    }
}

static std::string tmpdir;

static void mkdirs(const std::string &path)
{
    std::string cur;
    size_t i = 0;
    if (!path.empty() && path[0] == '/') { cur = "/"; i = 1; }
    while (i <= path.size()) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && cur != "/")
                mkdir(cur.c_str(), 0755);
            if (i < path.size())
                cur += "/";
        } else {
            cur += path[i];
        }
        i++;
    }
}

static std::string write_fixture(const std::string &relpath, const std::string &content)
{
    std::string full = tmpdir + "/" + relpath;
    std::string::size_type slash = full.find_last_of('/');
    if (slash != std::string::npos)
        mkdirs(full.substr(0, slash));
    std::ofstream f(full.c_str());
    f << content;
    f.close();
    if (relpath.find("sbin/") != std::string::npos || relpath.find("bin/") != std::string::npos)
        chmod(full.c_str(), 0755);
    return full;
}

static void test_yaml_lite_roundtrip()
{
    printf("yaml_lite: parses sections/entries, rejects malformed input\n");

    std::vector<yaml::Section> sec;
    std::string err;
    check(yaml::parse("a:\n  - k: v\n    k2: v2\nb:\n  - k: v3\n", sec, err), "valid doc parses");
    check(sec.size() == 2, "two sections");
    if (sec.size() == 2) {
        check_str(sec[0].name, "a", "first section name");
        check(sec[0].entries.size() == 1, "section a has one entry");
        check_str(sec[0].entries[0].get("k"), "v", "entry field k");
        check_str(sec[0].entries[0].get("k2"), "v2", "entry field k2 (continuation line)");
    }

    check(!yaml::parse("  - k: v\n", sec, err), "indented content with no section fails");
    check(!err.empty(), "error message set on failure");
}

static void test_shell_var_list()
{
    printf("read_shell_var_list: extracts a VAR=\"...\" block, skips blanks/comments\n");

    std::string f = write_fixture("tools/fake-modules.sh",
        "# header comment\n"
        "SOME_MODULES=\"\n"
        "a/b.ko\n"
        "\n"
        "# a comment inside the block\n"
        "c/d.ko\n"
        "\"\n"
        "OTHER=\"x\"\n");

    std::vector<std::string> items;
    std::string error;
    check(read_shell_var_list(f, "SOME_MODULES", items, error), "reads the variable");
    check(items.size() == 2, "blank line and comment line dropped");
    if (items.size() == 2) {
        check_str(items[0], "a/b.ko", "first item");
        check_str(items[1], "c/d.ko", "second item");
    }

    check(!read_shell_var_list(f, "NO_SUCH_VAR", items, error), "missing variable fails");
    check(!error.empty(), "missing variable sets an error");
}

static void test_put_files_from_dir()
{
    printf("put_files_from_dir: one step per regular file, non-recursive\n");

    write_fixture("pcm/a.conf", "a");
    write_fixture("pcm/b.conf", "b");
    write_fixture("pcm/sub/c.conf", "c");

    std::vector<Step> steps;
    std::string error;
    check(put_files_from_dir(tmpdir + "/pcm", "/usr/share/alsa/pcm", 0644, steps, error),
          "walks the directory");
    check(steps.size() == 2, "two regular files, subdirectory excluded");
}

static void test_put_files_from_tree_preserves_mode()
{
    printf("put_files_from_tree: recurses, preserves each file's own mode\n");

    std::string exe = write_fixture("tree/usr/bin/thing", "#!/bin/sh\n");
    chmod(exe.c_str(), 0755);
    std::string cfg = write_fixture("tree/etc/thing.conf", "x=1\n");
    chmod(cfg.c_str(), 0644);

    std::vector<Step> steps;
    std::string error;
    check(put_files_from_tree(tmpdir + "/tree", "/", steps, error), "walks recursively");
    check(steps.size() == 2, "both files found across subdirectories");

    bool found_exe = false, found_cfg = false;
    for (size_t i = 0; i < steps.size(); i++) {
        if (steps[i].remote_path == "/usr/bin/thing") {
            found_exe = true;
            check(steps[i].mode == 0755u, "executable's mode preserved as 0755");
        }
        if (steps[i].remote_path == "/etc/thing.conf") {
            found_cfg = true;
            check(steps[i].mode == 0644u, "config file's mode preserved as 0644");
        }
    }
    check(found_exe, "found the binary at its mirrored path");
    check(found_cfg, "found the config at its mirrored path");
}

static DeployContext make_fixture_context()
{
    DeployContext ctx;
    ctx.repo = tmpdir + "/repo";
    ctx.kernel_dir = tmpdir + "/repo/kernel-src/linux-7.1.4";
    ctx.kver = "7.1.4";
    ctx.ssh_stage = tmpdir + "/repo/userspace/stage-ssh";
    ctx.alsa_stage = tmpdir + "/repo/userspace/stage-alsa-runtime";
    ctx.mplayer_stage = tmpdir + "/repo/userspace/stage-mplayer";
    ctx.sdl_stage = tmpdir + "/repo/userspace/stage-sdl-runtime";
    ctx.tcroot = tmpdir + "/repo/toolchain/sysroot";
    ctx.x11_payload = tmpdir + "/repo/matchbox-payload.tar";
    return ctx;
}

static void seed_minimal_repo(const DeployContext &ctx)
{
    write_fixture("repo/kernel-src/linux-7.1.4/arch/arm/boot/zImage", "zimage-bytes");
    write_fixture("repo/tools/kernel/kernel-modules.sh",
        "AUDIO_MODULES=\"\nsound/soundcore.ko\n\"\n"
        "WIFI_MODULES=\"\nkernel/drivers/pcmcia/pcmcia_core.ko\n\"\n"
        "SD_MODULES=\"\nkernel/fs/fat/fat.ko\n\"\n");
    write_fixture("repo/kernel-src/linux-7.1.4/sound/soundcore.ko", "x");
    write_fixture("repo/kernel-src/linux-7.1.4/drivers/pcmcia/pcmcia_core.ko", "x");
    write_fixture("repo/kernel-src/linux-7.1.4/fs/fat/fat.ko", "x");

    static const char *always_present[] = {
        "repo/rootfs/etc/init.d/rcS", "repo/rootfs/etc/init.d/xsession",
        "repo/rootfs/etc/inittab", "repo/rootfs/etc/modprobe.d/hostap.conf",
        "repo/rootfs/etc/wifi-up.sh", "repo/rootfs/usr/sbin/audioon",
        "repo/rootfs/usr/sbin/audinfo", "repo/rootfs/usr/sbin/bright",
        "repo/rootfs/usr/sbin/flip", "repo/rootfs/root/.matchbox/wallpaper",
        "repo/rootfs/usr/sbin/settime", "repo/rootfs/etc/zaurus-card.sh",
        "repo/rootfs/etc/profile", "repo/rootfs/etc/zshrc",
        "repo/rootfs/usr/sbin/sdapps", "repo/rootfs/usr/sbin/sdcard",
        "repo/rootfs/etc/mdev.conf", "repo/rootfs/usr/sbin/suspend",
        "repo/rootfs/usr/sbin/gototty", "repo/rootfs/usr/sbin/softreboot",
        0
    };
    for (int i = 0; always_present[i]; i++)
        write_fixture(always_present[i], "x");

    (void)ctx;
}

static const yaml::Section *find_section(const std::vector<yaml::Section> &sections,
                                          const std::string &name)
{
    for (size_t i = 0; i < sections.size(); i++)
        if (sections[i].name == name) return &sections[i];
    return 0;
}

static bool any_step_with_remote(const std::vector<Step> &steps, const std::string &remote)
{
    for (size_t i = 0; i < steps.size(); i++)
        if (steps[i].remote_path == remote) return true;
    return false;
}

static const Step *step_with_remote(const std::vector<Step> &steps, const std::string &remote)
{
    for (size_t i = 0; i < steps.size(); i++)
        if (steps[i].remote_path == remote) return &steps[i];
    return 0;
}

static void test_real_manifest_loads()
{
    printf("manifest.yaml: the real tracked file parses and has every expected section\n");

    std::string text;
    check(read_whole_file("../manifest.yaml", text), "manifest.yaml is readable from tests/");

    std::vector<yaml::Section> sections;
    std::string error;
    check(yaml::parse(text, sections, error), "the real manifest.yaml parses: " + error);

    static const char *expected[] = {
        "kernel", "sound_modules", "wifi_modules", "sd_modules", "core_scripts",
        "sd_card_overlay", "ssh_payload", "opkg", "panel_actions",
        "userspace_media", "x11_matchbox", 0
    };
    for (int i = 0; expected[i]; i++)
        check(find_section(sections, expected[i]) != 0,
              std::string("manifest.yaml has a \"") + expected[i] + "\" section");
}

static void test_kernel_only_stops_after_kernel()
{
    printf("build_plan: --kernel-only produces exactly the kernel section\n");

    DeployContext ctx = make_fixture_context();
    seed_minimal_repo(ctx);
    ctx.flags.kernel_only = true;

    std::string text;
    read_whole_file("../manifest.yaml", text);
    std::vector<yaml::Section> sections;
    std::string error;
    yaml::parse(text, sections, error);

    std::vector<std::string> which = select_sections(ctx.flags);
    check(which.size() == 1, "only the kernel section is selected");

    std::vector<Step> steps;
    check(build_plan(sections, which, ctx, steps, error), "plan builds: " + error);
    check(steps.size() == 1, "exactly one step");
    if (steps.size() == 1) {
        check(steps[0].type == STEP_PUT_FILE, "it's a put_file");
        check_str(steps[0].remote_path, "/boot/zImage-full", "kernel destination");
        check_str(steps[0].local_path, ctx.kernel_dir + "/arch/arm/boot/zImage", "kernel source");
    }
}

static void test_conditional_files_absent_by_default()
{
    printf("build_plan: if_exists-gated entries are skipped when the local file is absent\n");

    DeployContext ctx = make_fixture_context();
    seed_minimal_repo(ctx);

    std::string text;
    read_whole_file("../manifest.yaml", text);
    std::vector<yaml::Section> sections;
    std::string error;
    yaml::parse(text, sections, error);

    std::vector<Step> steps;
    check(build_plan(sections, select_sections(ctx.flags), ctx, steps, error), "plan builds: " + error);

    check(!any_step_with_remote(steps, "/usr/sbin/brightd"), "brightd absent -> not in plan");
    check(!any_step_with_remote(steps, "/usr/sbin/mhz"), "mhz absent -> not in plan");
    check(!any_step_with_remote(steps, "/usr/sbin/flipd"), "flipd absent -> not in plan");
    check(!any_step_with_remote(steps, "/usr/sbin/pkillx"), "pkillx absent -> not in plan");
    check(!any_step_with_remote(steps, "/usr/local/bin/kill"), "kill absent -> not in plan");
    check(!any_step_with_remote(steps, "/usr/sbin/dropbear"),
          "dropbear absent from plan without --replace-dropbear (if_flag gate)");

    check(!any_step_with_remote(steps, "/usr/bin/opkg"), "opkg binary absent -> not in plan");
    check(!any_step_with_remote(steps, "/etc/opkg/opkg.conf"), "opkg.conf absent -> not in plan");

    check(any_step_with_remote(steps, "/etc/init.d/rcS"), "rcS is in the plan");
    check(any_step_with_remote(steps, "/usr/sbin/audioon"), "audioon is in the plan");
}

static void test_conditional_files_present_when_built()
{
    printf("build_plan: if_exists-gated entries appear once the local artifact exists\n");

    DeployContext ctx = make_fixture_context();
    seed_minimal_repo(ctx);
    write_fixture("repo/userspace/src/brightd", "elf-bytes");
    write_fixture("repo/userspace/src/mhz", "elf-bytes");

    std::string text;
    read_whole_file("../manifest.yaml", text);
    std::vector<yaml::Section> sections;
    std::string error;
    yaml::parse(text, sections, error);

    std::vector<Step> steps;
    check(build_plan(sections, select_sections(ctx.flags), ctx, steps, error), "plan builds: " + error);

    check(any_step_with_remote(steps, "/usr/sbin/brightd"), "brightd present -> in plan");
    check(any_step_with_remote(steps, "/usr/sbin/mhz"), "mhz present -> in plan");
}

static void test_replace_dropbear_flag_gate()
{
    printf("build_plan: dropbear only appears with --replace-dropbear, and is always backed up\n");

    DeployContext ctx = make_fixture_context();
    seed_minimal_repo(ctx);
    write_fixture("repo/userspace/stage-ssh/usr/sbin/dropbear", "elf-bytes");
    ctx.flags.replace_dropbear = true;

    std::string text;
    read_whole_file("../manifest.yaml", text);
    std::vector<yaml::Section> sections;
    std::string error;
    yaml::parse(text, sections, error);

    std::vector<Step> steps;
    check(build_plan(sections, select_sections(ctx.flags), ctx, steps, error), "plan builds: " + error);

    const Step *db = step_with_remote(steps, "/usr/sbin/dropbear");
    check(db != 0, "dropbear appears with --replace-dropbear");
    if (db) {
        check(db->backup, "dropbear is backed up even without --create-backup-files");
        check(db->mode == 0755u, "dropbear mode");
    }
}

static void test_tz_policy_is_if_missing()
{
    printf("build_plan: /etc/TZ carries the if_missing policy, not always-overwrite\n");

    DeployContext ctx = make_fixture_context();
    seed_minimal_repo(ctx);
    write_fixture("repo/rootfs/etc/TZ", "UTC\n");

    std::string text;
    read_whole_file("../manifest.yaml", text);
    std::vector<yaml::Section> sections;
    std::string error;
    yaml::parse(text, sections, error);

    std::vector<Step> steps;
    check(build_plan(sections, select_sections(ctx.flags), ctx, steps, error), "plan builds: " + error);

    const Step *tz = step_with_remote(steps, "/etc/TZ");
    check(tz != 0, "TZ is in the plan (local file exists)");
    if (tz)
        check(tz->policy == PUT_IF_MISSING, "TZ policy is if_missing");
}

static void test_wifi_modules_keep_kernel_prefix_on_remote_only()
{
    printf("build_plan: WIFI_MODULES strip the kernel/ prefix locally but keep it remotely\n");

    DeployContext ctx = make_fixture_context();
    seed_minimal_repo(ctx);

    std::string text;
    read_whole_file("../manifest.yaml", text);
    std::vector<yaml::Section> sections;
    std::string error;
    yaml::parse(text, sections, error);

    std::vector<Step> steps;
    check(build_plan(sections, select_sections(ctx.flags), ctx, steps, error), "plan builds: " + error);

    std::string remote = "/lib/modules/7.1.4/kernel/drivers/pcmcia/pcmcia_core.ko";
    const Step *m = step_with_remote(steps, remote);
    check(m != 0, "wifi module lands at the modules-tree path including kernel/");
    if (m)
        check_str(m->local_path,
                  ctx.kernel_dir + "/drivers/pcmcia/pcmcia_core.ko",
                  "wifi module's local path has kernel/ stripped");
}

static void test_userspace_media_skipped_by_no_userspace()
{
    printf("build_plan: --no-userspace drops the userspace_media section entirely\n");

    DeployContext ctx = make_fixture_context();
    seed_minimal_repo(ctx);
    ctx.flags.no_userspace = true;

    std::vector<std::string> which = select_sections(ctx.flags);
    bool has_media = false;
    for (size_t i = 0; i < which.size(); i++)
        if (which[i] == "userspace_media") has_media = true;
    check(!has_media, "userspace_media not selected with --no-userspace");
}

static void test_x11_section_emits_extract_step()
{
    printf("build_plan: x11_matchbox emits a STEP_EXTRACT_TAR_TREE when the tar exists\n");

    DeployContext ctx = make_fixture_context();
    seed_minimal_repo(ctx);
    write_fixture("repo/matchbox-payload.tar", "not-a-real-tar-but-just-needs-to-exist");

    std::string text;
    read_whole_file("../manifest.yaml", text);
    std::vector<yaml::Section> sections;
    std::string error;
    yaml::parse(text, sections, error);

    std::vector<Step> steps;
    check(build_plan(sections, select_sections(ctx.flags), ctx, steps, error), "plan builds: " + error);

    bool found = false;
    for (size_t i = 0; i < steps.size(); i++) {
        if (steps[i].type == STEP_EXTRACT_TAR_TREE) {
            found = true;
            check_str(steps[i].local_tar_path, ctx.x11_payload, "extract step's tar path");
            check_str(steps[i].tar_remote_base, "/", "extract step's remote base");
        }
    }
    check(found, "STEP_EXTRACT_TAR_TREE present when the tar exists");
}

int main()
{
    char tmpl[] = "/tmp/manifest-test.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    tmpdir = tmpl;

    test_yaml_lite_roundtrip();
    test_shell_var_list();
    test_put_files_from_dir();
    test_put_files_from_tree_preserves_mode();

    test_real_manifest_loads();
    test_kernel_only_stops_after_kernel();
    test_conditional_files_absent_by_default();
    test_conditional_files_present_when_built();
    test_replace_dropbear_flag_gate();
    test_tz_policy_is_if_missing();
    test_wifi_modules_keep_kernel_prefix_on_remote_only();
    test_userspace_media_skipped_by_no_userspace();
    test_x11_section_emits_extract_step();

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir.c_str());
    if (system(cmd) != 0)
        printf("  (warning: could not remove %s)\n", tmpdir.c_str());

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    if (failures) {
        printf("MANIFEST-TEST: FAIL\n");
        return 1;
    }
    printf("MANIFEST-TEST: PASS\n");
    return 0;
}
