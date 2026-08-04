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
 *   2. indexes every *.ko entry under lib/modules/ directly out of the
 *      actual shipped /update.tar (name + byte offset + size -- no
 *      extraction to disk), then insmod's (init_module(2)) each one in a
 *      fixpoint retry loop -- this package's modules aren't shipped with
 *      full depmod metadata (build-update-package.sh copies specific
 *      .ko files by hand, see its own comment on this), so plain
 *      dependency-order-agnostic retries stand in for modprobe's
 *      dependency resolution. Reading straight out of /update.tar (rather
 *      than a separately loose-extracted copy under /lib/modules, which
 *      is how this used to work) means module data exists on this
 *      initramfs's tmpfs exactly once, not twice -- see
 *      flash/qemu-smoke-test.sh's own comment on why that doubling kept
 *      tipping this test over the guest's fixed 64M as the package grew.
 *   3. execs the actual shipped /usr/sbin/piko-update against that same
 *      /update.tar with --dry-run -- the same package this test run is
 *      itself validating. (piko-update's own --dry-run stages a verify
 *      copy of every file under /tmp as part of its normal safety design
 *      -- see piko-update.c's header -- so a THIRD copy of the package's
 *      content transiently exists at that point regardless; avoiding the
 *      loose-module copy here is what keeps that within budget.)
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
#define TAR_BLOCK_SIZE 512

/* Plain POSIX ustar header -- same layout as piko-update.c's own reader
 * (userspace/src/piko-update.c), just the subset this needs (no MANIFEST/
 * checksum handling: this only ever indexes the shipped /update.tar this
 * same smoke test just built, never an untrusted one). */
struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

struct module_entry {
    char path[MAX_PATH_LEN];
    off_t offset;
    long size;
};

static struct module_entry modules[MAX_MODULES];
static int module_count = 0;

static long parse_octal(const char *field, size_t len)
{
    long val = 0;
    size_t i = 0;

    while (i < len && field[i] == ' ')
        i++;
    for (; i < len && field[i]; i++) {
        if (field[i] < '0' || field[i] > '7')
            break;
        val = val * 8 + (field[i] - '0');
    }
    return val;
}

static int is_zero_block(const unsigned char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (b[i])
            return 0;
    return 1;
}

static void header_full_name(const struct tar_header *h, char *out, size_t outsz)
{
    char name[101];
    char prefix[156];

    memcpy(name, h->name, 100);
    name[100] = '\0';
    memcpy(prefix, h->prefix, 155);
    prefix[155] = '\0';

    if (prefix[0])
        snprintf(out, outsz, "%s/%s", prefix, name);
    else
        snprintf(out, outsz, "%s", name);
}

/* Walks the ustar archive at tar_path purely by header arithmetic (no
 * extraction) and records the name/offset/size of every lib/modules/...
 * entry ending in .ko. Tolerant of a missing/truncated archive -- this
 * runs before anything has confirmed the package is well-formed at all,
 * and a missing/empty module list is a legitimate, reportable outcome
 * (main() below already handles module_count == 0), not a crash. */
static void index_modules(const char *tar_path)
{
    int fd = open(tar_path, O_RDONLY);
    struct tar_header hdr;
    off_t pos = 0;

    if (fd < 0) {
        printf("index_modules: open(%s) failed: %s\n", tar_path, strerror(errno));
        return;
    }

    for (;;) {
        ssize_t n = pread(fd, &hdr, sizeof(hdr), pos);
        long size, nblocks;
        char full_name[MAX_PATH_LEN];
        size_t len;

        if (n != (ssize_t)sizeof(hdr) || is_zero_block((unsigned char *)&hdr, sizeof(hdr)))
            break;
        pos += TAR_BLOCK_SIZE;

        size = parse_octal(hdr.size, sizeof(hdr.size));
        nblocks = (size + TAR_BLOCK_SIZE - 1) / TAR_BLOCK_SIZE;
        header_full_name(&hdr, full_name, sizeof(full_name));

        if (hdr.typeflag == '0' || hdr.typeflag == '\0') {
            len = strlen(full_name);
            if (len > 3 && strcmp(full_name + len - 3, ".ko") == 0 &&
                module_count < MAX_MODULES) {
                snprintf(modules[module_count].path, MAX_PATH_LEN, "%s", full_name);
                modules[module_count].offset = pos;
                modules[module_count].size = size;
                module_count++;
            }
        }
        pos += (off_t)nblocks * TAR_BLOCK_SIZE;
    }

    close(fd);
}

/* verbose=1 prints exactly why this specific attempt failed (read/malloc
 * vs. the actual init_module() syscall) -- used only for the final,
 * still-unloaded modules after the retry loop converges, so a genuine
 * failure (real symbol/version mismatch, bad ELF) is distinguishable from
 * "just hadn't loaded its dependency yet". */
static int init_module_entry(int fd, const struct module_entry *m, int verbose)
{
    void *buf;
    ssize_t n;
    long rc;

    buf = malloc((size_t)m->size);
    if (!buf) {
        if (verbose)
            printf("  %s: malloc(%ld) failed\n", m->path, m->size);
        return -1;
    }
    n = pread(fd, buf, (size_t)m->size, m->offset);
    if (n != (ssize_t)m->size) {
        if (verbose)
            printf("  %s: read %zd of %ld bytes: %s\n", m->path, n, m->size,
                   strerror(errno));
        free(buf);
        return -1;
    }
    rc = syscall(SYS_init_module, buf, (unsigned long)m->size, "");
    if (verbose && rc != 0)
        printf("  %s: init_module failed: %s\n", m->path, strerror(errno));
    free(buf);
    return rc == 0 ? 0 : -1;
}

/* Loads every indexed module, retrying whatever hasn't loaded yet until a
 * full pass makes no further progress -- resolves load-order dependencies
 * without needing to know them upfront. Returns the number still unloaded
 * (0 == everything loaded). */
static int load_all_modules(const char *tar_path)
{
    int fd = open(tar_path, O_RDONLY);
    int loaded[MAX_MODULES] = {0};
    int remaining = module_count;
    int progress = 1;

    if (fd < 0) {
        printf("load_all_modules: open(%s) failed: %s\n", tar_path, strerror(errno));
        return module_count;
    }

    while (remaining > 0 && progress) {
        int i;
        progress = 0;
        for (i = 0; i < module_count; i++) {
            if (loaded[i])
                continue;
            if (init_module_entry(fd, &modules[i], 0) == 0) {
                printf("insmod OK   %s\n", modules[i].path);
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
                printf(" %s", modules[i].path);
        printf("\n");
        /* One verbose re-attempt per still-failed module so the log shows
         * *why*, not just *that*. */
        for (i = 0; i < module_count; i++)
            if (!loaded[i])
                init_module_entry(fd, &modules[i], 1);
    }
    close(fd);
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

/* Waits up to timeout_ms for path to appear (access() succeeding), polling
 * every 100ms. Used only for the SD card's device node below -- MMC card
 * detection runs its own enumeration after the bus comes up, so the node
 * isn't guaranteed to exist the instant devtmpfs is mounted. */
static int wait_for_path(const char *path, int timeout_ms)
{
    int waited = 0;

    while (access(path, F_OK) != 0) {
        if (waited >= timeout_ms)
            return -1;
        usleep(100000);
        waited += 100;
    }
    return 0;
}

/* Mounts the SD card flash/qemu-smoke-test.sh attaches at /tmp, so
 * piko-update's --dry-run (which stages a full verify copy of every
 * shipped file under /tmp before touching anything live -- see
 * piko-update.c's own safety-model comment) has real storage instead of
 * competing with the rest of this initramfs for the guest's fixed 64M of
 * RAM. CONFIG_MMC/CONFIG_MMC_BLOCK/CONFIG_MMC_PXA/CONFIG_VFAT_FS are all
 * built into the kernel (not modules), so the device node just needs
 * devtmpfs mounted and a short wait for card detection to finish -- no
 * insmod dependency here at all.
 *
 * Not fatal if this fails: piko-update's own staging attempt is what
 * actually surfaces the problem (ENOSPC if /tmp is still tmpfs-backed and
 * the package doesn't fit), so this only prints a clear diagnostic rather
 * than aborting -- a silent fallback here would turn a real regression
 * back into the same confusing "No space left on device" this whole setup
 * exists to avoid. */
static void mount_sdcard_at_tmp(void)
{
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    if (wait_for_path("/dev/mmcblk0", 15000) != 0) {
        printf("sdcard: /dev/mmcblk0 never appeared -- /tmp stays tmpfs-backed "
               "(piko-update's --dry-run staging may hit ENOSPC on a large package)\n");
        return;
    }
    if (mount("/dev/mmcblk0", "/tmp", "vfat", 0, NULL) != 0) {
        printf("sdcard: mount /dev/mmcblk0 at /tmp failed: %s -- /tmp stays "
               "tmpfs-backed (piko-update's --dry-run staging may hit ENOSPC "
               "on a large package)\n", strerror(errno));
        return;
    }
    printf("sdcard: /dev/mmcblk0 mounted at /tmp\n");
}

int main(void)
{
    int modules_failed;
    int piko_update_failed;
    int smf_failed;

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount_sdcard_at_tmp();

    printf("\n=== piko-update QEMU smoke test ===\n\n");

    index_modules("/update.tar");
    if (module_count > 0) {
        printf("-- loading %d kernel module(s) --\n", module_count);
        modules_failed = load_all_modules("/update.tar");
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
