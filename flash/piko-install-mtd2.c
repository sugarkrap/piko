/* Freestanding ARM OABI Linux binary: no libc, no TLS, targets ancient
 * pre-EABI kernels (syscall number encoded in the swi immediate, not r7). */

#define OABI_BASE 0x900000

#define SYSCALL1(fname, nr) \
static long fname(long a0) { \
    register long r0 asm("r0") = a0; \
    asm volatile ("swi %1" : "+r"(r0) : "i"(OABI_BASE + (nr)) : "memory"); \
    return r0; \
}

#define SYSCALL2(fname, nr) \
static long fname(long a0, long a1) { \
    register long r0 asm("r0") = a0; \
    register long r1 asm("r1") = a1; \
    asm volatile ("swi %2" : "+r"(r0) : "r"(r1), "i"(OABI_BASE + (nr)) : "memory"); \
    return r0; \
}

#define SYSCALL3(fname, nr) \
static long fname(long a0, long a1, long a2) { \
    register long r0 asm("r0") = a0; \
    register long r1 asm("r1") = a1; \
    register long r2 asm("r2") = a2; \
    asm volatile ("swi %3" : "+r"(r0) : "r"(r1), "r"(r2), "i"(OABI_BASE + (nr)) : "memory"); \
    return r0; \
}

#define SYSCALL4(fname, nr) \
static long fname(long a0, long a1, long a2, long a3) { \
    register long r0 asm("r0") = a0; \
    register long r1 asm("r1") = a1; \
    register long r2 asm("r2") = a2; \
    register long r3 asm("r3") = a3; \
    asm volatile ("swi %4" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r3), "i"(OABI_BASE + (nr)) : "memory"); \
    return r0; \
}

SYSCALL1(sys_exit,   1)
SYSCALL3(sys_read,   3)
SYSCALL3(sys_write,  4)
SYSCALL3(sys_open,   5)
SYSCALL1(sys_close,  6)
SYSCALL4(sys_wait4,  114)
SYSCALL1(sys_fork_,  2)
SYSCALL1(sys_unlink, 10)
SYSCALL3(sys_execve, 11)
SYSCALL1(sys_chdir,  12)
SYSCALL3(sys_lseek,  19)
SYSCALL2(sys_mkdir,  39)
SYSCALL2(sys_dup2,   63)

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000
#define SEEK_SET 0
#define SEEK_END 2

static void _exit_(int code) { sys_exit(code); __builtin_unreachable(); }

int raise(int sig) { (void)sig; _exit_(139); return 0; }

static long sys_fork(void) { return sys_fork_(0); }

typedef unsigned long size_t;

static size_t strlen_(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

void *memcpy(void *dst, const void *src, size_t n)
{
    char *d = dst;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

static long g_logfd = -1;

static void puts_(const char *s)
{
    size_t n = strlen_(s);
    sys_write(1, (long)s, n);
    if (g_logfd >= 0) sys_write(g_logfd, (long)s, n);
}

static void putnum(long v)
{
    char out[24];
    char tmp[24];
    int i = 0;
    int j = 0;
    unsigned long u;
    if (v < 0) { out[j++] = '-'; u = (unsigned long)(-v); }
    else u = (unsigned long)v;
    if (u == 0) { out[j++] = '0'; out[j] = 0; puts_(out); return; }
    while (u > 0) { tmp[i++] = '0' + (u % 10); u /= 10; }
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
    puts_(out);
}

#define SMF_MTD      "/dev/mtd2"
#define TARGET_FILE  "mtd2.jffs2"
#define TMP_CHUNK    "/tmp/update/tmpdata.bin"
#define TMP_VERIFY   "/tmp/update/verify.bin"
#define TMP_LOG      "/tmp/update/nandlog.txt"
#define START_ADDR   0
#define CHUNK_SIZE   524288
#define MAX_KERNEL   52428800

static char chunkbuf[CHUNK_SIZE];
static char verifybuf[CHUNK_SIZE];

static void itoa_(long v, char *out)
{
    int i = 0;
    char tmp[24];
    unsigned long u = (unsigned long)v;
    if (u == 0) { out[0] = '0'; out[1] = 0; return; }
    while (u > 0) { tmp[i++] = '0' + (u % 10); u /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

static void dump_log(void)
{
    long fd = sys_open((long)TMP_LOG, O_RDONLY, 0);
    if (fd < 0) return;
    char buf[512];
    long n;
    puts_("Piko Install: --- nandlogical output begin ---\n");
    while ((n = sys_read(fd, (long)buf, sizeof(buf))) > 0) {
        sys_write(1, (long)buf, n);
        if (g_logfd >= 0) sys_write(g_logfd, (long)buf, n);
    }
    puts_("Piko Install: --- nandlogical output end ---\n");
    sys_close(fd);
}

static int run_nandlogical(const char *mode, const char *addr_s, const char *size_s, const char *file)
{
    sys_unlink((long)TMP_LOG);
    long pid = sys_fork();
    if (pid < 0) {
        puts_("Piko Install: fork failed\n");
        return -1;
    }
    if (pid == 0) {
        long lfd = sys_open((long)TMP_LOG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (lfd >= 0) {
            sys_dup2(lfd, 1);
            sys_dup2(lfd, 2);
        }
        char *argv[] = { "/sbin/nandlogical", SMF_MTD, (char *)mode, (char *)addr_s, (char *)size_s, (char *)file, 0 };
        char *envp[] = { 0 };
        sys_execve((long)"/sbin/nandlogical", (long)argv, (long)envp);
        _exit_(127);
    }
    int status = 0;
    long wr = sys_wait4(pid, (long)&status, 0, 0);

    puts_("Piko Install: waitpid returned ");
    putnum(wr);
    puts_(" (child pid was ");
    putnum(pid);
    puts_(")\n");
    puts_("Piko Install: nandlogical raw status ");
    putnum(status);
    puts_("\n");

    {
        long ffd = sys_open((long)file, O_RDONLY, 0);
        puts_("Piko Install: target file open returned fd ");
        putnum(ffd);
        puts_("\n");
        if (ffd >= 0) {
            long fsz = sys_lseek(ffd, 0, SEEK_END);
            puts_("Piko Install: target file size ");
            putnum(fsz);
            puts_("\n");
            sys_close(ffd);
        }
    }

    dump_log();
    sys_unlink((long)TMP_LOG);
    if ((status & 0x7f) != 0) return -1;
    if (((status >> 8) & 0xff) != 0) return -1;
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        puts_("Piko Install: usage: piko-install <datapath>\n");
        _exit_(1);
    }
    const char *datapath = argv[1];

    long mkdir_r = sys_mkdir((long)"/tmp/update", 0755);

    if (sys_chdir((long)datapath) < 0) {
        puts_("Piko Install: cannot cd to datapath\n");
        _exit_(1);
    }

    g_logfd = sys_open((long)"piko-log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    puts_("Piko Install: selftest: mkdir /tmp/update returned ");
    putnum(mkdir_r);
    puts_("\n");

    {
        long tfd = sys_open((long)"/tmp/update/selftest.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        puts_("Piko Install: selftest: open for write returned fd ");
        putnum(tfd);
        puts_("\n");
        if (tfd >= 0) {
            long wr = sys_write(tfd, (long)"hello", 5);
            puts_("Piko Install: selftest: write returned ");
            putnum(wr);
            puts_("\n");
            sys_close(tfd);
        }

        long rfd = sys_open((long)"/tmp/update/selftest.txt", O_RDONLY, 0);
        puts_("Piko Install: selftest: open for read returned fd ");
        putnum(rfd);
        puts_("\n");
        if (rfd >= 0) {
            char rb[16];
            long rn = sys_read(rfd, (long)rb, sizeof(rb));
            puts_("Piko Install: selftest: read returned ");
            putnum(rn);
            puts_("\n");
            sys_close(rfd);
        }

        long ur = sys_unlink((long)"/tmp/update/selftest.txt");
        puts_("Piko Install: selftest: unlink returned ");
        putnum(ur);
        puts_("\n");
    }

    puts_("Piko Install: isolated read test (offset 0, 4096 bytes)\n");
    sys_unlink((long)TMP_VERIFY);
    run_nandlogical("READ", "0", "4096", TMP_VERIFY);

    long src = sys_open((long)TARGET_FILE, O_RDONLY, 0);
    if (src < 0) {
        puts_("Piko Install: mtd2.jffs2 not found on card\n");
        _exit_(1);
    }

    long datasize = sys_lseek(src, 0, SEEK_END);
    sys_lseek(src, 0, SEEK_SET);
    if (datasize < 0) {
        puts_("Piko Install: lseek failed\n");
        _exit_(1);
    }
    if (datasize > MAX_KERNEL) {
        puts_("Piko Install: mtd2 image too big for partition: ");
        putnum(datasize);
        puts_("\n");
        _exit_(1);
    }

    puts_("Piko Install: flashing mtd2.jffs2 (");
    putnum(datasize);
    puts_(" bytes) to " SMF_MTD " at offset ");
    putnum(START_ADDR);
    puts_("\n");

    long pos = 0;
    long addr = START_ADDR;

    while (pos < datasize) {
        long n = sys_read(src, (long)chunkbuf, CHUNK_SIZE);
        if (n <= 0) {
            puts_("Piko Install: read error\n");
            sys_close(src);
            _exit_(1);
        }

        long fd = sys_open((long)TMP_CHUNK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            puts_("Piko Install: cannot open tmp chunk\n");
            sys_close(src);
            _exit_(1);
        }
        if (sys_write(fd, (long)chunkbuf, n) != n) {
            puts_("Piko Install: write chunk failed\n");
            sys_close(fd);
            sys_close(src);
            _exit_(1);
        }
        sys_close(fd);

        char addr_s[24];
        char size_s[24];
        itoa_(addr, addr_s);
        itoa_(n, size_s);

        puts_("Piko Install: writing chunk at addr ");
        puts_(addr_s);
        puts_(" size ");
        puts_(size_s);
        puts_("\n");

        if (run_nandlogical("WRITE", addr_s, size_s, TMP_CHUNK) != 0) {
            puts_("Piko Install: nandlogical write failed\n");
            sys_unlink((long)TMP_CHUNK);
            sys_close(src);
            _exit_(1);
        }

        sys_unlink((long)TMP_CHUNK);
        pos += n;
        addr += n;
        puts_("Piko Install: wrote ");
        putnum(pos);
        puts_(" of ");
        putnum(datasize);
        puts_("\n");
    }

    sys_close(src);
    puts_("Piko Install: write pass complete. Verifying...\n");

    /* Verification pass: re-read zImage and the just-written flash region, compare. */
    src = sys_open((long)TARGET_FILE, O_RDONLY, 0);
    if (src < 0) {
        puts_("Piko Install: verify: cannot reopen mtd2.jffs2\n");
        _exit_(1);
    }

    pos = 0;
    addr = START_ADDR;
    int mismatch = 0;

    while (pos < datasize && !mismatch) {
        long n = sys_read(src, (long)chunkbuf, CHUNK_SIZE);
        if (n <= 0) {
            puts_("Piko Install: verify: read error\n");
            sys_close(src);
            _exit_(1);
        }

        char addr_s[24];
        char size_s[24];
        itoa_(addr, addr_s);
        itoa_(n, size_s);

        sys_unlink((long)TMP_VERIFY);
        if (run_nandlogical("READ", addr_s, size_s, TMP_VERIFY) != 0) {
            puts_("Piko Install: verify: nandlogical read failed (non-fatal, skipping verify)\n");
            mismatch = -1;
            break;
        }

        long vfd = sys_open((long)TMP_VERIFY, O_RDONLY, 0);
        if (vfd < 0) {
            puts_("Piko Install: verify: cannot open readback file (non-fatal, skipping verify)\n");
            mismatch = -1;
            break;
        }
        long vn = sys_read(vfd, (long)verifybuf, CHUNK_SIZE);
        sys_close(vfd);
        sys_unlink((long)TMP_VERIFY);

        if (vn != n) {
            puts_("Piko Install: verify: size mismatch, expected ");
            putnum(n);
            puts_(" got ");
            putnum(vn);
            puts_("\n");
            mismatch = 1;
            break;
        }

        long i;
        for (i = 0; i < n; i++) {
            if (chunkbuf[i] != verifybuf[i]) {
                puts_("Piko Install: verify: MISMATCH at file offset ");
                putnum(pos + i);
                puts_(" expected 0x");
                {
                    char hex[] = "0123456789abcdef";
                    unsigned char eb = (unsigned char)chunkbuf[i];
                    unsigned char ab = (unsigned char)verifybuf[i];
                    char h[3] = { hex[eb >> 4], hex[eb & 0xf], 0 };
                    puts_(h);
                    puts_(" got 0x");
                    char h2[3] = { hex[ab >> 4], hex[ab & 0xf], 0 };
                    puts_(h2);
                }
                puts_("\n");
                mismatch = 1;
                break;
            }
        }

        pos += n;
        addr += n;
    }

    sys_close(src);

    if (mismatch == 1) {
        puts_("Piko Install: VERIFY FAILED. Flash NOT confirmed correct.\n");
        _exit_(1);
    }
    if (mismatch == -1) {
        puts_("Piko Install: verify unavailable, but write pass reported success. Power off and reboot.\n");
        _exit_(0);
    }

    puts_("Piko Install: VERIFY OK. mtd2 flash confirmed correct. Power off and reboot.\n");
    _exit_(0);
    return 0;
}

/* Custom entry point: ARM kernel start puts argc at [sp], argv at [sp+4]. */
asm (
    ".global _start\n"
    "_start:\n"
    "    mov r0, sp\n"
    "    and sp, sp, #-8\n"
    "    bl _entry\n"
);

void _entry(long *stack)
{
    int argc = (int)stack[0];
    char **argv = (char **)&stack[1];
    int ret = main(argc, argv);
    _exit_(ret);
}
