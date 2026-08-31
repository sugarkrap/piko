
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BLOCK_SIZE 512
#define MAX_PATH   300

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

static void die(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "untar: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
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

static void extract_file_entry(int fd, const char *dest_path, long size, mode_t mode)
{
    long nblocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    long b;
    long written = 0;
    int out;
    char dir[MAX_PATH + 16];
    char temp_path[MAX_PATH + 16];

    path_dirname(dest_path, dir, sizeof(dir));
    if (mkdir_p(dir) < 0)
        die("could not create directory %s: %s", dir, strerror(errno));

    if (snprintf(temp_path, sizeof(temp_path), "%s.untar-new", dest_path) >= (int)sizeof(temp_path))
        die("path too long: %s", dest_path);

    out = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
        die("could not create %s: %s", temp_path, strerror(errno));

    for (b = 0; b < nblocks; b++) {
        unsigned char block[BLOCK_SIZE];
        long used;

        if (read_full(fd, block, BLOCK_SIZE) < 0)
            die("archive truncated while reading %s", dest_path);

        used = size - written;
        if (used > BLOCK_SIZE)
            used = BLOCK_SIZE;
        if (used > 0) {
            if (write_full(out, block, (size_t)used) < 0)
                die("write failed extracting %s: %s", dest_path, strerror(errno));
            written += used;
        }
    }

    if (fchmod(out, mode) < 0)
        die("chmod failed on %s: %s", temp_path, strerror(errno));
    close(out);

    if (rename(temp_path, dest_path) < 0)
        die("could not replace %s: %s", dest_path, strerror(errno));
}

int main(int argc, char **argv)
{
    int fd;
    struct tar_header hdr;
    const char *archive_path;
    const char *dest_root;
    int nfiles = 0, ndirs = 0, nlinks = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <archive.tar> <destination-dir>\n", argv[0]);
        return 1;
    }
    archive_path = argv[1];
    dest_root = argv[2];

    if (mkdir_p(dest_root) < 0)
        die("could not create destination %s: %s", dest_root, strerror(errno));

    fd = open(archive_path, O_RDONLY);
    if (fd < 0)
        die("cannot open %s: %s", archive_path, strerror(errno));

    for (;;) {
        char full_name[MAX_PATH];
        char dest_path[MAX_PATH + 32];
        long size, nblocks, b;
        char typeflag;

        if (read_full(fd, &hdr, sizeof(hdr)) < 0)
            die("archive truncated (missing end-of-archive marker)");

        if (is_zero_block((unsigned char *)&hdr, sizeof(hdr)))
            break;

        if (!header_checksum_ok(&hdr))
            die("corrupt archive: bad header checksum");

        header_full_name(&hdr, full_name, sizeof(full_name));
        if (!path_is_safe(full_name))
            die("unsafe path in archive: %s", full_name);

        size = parse_octal(hdr.size, sizeof(hdr.size));
        nblocks = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        typeflag = hdr.typeflag;

        snprintf(dest_path, sizeof(dest_path), "%s/%s", dest_root, full_name);

        if (typeflag == '5') {
            mkdir_p(dest_path);
            ndirs++;
            for (b = 0; b < nblocks; b++) {
                unsigned char block[BLOCK_SIZE];
                if (read_full(fd, block, BLOCK_SIZE) < 0)
                    die("archive truncated after directory entry %s", full_name);
            }
            continue;
        }

        if (typeflag == '0' || typeflag == '\0') {
            mode_t mode = (mode_t)(parse_octal(hdr.mode, sizeof(hdr.mode)) & 07777);
            extract_file_entry(fd, dest_path, size, mode);
            nfiles++;
            continue;
        }

        if (typeflag == '2') {
            char linkname[101];

            memcpy(linkname, hdr.linkname, 100);
            linkname[100] = '\0';
            unlink(dest_path);
            if (symlink(linkname, dest_path) < 0)
                die("could not create symlink %s -> %s: %s", dest_path, linkname, strerror(errno));
            nlinks++;
            continue;
        }

        die("unsupported entry type '%c' for %s (only files, directories and symlinks are supported)",
            typeflag, full_name);
    }

    close(fd);
    printf("untar: extracted %d file(s), %d dir(s), %d symlink(s) to %s\n",
           nfiles, ndirs, nlinks, dest_root);
    return 0;
}
