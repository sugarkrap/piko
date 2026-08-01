/*
 * piko-update -- apply a piko-update package to this device, offline: no
 * WiFi, no SSH, no second computer. Point it at a package staged on the SD
 * card and it does what tools/chunked-deploy.sh does over SSH, but locally:
 *
 *   piko-update /mnt/card/update.tar [--dry-run] [--no-reboot]
 *   piko-update --commit-smf [--force]
 *
 * This is the single updater for the whole ROM. It covers both partitions,
 * but they are NOT equally dangerous and the mechanism keeps them apart:
 *
 *   home (mtd3)  -- a normal writable JFFS2 filesystem while the device is
 *                   running (docs/HOWTO-BUILD-DEPLOY-KERNEL.md). Userspace,
 *                   /etc, and the stage-2 kernel all live here as ordinary
 *                   files. Replacing them is a rename(). Reversible, no raw
 *                   NAND I/O, no way to lose the board.
 *
 *   smf (mtd1)   -- the raw NAND bootstrap partition holding the tiny
 *                   kexec loader kernel. Nothing executes from it at
 *                   runtime (it kexec'd into RAM at boot), so it *can* be
 *                   written live -- but a torn write leaves a board that
 *                   does not boot, and there is no serial and no USB to
 *                   recover through. The only floor is a human with an SD
 *                   card and the Sharp maintenance menu
 *                   (docs/FLASH-MTD1-MTD3-SAFE.md).
 *
 * So an smf image in a package is never written during the same run that
 * installs home. Instead:
 *
 *   1. The image rides in the package as an ordinary file, boot/zImage-smf,
 *      staged and MD5-verified by exactly the same code as everything else.
 *   2. After home is installed, its content is compared against what mtd1
 *      actually holds (piko-smf-write --compare, read-only). Identical is
 *      the common case -- the bootstrap changes maybe twice a year -- and
 *      then nothing happens at all. This is what makes shipping the
 *      bootstrap in every package free rather than a recurring brick risk.
 *   3. Only if it differs is /boot/smf-pending written, and the device
 *      reboots into the newly installed stage-2 *first*.
 *   4. The NAND write happens later, by hand, via `smfcommit` (which runs
 *      piko-update --commit-smf) -- once the new kernel and rootfs have
 *      demonstrably booted. That reboot is the checkpoint: it means only
 *      one half of the boot chain is ever in question at a time.
 *
 * Note this is the reverse of docs/FLASH-MTD1-MTD3-SAFE.md's SD-card
 * order, which does mtd1 before mtd3. That procedure is reviving a board
 * that is already down; this one is running on a board that currently
 * boots, and a working mtd1 is the only thing keeping it recoverable.
 *
 * On this device we can never assume the running ROM has tar/unzip/gzip/
 * md5sum available as external commands -- that's exactly why the
 * chunked-deploy.sh comments mention this rootfs's busybox has no md5sum
 * built in. So this binary is self-contained for archive handling: it
 * reads plain (uncompressed) POSIX ustar with its own minimal reader and
 * hashes with the from-scratch MD5 in md5.h.
 *
 * The one thing it does exec is /usr/sbin/piko-smf-write, for NAND I/O
 * only. That is not a walk-back of the no-external-tools rule -- the rule
 * is about busybox applets that may not exist on an unknown ROM.
 * piko-smf-write ships in this same package, at a known path, and its FTL
 * mapping code is the byte-verified path for this partition. Duplicating
 * that logic here to avoid a fork() would create two implementations of
 * the one routine that can brick the board, free to drift apart.
 *
 * Package format: a plain ustar archive whose first entry is a file named
 * "MANIFEST" -- a text file, one line per shipped file:
 *
 *   PIKO-UPDATE-PACKAGE 1              (required first line, exact match)
 *   # free-text lines starting with '#' are printed and otherwise ignored
 *   <32-hex-char md5>  <path relative to />
 *   SYMLINK <path relative to /> -> <target>
 *   ...
 *
 * Every non-MANIFEST, non-directory entry in the archive must have a
 * matching MANIFEST line, and vice versa -- a file missing from either
 * side fails the whole update before anything is touched. A SYMLINK line
 * has no content to hash (the tar entry carries no data blocks); its
 * archive typeflag '2' entry is matched by path, and the archive's own
 * linkname is cross-checked against the MANIFEST-recorded target before
 * the symlink is staged, same "trust nothing, verify everything" spirit
 * as the md5 check on regular files. Added for the X11/Matchbox desktop
 * payload, which relies on symlinks for shared-library SONAMEs (e.g.
 * libX11.so.6 -> libX11.so.6.3.0) -- until then this format shipped no
 * symlinks at all.
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
 *      tools/chunked-deploy.sh's own lock comment).
 *   5. Reboots via /usr/sbin/softreboot (kexec self-jump), never a raw
 *      reboot() -- this hardware's normal restart path is indistinguishable
 *      from a hard poweroff (see softreboot's own comments).
 *   6. An smf write is gated on a *verified* full-partition backup and on
 *      mains power, and is never automatic: it takes a deliberate second
 *      command after a successful boot.
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
#include <sys/wait.h>
#include <unistd.h>

#include "md5.h"

#define BLOCK_SIZE      512
/* Raised from 512 when the X11/Matchbox desktop payload (~450 regular
 * files + symlinks) started shipping through this format -- the old cap
 * left almost no headroom once the existing kernel-modules + rootfs
 * overlay entries were added in too. */
#define MAX_ENTRIES     1024
#define MAX_PATH        300
#define STAGING_DIR     "/tmp/piko-update.staging"
#define LOCK_PATH       "/tmp/piko-update.lock"
#define SOFTREBOOT      "/usr/sbin/softreboot"
#define ZIMAGE_PATH     "boot/zImage-full"

/* smf / mtd1 bootstrap partition.
 *
 * SMF_LADDR and SMF_MAX are the *logical* NAND geometry of the kernel slot
 * and are not ours to choose -- the Sharp bootloader reads a fixed logical
 * address. Both numbers are load-bearing and documented in AGENTS.md and
 * docs/DEADLETTER-MTD1-OFFSET.md. Do not "clean them up".
 *
 * The partition is found by NAME, never by number: this rootfs's mainline
 * kernel numbers it mtd0 while the Cacko/recovery kernel the SD-card
 * installer runs under calls the same partition mtd1 (AGENTS.md). A
 * hardcoded mtdN here would write the wrong partition under one of them. */
#define SMF_LADDR       917504
#define SMF_MAX         1294336
#define SMF_IMAGE_PATH  "/boot/zImage-smf"
#define SMF_PENDING     "/boot/smf-pending"
#define SMF_WRITER      "/usr/sbin/piko-smf-write"
#define SMF_PENDING_MAGIC "PIKO-SMF-PENDING 1"

/* piko-smf-write --compare exit codes; anything else means "undetermined",
 * which must never be treated as "already up to date". */
#define SMF_CMP_SAME    0
#define SMF_CMP_DIFFERS 3

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
    int is_symlink;
    char target[MAX_PATH];   /* only meaningful when is_symlink */
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

        if (strncmp(line, "SYMLINK ", 8) == 0) {
            char *path = line + 8;
            char *arrow = strstr(path, " -> ");

            if (!arrow || arrow == path) {
                fprintf(stderr, "piko-update: malformed SYMLINK line: %s\n", line);
                return -1;
            }
            *arrow = '\0';
            {
                const char *target = arrow + 4;

                if (*target == '\0') {
                    fprintf(stderr, "piko-update: malformed SYMLINK line: %s\n", line);
                    return -1;
                }
                if (manifest_count >= MAX_ENTRIES) {
                    fprintf(stderr, "piko-update: too many files in archive (max %d)\n", MAX_ENTRIES);
                    return -1;
                }
                if (!path_is_safe(path)) {
                    fprintf(stderr, "piko-update: unsafe path in MANIFEST: %s\n", path);
                    return -1;
                }

                {
                    struct manifest_entry *e = &manifest[manifest_count++];
                    e->md5hex[0] = '\0';
                    e->is_symlink = 1;
                    snprintf(e->path, sizeof(e->path), "%s", path);
                    snprintf(e->target, sizeof(e->target), "%s", target);
                    e->seen = 0;
                }
            }
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
            e->is_symlink = 0;
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
            if (want->is_symlink)
                die("archive has %s as a regular file, but MANIFEST lists it as a symlink", full_name);
            if (want->seen)
                die("archive contains %s twice", full_name);

            mode = (mode_t)(parse_octal(hdr.mode, sizeof(hdr.mode)) & 07777);
            snprintf(stage_path, sizeof(stage_path), "%s/%s", STAGING_DIR, full_name);
            stage_file_entry(fd, stage_path, size, mode, want);
            want->seen = 1;
            continue;
        }

        if (typeflag == '2') { /* symlink */
            struct manifest_entry *want = find_manifest(full_name);
            char stage_path[MAX_PATH + 32];
            char stage_dir[MAX_PATH + 16];
            char linkname[101];

            if (!want)
                die("archive contains %s, which is not listed in MANIFEST", full_name);
            if (!want->is_symlink)
                die("archive has %s as a symlink, but MANIFEST lists it as a regular file", full_name);
            if (want->seen)
                die("archive contains %s twice", full_name);

            memcpy(linkname, hdr.linkname, 100);
            linkname[100] = '\0';
            if (strcmp(linkname, want->target) != 0)
                die("symlink target mismatch for %s: archive says -> %s, MANIFEST says -> %s",
                    full_name, linkname, want->target);

            snprintf(stage_path, sizeof(stage_path), "%s/%s", STAGING_DIR, full_name);
            path_dirname(stage_path, stage_dir, sizeof(stage_dir));
            if (mkdir_p(stage_dir) < 0)
                die("could not create staging directory %s: %s", stage_dir, strerror(errno));
            unlink(stage_path); /* symlink() fails if stage_path already exists */
            if (symlink(want->target, stage_path) < 0)
                die("could not create symlink %s -> %s: %s", stage_path, want->target, strerror(errno));

            /* ustar symlinks carry no data blocks (size 0), but consume
             * whatever is there defensively rather than assume it. */
            for (b = 0; b < nblocks; b++) {
                unsigned char block[BLOCK_SIZE];
                if (read_full(fd, block, BLOCK_SIZE) < 0)
                    die("archive truncated after symlink entry %s", full_name);
            }

            printf("piko-update: verify OK  %-40s (symlink -> %s)\n", want->path, want->target);
            want->seen = 1;
            continue;
        }

        die("unsupported entry type '%c' for %s (only files, directories and symlinks are supported)",
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

static void install_file(const struct manifest_entry *e)
{
    char staged[MAX_PATH + 32];
    char dest[MAX_PATH + 8];
    char dest_dir[MAX_PATH + 16];

    snprintf(staged, sizeof(staged), "%s/%s", STAGING_DIR, e->path);
    snprintf(dest, sizeof(dest), "/%s", e->path);
    path_dirname(dest, dest_dir, sizeof(dest_dir));

    if (mkdir_p(dest_dir) < 0)
        die("could not create %s: %s", dest_dir, strerror(errno));

    /* The one file with a documented recovery story (see
     * docs/HOWTO-BUILD-DEPLOY-KERNEL.md): back it up before replacing so
     * a panicking new kernel can be undone by hand at the console. */
    if (strcmp(e->path, ZIMAGE_PATH) == 0 && access(dest, F_OK) == 0) {
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
         * fall back to a plain copy (or, for a symlink, a fresh
         * readlink+symlink -- rename() across filesystems can't move a
         * symlink object itself, and opening it directly would follow it
         * to whatever it points at instead of copying the link). */
        if (e->is_symlink) {
            unlink(dest);
            if (symlink(e->target, dest) < 0)
                die("could not create symlink %s -> %s: %s", dest, e->target, strerror(errno));
            unlink(staged);
        } else {
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
    }

    printf("piko-update: install %s\n", dest);
}

static void apply_update(void)
{
    int i;

    for (i = 0; i < manifest_count; i++)
        install_file(&manifest[i]);

    sync();
    printf("piko-update: %d file(s) installed.\n", manifest_count);
}

/* ---------------------------------------------------------------------- *
 * smf / mtd1 bootstrap partition                                          *
 * ---------------------------------------------------------------------- */

/* Runs SMF_WRITER and returns its exit status, or -1 if it could not be
 * run at all. -1 and "nonzero exit" are deliberately distinguishable:
 * callers must not read "couldn't exec" as any kind of answer about what
 * is in flash. */
static int run_writer(char *const args[])
{
    pid_t pid = fork();
    int status;

    if (pid < 0)
        return -1;
    if (pid == 0) {
        execv(SMF_WRITER, args);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (!WIFEXITED(status))
        return -1;
    if (WEXITSTATUS(status) == 127)
        return -1;
    return WEXITSTATUS(status);
}

/* Locates the smf partition by name in /proc/mtd. See SMF_LADDR's comment
 * for why this is never a hardcoded mtdN. */
static int find_smf_mtd(char *out, size_t outsz)
{
    FILE *f = fopen("/proc/mtd", "r");
    char line[256];
    int found = 0;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        int n;
        if (strstr(line, "\"smf\"") && sscanf(line, "mtd%d:", &n) == 1) {
            snprintf(out, outsz, "/dev/mtd%d", n);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

static int md5_file(const char *path, char *hex_out)
{
    int fd = open(path, O_RDONLY);
    unsigned char buf[BLOCK_SIZE * 8];
    unsigned char digest[16];
    md5_ctx ctx;
    ssize_t n;

    if (fd < 0)
        return -1;
    md5_init(&ctx);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        md5_update(&ctx, buf, (size_t)n);
    close(fd);
    if (n < 0)
        return -1;
    md5_final(&ctx, digest);
    md5_to_hex(digest, hex_out);
    return 0;
}

/* Fills *ac (1 mains, 0 battery, -1 unknown) and *pct (or -1 unknown).
 * Returns -1 if /proc/apm could not be read or parsed at all. */
static int read_apm(int *ac, int *pct)
{
    FILE *f = fopen("/proc/apm", "r");
    char driver[32];
    int maj, min;
    unsigned aflags, ac_raw, bstat, bflag;
    int percent;
    int got;

    *ac = -1;
    *pct = -1;
    if (!f)
        return -1;
    got = fscanf(f, "%31s %d.%d 0x%x 0x%x 0x%x 0x%x %d%%",
                 driver, &maj, &min, &aflags, &ac_raw, &bstat, &bflag, &percent);
    fclose(f);
    if (got < 5)
        return -1;

    if (ac_raw == 0x01)
        *ac = 1;
    else if (ac_raw == 0x00)
        *ac = 0;

    if (got >= 8 && percent >= 0 && percent <= 100)
        *pct = percent;
    return 0;
}

/* Returns 0 if it is acceptable to erase the bootstrap partition now.
 *
 * A torn smf write is unrecoverable on this board, and the window is long
 * (full-partition backup, then erase+program). Losing power inside it is
 * the realistic way to lose the board, so this refuses on battery.
 *
 * If /proc/apm cannot be read it warns and continues rather than blocking:
 * this board's APM has been unreliable before (it reported -1% until the
 * sharpsl param block was restored), and a sensor that is merely broken
 * must not make the updater permanently unusable. Explicit "on battery" is
 * a refusal; "no idea" is a warning. --force overrides either. */
static int power_ok(int force)
{
    int ac, pct;

    if (read_apm(&ac, &pct) < 0) {
        printf("piko-update: WARNING: cannot read /proc/apm -- power state unknown.\n");
        printf("piko-update:          Make sure the AC adapter is connected.\n");
        return 0;
    }

    if (ac == 1) {
        if (pct >= 0)
            printf("piko-update: power: on AC (battery %d%%)\n", pct);
        else
            printf("piko-update: power: on AC\n");
        return 0;
    }

    if (ac == 0) {
        fprintf(stderr, "piko-update: refusing to write smf on battery power.\n");
        fprintf(stderr, "piko-update: connect the AC adapter and run smfcommit again.\n");
        if (force)
            fprintf(stderr, "piko-update: --force given, continuing anyway.\n");
        return force ? 0 : -1;
    }

    printf("piko-update: WARNING: /proc/apm reports AC status as unknown.\n");
    printf("piko-update:          Make sure the AC adapter is connected.\n");
    return 0;
}

/* Asks the writer whether mtd1 already holds exactly `image`.
 * Returns the writer's exit code, or -1 if it could not be consulted. */
static int smf_compare(const char *mtd_dev, const char *image)
{
    char laddr[32], maxsz[32];
    char *args[8];

    snprintf(laddr, sizeof(laddr), "%u", (unsigned)SMF_LADDR);
    snprintf(maxsz, sizeof(maxsz), "%u", (unsigned)SMF_MAX);

    args[0] = (char *)"piko-smf-write";
    args[1] = (char *)"--compare";
    args[2] = (char *)mtd_dev;
    args[3] = (char *)image;
    args[4] = laddr;
    args[5] = maxsz;
    args[6] = NULL;

    return run_writer(args);
}

static void smf_write_pending(const char *md5hex)
{
    FILE *f = fopen(SMF_PENDING, "w");

    if (!f)
        die("could not record pending smf update in %s: %s",
            SMF_PENDING, strerror(errno));
    fprintf(f, "%s\n%s  %s\n", SMF_PENDING_MAGIC, md5hex, SMF_IMAGE_PATH);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    sync();
}

static void smf_notice_pending(void)
{
    printf("\n");
    printf("  ============================================================\n");
    printf("   BOOTSTRAP UPDATE PENDING\n");
    printf("  \n");
    printf("   This package also updates the smf boot partition, which\n");
    printf("   is NOT being written now. The device will reboot into the\n");
    printf("   new system first.\n");
    printf("  \n");
    printf("   If it comes back up fine, plug in the AC adapter and type:\n");
    printf("  \n");
    printf("       smfcommit\n");
    printf("  \n");
    printf("   If it does not come back up, do NOT run that -- the old\n");
    printf("   bootstrap is still in place and still works.\n");
    printf("  ============================================================\n");
    printf("\n");
}

/* Called after home (mtd3) has been installed. Decides whether an smf
 * write is needed later, and never performs one. */
static void smf_stage_pending(void)
{
    char mtd_dev[64];
    char hex[33];
    int cmp;

    if (access(SMF_IMAGE_PATH, F_OK) != 0)
        return;                 /* package carries no bootstrap image */

    if (md5_file(SMF_IMAGE_PATH, hex) < 0)
        die("could not hash %s: %s", SMF_IMAGE_PATH, strerror(errno));

    if (find_smf_mtd(mtd_dev, sizeof(mtd_dev)) < 0) {
        printf("piko-update: no \"smf\" partition in /proc/mtd -- "
               "skipping bootstrap update.\n");
        return;
    }

    cmp = smf_compare(mtd_dev, SMF_IMAGE_PATH);
    if (cmp == SMF_CMP_SAME) {
        printf("piko-update: bootstrap (smf) already up to date, nothing to do.\n");
        unlink(SMF_PENDING);    /* clear any stale marker */
        return;
    }

    if (cmp != SMF_CMP_DIFFERS) {
        /* Could not get a confident answer. Record it as pending rather
         * than dropping it -- the commit step re-checks everything and
         * will refuse loudly if things are still wrong. Silently skipping
         * would leave the user believing the bootstrap was updated. */
        printf("piko-update: WARNING: could not compare %s against %s.\n",
               SMF_IMAGE_PATH, mtd_dev);
        printf("piko-update:          Recording it as pending to be safe.\n");
    }

    smf_write_pending(hex);
    smf_notice_pending();
}

/* piko-update --commit-smf: the deliberate second step, run by hand after
 * the new system has booted. This is the only code path that erases NAND. */
static void smf_commit(int force)
{
    char mtd_dev[64];
    char want[33], have[33];
    char backup[MAX_PATH];
    char laddr[32], maxsz[32];
    char line[256];
    FILE *f;
    int cmp, rc;
    char *args[8];

    f = fopen(SMF_PENDING, "r");
    if (!f) {
        printf("piko-update: no bootstrap update is pending (%s absent).\n",
               SMF_PENDING);
        printf("piko-update: nothing to do.\n");
        return;
    }
    if (!fgets(line, sizeof(line), f) ||
        strncmp(line, SMF_PENDING_MAGIC, strlen(SMF_PENDING_MAGIC)) != 0) {
        fclose(f);
        die("%s is not a pending-update marker this version understands",
            SMF_PENDING);
    }
    if (fscanf(f, "%32s", want) != 1 || !is_hex32(want)) {
        fclose(f);
        die("%s has no usable checksum line", SMF_PENDING);
    }
    fclose(f);

    if (access(SMF_IMAGE_PATH, F_OK) != 0)
        die("%s is pending but %s is missing -- re-run the package",
            SMF_PENDING, SMF_IMAGE_PATH);

    /* Re-hash rather than trusting the earlier verify: the image has sat
     * on flash across at least one reboot since it was checked. */
    if (md5_file(SMF_IMAGE_PATH, have) < 0)
        die("could not hash %s: %s", SMF_IMAGE_PATH, strerror(errno));
    if (strcmp(want, have) != 0)
        die("%s changed since it was staged (expected %s, got %s) -- "
            "refusing to flash it", SMF_IMAGE_PATH, want, have);
    printf("piko-update: pending image verified: %s (%s)\n", SMF_IMAGE_PATH, have);

    if (find_smf_mtd(mtd_dev, sizeof(mtd_dev)) < 0)
        die("no \"smf\" partition found in /proc/mtd");
    printf("piko-update: smf partition: %s\n", mtd_dev);

    if (access(SMF_WRITER, X_OK) != 0)
        die("%s is missing or not executable", SMF_WRITER);

    cmp = smf_compare(mtd_dev, SMF_IMAGE_PATH);
    if (cmp == SMF_CMP_SAME) {
        printf("piko-update: %s already holds this image -- nothing to write.\n",
               mtd_dev);
        unlink(SMF_PENDING);
        sync();
        return;
    }
    if (cmp != SMF_CMP_DIFFERS)
        die("could not read %s to compare it (writer exit %d) -- "
            "not erasing a partition I cannot read", mtd_dev, cmp);

    if (power_ok(force) < 0)
        exit(1);

    /* Take the backup as its own verified step before anything is erased,
     * so a bad backup is discovered while the board still boots. */
    snprintf(backup, sizeof(backup), "/boot/smf-backup-%.8s.bin", have);
    printf("piko-update: backing up %s to %s ...\n", mtd_dev, backup);

    args[0] = (char *)"piko-smf-write";
    args[1] = (char *)"--backup";
    args[2] = mtd_dev;
    args[3] = backup;
    args[4] = NULL;
    rc = run_writer(args);
    if (rc != 0)
        die("backup of %s failed (writer exit %d) -- nothing was erased", mtd_dev, rc);
    sync();

    printf("\n");
    printf("  *** WRITING BOOTSTRAP PARTITION -- DO NOT POWER OFF ***\n\n");

    snprintf(laddr, sizeof(laddr), "%u", (unsigned)SMF_LADDR);
    snprintf(maxsz, sizeof(maxsz), "%u", (unsigned)SMF_MAX);
    args[0] = (char *)"piko-smf-write";
    args[1] = mtd_dev;
    args[2] = (char *)SMF_IMAGE_PATH;
    args[3] = laddr;
    args[4] = maxsz;
    args[5] = NULL;
    rc = run_writer(args);
    sync();

    if (rc != 0) {
        fprintf(stderr, "\npiko-update: smf write FAILED (writer exit %d).\n", rc);
        fprintf(stderr, "piko-update: a verified backup is at %s\n", backup);
        fprintf(stderr, "piko-update: DO NOT power off. The running system is\n");
        fprintf(stderr, "piko-update: still fine -- see docs/FLASH-MTD1-MTD3-SAFE.md\n");
        fprintf(stderr, "piko-update: before rebooting.\n");
        exit(1);
    }

    /* The writer verifies its own payload readback (its Step 7), so
     * reaching here means the flash content was confirmed. */
    unlink(SMF_PENDING);
    sync();

    printf("\npiko-update: bootstrap updated and verified.\n");
    printf("piko-update: backup kept at %s\n", backup);
    printf("piko-update: the new bootstrap takes effect at the next COLD boot.\n");
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <package.tar> [--dry-run] [--no-reboot]\n"
        "       %s --commit-smf [--force]\n"
        "\n"
        "  --dry-run     verify the package, change nothing\n"
        "  --no-reboot   install but leave the reboot to you\n"
        "  --commit-smf  write a pending bootstrap update to the smf\n"
        "                partition (also available as `smfcommit`)\n"
        "  --force       with --commit-smf, proceed on battery power\n",
        prog, prog);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *archive_path = NULL;
    int commit_smf = 0;
    int force = 0;
    int i;
    int lockfd;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0)
            dry_run = 1;
        else if (strcmp(argv[i], "--no-reboot") == 0)
            no_reboot = 1;
        else if (strcmp(argv[i], "--commit-smf") == 0)
            commit_smf = 1;
        else if (strcmp(argv[i], "--force") == 0)
            force = 1;
        else if (!archive_path)
            archive_path = argv[i];
        else
            usage(argv[0]);
    }
    if (commit_smf ? archive_path != NULL : archive_path == NULL)
        usage(argv[0]);

    lockfd = open(LOCK_PATH, O_CREAT | O_RDWR, 0644);
    if (lockfd < 0)
        die("could not open lock file %s: %s", LOCK_PATH, strerror(errno));
    if (flock(lockfd, LOCK_EX | LOCK_NB) < 0)
        die("another piko-update is already running (lock held on %s)", LOCK_PATH);

    /* Held under the same lock as a package install: committing a
     * bootstrap write while an install is mid-flight would be exactly the
     * concurrency the lock exists to prevent. */
    if (commit_smf) {
        smf_commit(force);
        return 0;
    }

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

    /* Only after home is installed, and never a write -- see the header. */
    smf_stage_pending();

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
