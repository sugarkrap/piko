/*
 * piko-update -- apply a piko-update package (kernel + modules + rootfs
 * overlay) to the running "home" partition (mtd3), entirely offline: no
 * WiFi, no SSH, no second computer. Point it at a package staged on the SD
 * card and it does what flash/chunked-deploy.sh does over SSH, but locally:
 *
 *   piko-update /mnt/card/update.tar [--dry-run] [--no-reboot]
 *
 * home is a normal writable JFFS2 filesystem while the device is running
 * (see docs/HOWTO-BUILD-DEPLOY-KERNEL.md) -- this tool never touches the
 * smf/mtd1 bootstrap partition or does any raw NAND I/O; that stays
 * flash/picoupdate.sh's job.
 *
 * On this device we can never assume the running ROM has tar/unzip/gzip/
 * md5sum available as external commands -- that's exactly why the
 * chunked-deploy.sh comments mention this rootfs's busybox has no md5sum
 * built in. So this binary is fully self-contained: it reads plain
 * (uncompressed) POSIX ustar directly with its own minimal reader, and
 * hashes with the from-scratch MD5 in md5.h. No forked-out tools at all.
 *
 * Package format: a plain ustar archive whose first entry is a file named
 * "MANIFEST" -- a text file, one line per shipped file:
 *
 *   PIKO-UPDATE-PACKAGE 1              (required first line, exact match)
 *   # free-text lines starting with '#' are printed and otherwise ignored
 *   <32-hex-char md5>  <path relative to />
 *   ...
 *
 * Every non-MANIFEST, non-directory entry in the archive must have a
 * matching MANIFEST line, and vice versa -- a file missing from either
 * side fails the whole update before anything is touched.
 *
 * Safety model (this is the last spare board, no serial/USB recovery --
 * see AGENTS.md):
 *   1. Every file is streamed into a staging tree under STAGING_DIR and
 *      its content MD5 checked against MANIFEST *before* anything under
 *      "/" is touched. Any mismatch, truncation, or manifest/archive
 *      disagreement aborts cleanly with nothing applied.
 *   2. STAGING_DIR lives on this same root filesystem (there is no
 *      tmpfs /tmp on this rootfs), so once staged content is verified,
 *      installing it is a same-filesystem rename() into place --
 *      metadata-only, no risk of a partial write landing on a live path.
 *   3. /boot/zImage-full specifically gets backed up to .bak first (a
 *      rename, not a copy) if the package replaces it, matching the
 *      recovery procedure already documented in
 *      docs/HOWTO-BUILD-DEPLOY-KERNEL.md ("cp zImage-full.bak
 *      zImage-full; reboot"). No other file gets this treatment -- it's
 *      the one path with an actual documented recovery story.
 *   4. A flock on LOCK_PATH refuses a second concurrent run, the same
 *      class of bug that corrupted JFFS2 once already (see
 *      flash/chunked-deploy.sh's own lock comment).
 *   5. Reboots via /usr/sbin/softreboot (kexec self-jump), never a raw
 *      reboot() -- this hardware's normal restart path is indistinguishable
 *      from a hard poweroff (see softreboot's own comments).
 *
 * Cross-compile (same toolchain as the rest of userspace/src):
 *   GCC=.../arm-buildroot-linux-uclibcgnueabi-gcc
 *   $GCC -march=armv5te -O2 -static -o piko-update piko-update.c
 *   arm-buildroot-linux-uclibcgnueabi-strip piko-update
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "md5.h"

#define BLOCK_SIZE      512
#define MAX_ENTRIES     512
#define MAX_PATH        300
#define STAGING_DIR     "/tmp/piko-update.staging"
#define LOCK_PATH       "/tmp/piko-update.lock"
#define SOFTREBOOT      "/usr/sbin/softreboot"
#define ZIMAGE_PATH     "boot/zImage-full"

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

struct manifest_entry {
    char path[MAX_PATH];
    char md5hex[33];
    int seen;
};

static struct manifest_entry manifest[MAX_ENTRIES];
static int manifest_count = 0;
static int dry_run = 0;
static int no_reboot = 0;

static void rmtree(const char *path);

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "piko-update: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    /* Never leave a half-populated staging tree behind on abort -- it's
     * not on a live path, but this is still flash on the last spare
     * board, not RAM (see STAGING_DIR's comment), so don't litter it. */
    rmtree(STAGING_DIR);
    exit(1);
}

static int read_full(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return got == n ? 0 : -1;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    size_t done = 0;

    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

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

static int header_checksum_ok(const struct tar_header *h)
{
    const unsigned char *raw = (const unsigned char *)h;
    long stored = parse_octal(h->chksum, sizeof(h->chksum));
    long sum = 0;
    size_t chk_off = offsetof(struct tar_header, chksum);
    size_t i;

    for (i = 0; i < sizeof(*h); i++)
        sum += (i >= chk_off && i < chk_off + sizeof(h->chksum)) ? ' ' : raw[i];
    return sum == stored;
}

static int is_zero_block(const unsigned char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (b[i])
            return 0;
    return 1;
}

/* Rejects absolute paths and any ".." path component. This archive is
 * built by our own packaging script, but a hand-edited or corrupt one
 * should never be able to write outside the staging root or the live
 * tree it gets installed into. */
static int path_is_safe(const char *name)
{
    const char *p = name;

    if (name[0] == '\0' || name[0] == '/')
        return 0;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0') &&
            (p == name || p[-1] == '/'))
            return 0;
        p++;
    }
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

static void path_dirname(const char *path, char *out, size_t outsz)
{
    const char *slash = strrchr(path, '/');

    if (!slash) {
        snprintf(out, outsz, ".");
        return;
    }
    snprintf(out, outsz, "%.*s", (int)(slash - path), path);
}

static int mkdir_p(const char *path)
{
    char tmp[MAX_PATH + 32];
    char *p;

    if (path[0] == '\0' || strcmp(path, ".") == 0)
        return 0;

    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static void rmtree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *de;
    char child[MAX_PATH + 32];
    struct stat st;

    if (!d) {
        unlink(path);
        return;
    }
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
            rmtree(child);
        else
            unlink(child);
    }
    closedir(d);
    rmdir(path);
}

static int is_hex32(const char *s)
{
    int i;

    for (i = 0; i < 32; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

static struct manifest_entry *find_manifest(const char *path)
{
    int i;

    for (i = 0; i < manifest_count; i++)
        if (strcmp(manifest[i].path, path) == 0)
            return &manifest[i];
    return NULL;
}

static int parse_manifest(char *buf)
{
    char *line, *saveptr = NULL;
    int first = 1;

    for (line = strtok_r(buf, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        if (*line == '\0')
            continue;

        if (first) {
            first = 0;
            if (strcmp(line, "PIKO-UPDATE-PACKAGE 1") != 0) {
                fprintf(stderr, "piko-update: unrecognized package format: %s\n", line);
                return -1;
            }
            continue;
        }

        if (line[0] == '#') {
            printf("piko-update: %s\n", line + 1);
            continue;
        }

        if (strlen(line) < 34 || line[32] != ' ' || !is_hex32(line)) {
            fprintf(stderr, "piko-update: malformed MANIFEST line: %s\n", line);
            return -1;
        }
        if (manifest_count >= MAX_ENTRIES) {
            fprintf(stderr, "piko-update: too many files in archive (max %d)\n", MAX_ENTRIES);
            return -1;
        }
        if (!path_is_safe(line + 33)) {
            fprintf(stderr, "piko-update: unsafe path in MANIFEST: %s\n", line + 33);
            return -1;
        }

        {
            struct manifest_entry *e = &manifest[manifest_count++];
            memcpy(e->md5hex, line, 32);
            e->md5hex[32] = '\0';
            snprintf(e->path, sizeof(e->path), "%s", line + 33);
            e->seen = 0;
        }
    }

    if (manifest_count == 0) {
        fprintf(stderr, "piko-update: MANIFEST has no file entries\n");
        return -1;
    }
    return 0;
}

/* Reads exactly one header block + its data, requires it to be a regular
 * file named "MANIFEST", and parses its content into manifest[]. */
static void read_and_parse_manifest(int fd)
{
    struct tar_header hdr;
    char name[101];
    long size;
    long nblocks, b;
    char *buf;
    size_t off = 0;

    if (read_full(fd, &hdr, sizeof(hdr)) < 0)
        die("archive is empty or truncated (no MANIFEST entry)");
    if (!header_checksum_ok(&hdr))
        die("corrupt archive: bad header checksum on first entry");

    memcpy(name, hdr.name, 100);
    name[100] = '\0';
    if (strcmp(name, "MANIFEST") != 0)
        die("archive must start with a MANIFEST entry (found \"%s\")", name);

    size = parse_octal(hdr.size, sizeof(hdr.size));
    if (size <= 0 || size > 1024L * 1024L)
        die("MANIFEST entry has an unreasonable size (%ld bytes)", size);

    buf = malloc((size_t)size + 1);
    if (!buf)
        die("out of memory reading MANIFEST (%ld bytes)", size);

    nblocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (b = 0; b < nblocks; b++) {
        unsigned char block[BLOCK_SIZE];
        size_t used;

        if (read_full(fd, block, BLOCK_SIZE) < 0) {
            free(buf);
            die("archive truncated while reading MANIFEST");
        }
        used = (size_t)size - off;
        if (used > BLOCK_SIZE)
            used = BLOCK_SIZE;
        memcpy(buf + off, block, used);
        off += used;
    }
    buf[size] = '\0';

    if (parse_manifest(buf) < 0) {
        free(buf);
        exit(1);
    }
    free(buf);

    printf("piko-update: MANIFEST OK, %d file(s) to verify\n", manifest_count);
}

/* Streams one regular-file entry's data (nblocks * BLOCK_SIZE bytes on
 * the wire, only the first `size` of which are real content) into a
 * fresh file at stage_path, hashing as it goes. */
static void stage_file_entry(int fd, const char *stage_path, long size, mode_t mode,
                              const struct manifest_entry *want)
{
    long nblocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    long b;
    long written = 0;
    md5_ctx ctx;
    char hex[33];
    int out;
    char dir[MAX_PATH + 16];

    path_dirname(stage_path, dir, sizeof(dir));
    if (mkdir_p(dir) < 0)
        die("could not create staging directory %s: %s", dir, strerror(errno));

    out = open(stage_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
        die("could not create staging file %s: %s", stage_path, strerror(errno));

    md5_init(&ctx);
    for (b = 0; b < nblocks; b++) {
        unsigned char block[BLOCK_SIZE];
        long used;

        if (read_full(fd, block, BLOCK_SIZE) < 0)
            die("archive truncated while reading %s", want->path);

        used = size - written;
        if (used > BLOCK_SIZE)
            used = BLOCK_SIZE;
        if (used > 0) {
            if (write_full(out, block, (size_t)used) < 0)
                die("write failed staging %s: %s", want->path, strerror(errno));
            md5_update(&ctx, block, (size_t)used);
            written += used;
        }
    }

    if (fchmod(out, mode) < 0)
        die("chmod failed staging %s: %s", want->path, strerror(errno));
    close(out);

    {
        unsigned char digest[16];
        md5_final(&ctx, digest);
        md5_to_hex(digest, hex);
    }

    if (strcmp(hex, want->md5hex) != 0)
        die("checksum mismatch for %s: archive says %s, expected %s (corrupt package?)",
            want->path, hex, want->md5hex);

    printf("piko-update: verify OK  %-40s (%ld bytes)\n", want->path, size);
}

static void verify_archive(const char *archive_path)
{
    int fd = open(archive_path, O_RDONLY);
    struct tar_header hdr;

    if (fd < 0)
        die("cannot open %s: %s", archive_path, strerror(errno));

    read_and_parse_manifest(fd);

    for (;;) {
        char full_name[MAX_PATH];
        long size, nblocks, b;
        char typeflag;

        if (read_full(fd, &hdr, sizeof(hdr)) < 0)
            die("archive truncated (missing end-of-archive marker)");

        if (is_zero_block((unsigned char *)&hdr, sizeof(hdr)))
            break; /* normal end of archive */

        if (!header_checksum_ok(&hdr))
            die("corrupt archive: bad header checksum");

        header_full_name(&hdr, full_name, sizeof(full_name));
        if (!path_is_safe(full_name))
            die("unsafe path in archive: %s", full_name);

        size = parse_octal(hdr.size, sizeof(hdr.size));
        nblocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        typeflag = hdr.typeflag;

        if (typeflag == '5') { /* directory */
            char stage_path[MAX_PATH + 32];

            snprintf(stage_path, sizeof(stage_path), "%s/%s", STAGING_DIR, full_name);
            mkdir_p(stage_path);
            for (b = 0; b < nblocks; b++) {
                unsigned char block[BLOCK_SIZE];
                if (read_full(fd, block, BLOCK_SIZE) < 0)
                    die("archive truncated after directory entry %s", full_name);
            }
            continue;
        }

        if (typeflag == '0' || typeflag == '\0') { /* regular file */
            struct manifest_entry *want = find_manifest(full_name);
            char stage_path[MAX_PATH + 32];
            mode_t mode;

            if (!want)
                die("archive contains %s, which is not listed in MANIFEST", full_name);
            if (want->seen)
                die("archive contains %s twice", full_name);

            mode = (mode_t)(parse_octal(hdr.mode, sizeof(hdr.mode)) & 07777);
            snprintf(stage_path, sizeof(stage_path), "%s/%s", STAGING_DIR, full_name);
            stage_file_entry(fd, stage_path, size, mode, want);
            want->seen = 1;
            continue;
        }

        die("unsupported entry type '%c' for %s (only files and directories are supported)",
            typeflag, full_name);
    }

    close(fd);

    {
        int i, missing = 0;
        for (i = 0; i < manifest_count; i++) {
            if (!manifest[i].seen) {
                fprintf(stderr, "piko-update: MANIFEST promises %s but archive never delivered it\n",
                        manifest[i].path);
                missing++;
            }
        }
        if (missing)
            die("archive is incomplete (%d file(s) missing)", missing);
    }

    printf("piko-update: all %d file(s) verified OK\n", manifest_count);
}

static void install_file(const char *rel_path)
{
    char staged[MAX_PATH + 32];
    char dest[MAX_PATH + 8];
    char dest_dir[MAX_PATH + 16];

    snprintf(staged, sizeof(staged), "%s/%s", STAGING_DIR, rel_path);
    snprintf(dest, sizeof(dest), "/%s", rel_path);
    path_dirname(dest, dest_dir, sizeof(dest_dir));

    if (mkdir_p(dest_dir) < 0)
        die("could not create %s: %s", dest_dir, strerror(errno));

    /* The one file with a documented recovery story (see
     * docs/HOWTO-BUILD-DEPLOY-KERNEL.md): back it up before replacing so
     * a panicking new kernel can be undone by hand at the console. */
    if (strcmp(rel_path, ZIMAGE_PATH) == 0 && access(dest, F_OK) == 0) {
        char bak[MAX_PATH + 16];
        snprintf(bak, sizeof(bak), "%s.bak", dest);
        if (rename(dest, bak) < 0)
            die("could not back up %s to %s: %s", dest, bak, strerror(errno));
    }

    if (rename(staged, dest) < 0) {
        if (errno != EXDEV)
            die("could not install %s: %s", dest, strerror(errno));

        /* Staging and the destination ended up on different filesystems
         * (not expected on this rootfs today, but don't brick over it) --
         * fall back to a plain copy. */
        int in = open(staged, O_RDONLY);
        int out;
        struct stat st;
        char buf[BLOCK_SIZE * 4];
        ssize_t n;

        if (in < 0 || fstat(in, &st) < 0)
            die("could not read staged %s: %s", staged, strerror(errno));
        out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
        if (out < 0)
            die("could not create %s: %s", dest, strerror(errno));
        while ((n = read(in, buf, sizeof(buf))) > 0) {
            if (write_full(out, buf, (size_t)n) < 0)
                die("write failed installing %s: %s", dest, strerror(errno));
        }
        close(in);
        close(out);
        unlink(staged);
    }

    printf("piko-update: install %s\n", dest);
}

static void apply_update(void)
{
    int i;

    for (i = 0; i < manifest_count; i++)
        install_file(manifest[i].path);

    sync();
    printf("piko-update: %d file(s) installed.\n", manifest_count);
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s <package.tar> [--dry-run] [--no-reboot]\n", prog);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *archive_path = NULL;
    int i;
    int lockfd;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = 1;
        else if (strcmp(argv[i], "--no-reboot") == 0)
            no_reboot = 1;
        else if (!archive_path)
            archive_path = argv[i];
        else
            usage(argv[0]);
    }
    if (!archive_path)
        usage(argv[0]);

    lockfd = open(LOCK_PATH, O_CREAT | O_RDWR, 0644);
    if (lockfd < 0)
        die("could not open lock file %s: %s", LOCK_PATH, strerror(errno));
    if (flock(lockfd, LOCK_EX | LOCK_NB) < 0)
        die("another piko-update is already running (lock held on %s)", LOCK_PATH);

    rmtree(STAGING_DIR);
    if (mkdir_p(STAGING_DIR) < 0)
        die("could not create staging directory %s: %s", STAGING_DIR, strerror(errno));

    verify_archive(archive_path);

    if (dry_run) {
        rmtree(STAGING_DIR);
        printf("piko-update: dry run OK, nothing was changed.\n");
        return 0;
    }

    apply_update();
    rmtree(STAGING_DIR);

    if (no_reboot) {
        printf("piko-update: done. Run softreboot manually when ready.\n");
        return 0;
    }

    printf("piko-update: rebooting via %s ...\n", SOFTREBOOT);
    fflush(stdout);
    execl(SOFTREBOOT, "softreboot", (char *)NULL);
    die("update applied but could not exec %s: %s (reboot manually)",
        SOFTREBOOT, strerror(errno));
    return 1;
}
