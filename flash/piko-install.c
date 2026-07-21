#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define SMF_MTD      "/dev/mtd1"
#define TARGET_FILE  "zImage"
#define TMP_CHUNK    "/tmp/update/tmpdata.bin"
#define START_ADDR   917504    /* 0xE0000 */
#define CHUNK_SIZE   524288    /* 0x80000 */
#define MAX_KERNEL   1294336   /* 0x13C000 */

static int run(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        perror("execv");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Piko Install: usage: piko-install <datapath>\n");
        return 1;
    }
    const char *datapath = argv[1];

    mkdir("/tmp/update", 0755);

    if (chdir(datapath) < 0) {
        fprintf(stderr, "Piko Install: cannot cd to %s\n", datapath);
        return 1;
    }

    struct stat st;
    if (stat(TARGET_FILE, &st) < 0) {
        fprintf(stderr, "Piko Install: %s not found on card\n", TARGET_FILE);
        return 1;
    }

    long datasize = (long)st.st_size;
    if (datasize > MAX_KERNEL) {
        fprintf(stderr, "Piko Install: kernel too big for flash slot (%ld > %d)\n",
                datasize, MAX_KERNEL);
        return 1;
    }

    printf("Piko Install: flashing %s (%ld bytes) to %s at offset %d\n",
           TARGET_FILE, datasize, SMF_MTD, START_ADDR);

    int src = open(TARGET_FILE, O_RDONLY);
    if (src < 0) {
        perror("open target");
        return 1;
    }

    long pos = 0;
    long addr = START_ADDR;
    char *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        fprintf(stderr, "Piko Install: out of memory\n");
        return 1;
    }

    while (pos < datasize) {
        ssize_t n = read(src, buf, CHUNK_SIZE);
        if (n <= 0) {
            fprintf(stderr, "Piko Install: read error at offset %ld\n", pos);
            close(src);
            free(buf);
            return 1;
        }

        int fd = open(TMP_CHUNK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("open chunk");
            close(src);
            free(buf);
            return 1;
        }
        if (write(fd, buf, n) != n) {
            perror("write chunk");
            close(fd);
            close(src);
            free(buf);
            return 1;
        }
        close(fd);

        char addr_s[32], size_s[32];
        snprintf(addr_s, sizeof(addr_s), "%ld", addr);
        snprintf(size_s, sizeof(size_s), "%ld", (long)n);

        char *nandlogical_argv[] = {
            "/sbin/nandlogical", SMF_MTD, "WRITE", addr_s, size_s, TMP_CHUNK, NULL
        };
        if (run(nandlogical_argv) != 0) {
            fprintf(stderr, "Piko Install: nandlogical write failed at offset %ld\n", pos);
            unlink(TMP_CHUNK);
            close(src);
            free(buf);
            return 1;
        }

        unlink(TMP_CHUNK);
        pos += n;
        addr += n;
        printf("Piko Install: wrote %ld of %ld\n", pos, datasize);
    }

    close(src);
    free(buf);

    printf("Piko Install: kernel flash complete. Power off and reboot.\n");
    return 0;
}
