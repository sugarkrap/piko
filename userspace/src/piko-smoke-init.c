/*
 * piko-smoke-init -- PID 1 for the QEMU smoke-test initramfs
 * (flash/qemu-smoke-test.sh). Not shipped to the device -- this only ever
 * runs inside a throwaway QEMU boot in CI.
 *
 * Deliberately not busybox-based: this project has no tracked static
 * busybox binary any more (the old initramfs/rootfs/ test environment was
 * dropped as legacy), and pulling one in fresh means either a network
 * dependency this sandbox can't exercise or vendoring another prebuilt
 * binary. Same reasoning as md5sum.c's own rationale ("small,
 * dependency-free, from-scratch") -- everything this needs (mount(2),
 * init_module(2), fork/exec) is a few direct syscalls, so it's its own
 * static ELF and IS /init directly (no shell, no shebang interpreter
 * needed at all).
 *
 * What it does, in order:
 *   1. mount proc/sysfs.
 *   2. insmod (init_module(2)) every *.ko under /lib/modules, in a
 *      fixpoint retry loop -- this package's modules aren't shipped with
 *      full depmod metadata (build-update-package.sh copies specific
 *      .ko files by hand, see its own comment on this), so plain
 *      dependency-order-agnostic retries stand in for modprobe's
 *      dependency resolution.
 *   3. execs the actual shipped /usr/sbin/piko-update against the actual
 *      shipped /update.tar with --dry-run -- the same package this test
 *      run is itself validating.
 *   4. Prints a single, greppable PASS/FAIL line to the console, then
 *      sleeps forever -- flash/qemu-smoke-test.sh's own host-side timeout
 *      is what ends the QEMU process; this never tries to power off the
 *      guest itself (spitz has no working power-management path for
 *      that here, and it doesn't need to -- the console line is already
 *      captured in the host's log by the time the timeout fires).
 *
 * Cross-compile:
 *   arm-linux-gnueabi-gcc -march=armv5te -O2 -static -o piko-smoke-init piko-smoke-init.c
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_MODULES 256
#define MAX_PATH_LEN 300

static char module_paths[MAX_MODULES][MAX_PATH_LEN];
static int module_count = 0;

static void find_modules(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    char child[MAX_PATH_LEN];
    struct stat st;

    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        if (stat(child, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            find_modules(child);
        } else if (module_count < MAX_MODULES) {
            size_t len = strlen(child);
            if (len > 3 && strcmp(child + len - 3, ".ko") == 0)
                snprintf(module_paths[module_count++], MAX_PATH_LEN, "%s", child);
        }
    }
    closedir(d);
}

/* verbose=1 prints exactly why this specific attempt failed (open/fstat/
 * malloc/read vs. the actual init_module() syscall) -- used only for the
 * final, still-unloaded modules after the retry loop converges, so a
 * genuine failure (missing file, bad ELF, real symbol/version mismatch)
 * is distinguishable from "just hadn't loaded its dependency yet". Without
 * this, every failure mode collapses into the same silent -1, and a file
 * that's simply missing from the initramfs looks identical in the log to
 * one that loaded fine on a later pass. */
static int init_module_file(const char *path, int verbose)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    void *buf;
    ssize_t n;
    long rc;

    if (fd < 0) {
        if (verbose)
            printf("  %s: open failed: %s\n", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) < 0) {
        if (verbose)
            printf("  %s: fstat failed: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    buf = malloc((size_t)st.st_size);
    if (!buf) {
        if (verbose)
            printf("  %s: malloc(%lld) failed\n", path, (long long)st.st_size);
        close(fd);
        return -1;
    }
    n = read(fd, buf, (size_t)st.st_size);
    close(fd);
    if (n != st.st_size) {
        if (verbose)
            printf("  %s: read %zd of %lld bytes: %s\n", path, n,
                   (long long)st.st_size, strerror(errno));
        free(buf);
        return -1;
    }
    rc = syscall(SYS_init_module, buf, (unsigned long)st.st_size, "");
    if (verbose && rc != 0)
        printf("  %s: init_module failed: %s\n", path, strerror(errno));
    free(buf);
    return rc == 0 ? 0 : -1;
}

/* Loads every discovered module, retrying whatever hasn't loaded yet
 * until a full pass makes no further progress -- resolves load-order
 * dependencies without needing to know them upfront. Returns the number
 * still unloaded (0 == everything loaded). */
static int load_all_modules(void)
{
    int loaded[MAX_MODULES] = {0};
    int remaining = module_count;
    int progress = 1;

    while (remaining > 0 && progress) {
        int i;
        progress = 0;
        for (i = 0; i < module_count; i++) {
            if (loaded[i])
                continue;
            if (init_module_file(module_paths[i], 0) == 0) {
                printf("insmod OK   %s\n", module_paths[i]);
                loaded[i] = 1;
                remaining--;
                progress = 1;
            }
        }
    }
    if (remaining > 0) {
        int i;
        printf("insmod FAILED for:");
        for (i = 0; i < module_count; i++)
            if (!loaded[i])
                printf(" %s", module_paths[i]);
        printf("\n");
        /* One verbose re-attempt per still-failed module so the log shows
         * *why*, not just *that*. */
        for (i = 0; i < module_count; i++)
            if (!loaded[i])
                init_module_file(module_paths[i], 1);
    }
    return remaining;
}

static int run_piko_update_dry_run(void)
{
    pid_t pid;
    int status;

    if (access("/usr/sbin/piko-update", X_OK) != 0) {
        printf("piko-update FAILED: not present/executable at /usr/sbin/piko-update: %s\n",
               strerror(errno));
        return -1;
    }

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *argv[] = {"/usr/sbin/piko-update", "/update.tar", "--dry-run", NULL};
        execv(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("piko-update --dry-run FAILED\n");
        return -1;
    }
    printf("piko-update --dry-run OK\n");
    return 0;
}

/* Checks the smf/bootstrap half of piko-update without any NAND to write.
 *
 * QEMU's spitz machine has no Sharp-FTL smf partition, so this cannot
 * exercise a real flash -- and that is the point. What it does pin down is
 * that the *safe* paths stay safe: the writer ships, a dry run does not
 * stage a bootstrap write, and asking to commit when nothing is pending is
 * a clean no-op rather than an error or (much worse) an attempted write.
 * Those are exactly the behaviours a careless refactor would break
 * silently, since nobody exercises them on real hardware until the one day
 * it matters. */
static int run_smf_checks(void)
{
    pid_t pid;
    int status;
    int failed = 0;

    if (access("/usr/sbin/piko-smf-write", X_OK) != 0) {
        printf("smf FAILED: /usr/sbin/piko-smf-write not shipped in the package: %s\n",
               strerror(errno));
        failed = 1;
    } else {
        printf("smf OK: piko-smf-write present in package\n");
    }

    /* The dry run above must not have staged a bootstrap write. */
    if (access("/boot/smf-pending", F_OK) == 0) {
        printf("smf FAILED: --dry-run created /boot/smf-pending\n");
        failed = 1;
    } else {
        printf("smf OK: --dry-run left no pending-bootstrap marker\n");
    }

    /* --commit-smf with nothing pending must succeed and do nothing. */
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *argv[] = {"/usr/sbin/piko-update", "--commit-smf", NULL};
        execv(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("smf FAILED: --commit-smf with nothing pending did not exit 0\n");
        failed = 1;
    } else {
        printf("smf OK: --commit-smf with nothing pending is a clean no-op\n");
    }

    return failed ? -1 : 0;
}

int main(void)
{
    int modules_failed;
    int piko_update_failed;
    int smf_failed;

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);

    printf("\n=== piko-update QEMU smoke test ===\n\n");

    find_modules("/lib/modules");
    if (module_count > 0) {
        printf("-- loading %d kernel module(s) --\n", module_count);
        modules_failed = load_all_modules();
    } else {
        printf("-- no kernel modules in this package, skipping --\n");
        modules_failed = 0;
    }

    printf("\n-- exercising piko-update against the actual shipped package --\n");
    piko_update_failed = run_piko_update_dry_run();

    printf("\n-- checking the smf/bootstrap paths stay inert --\n");
    smf_failed = run_smf_checks();

    printf("\n");
    if (modules_failed == 0 && piko_update_failed == 0 && smf_failed == 0) {
        printf("PIKO-SMOKE-TEST: PASS\n");
    } else {
        printf("PIKO-SMOKE-TEST: FAIL:");
        if (modules_failed)
            printf(" module load failure");
        if (piko_update_failed)
            printf(" piko-update dry-run");
        if (smf_failed)
            printf(" smf checks");
        printf("\n");
    }
    fflush(stdout);

    for (;;)
        sleep(3600);
    return 0;
}
