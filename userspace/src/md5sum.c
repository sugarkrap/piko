
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
