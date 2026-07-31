/*
 * md5sum.c -- minimal MD5 file-hashing tool for the Zaurus device rootfs.
 *
 * The device's busybox build has no md5sum/sha1sum/cksum/cmp applet, so
 * tools/chunked-deploy.sh can currently only verify transfers by byte
 * count. This is a small, dependency-free CLI that mimics `md5sum FILE
 * [FILE...]` output ("<hex digest>  <filename>"), matching GNU/busybox
 * md5sum so it's a drop-in on both ends. The actual MD5 implementation
 * (RFC 1321, from scratch) lives in md5.h, shared with piko-update.c.
 *
 * Cross-compile (same toolchain as the rest of userspace/src):
 *   GCC=.../arm-buildroot-linux-uclibcgnueabi-gcc
 *   $GCC -march=armv5te -O2 -static -o md5sum md5sum.c
 *   arm-buildroot-linux-uclibcgnueabi-strip md5sum
 */

#include <stdio.h>
#include "md5.h"

static int hash_file(const char *path)
{
    FILE *f;
    md5_ctx ctx;
    unsigned char buf[8192];
    unsigned char digest[16];
    char hex[33];
    size_t n;

    if (strcmp(path, "-") == 0) {
        f = stdin;
    } else {
        f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "md5sum: %s: cannot open\n", path);
            return 1;
        }
    }

    md5_init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        md5_update(&ctx, buf, n);
    md5_final(&ctx, digest);

    if (f != stdin)
        fclose(f);

    md5_to_hex(digest, hex);
    printf("%s  %s\n", hex, path);
    return 0;
}

int main(int argc, char **argv)
{
    int i;
    int status = 0;

    if (argc < 2)
        return hash_file("-");

    for (i = 1; i < argc; i++) {
        if (hash_file(argv[i]) != 0)
            status = 1;
    }
    return status;
}
