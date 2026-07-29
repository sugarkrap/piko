/* Freestanding ARM OABI Linux binary: no libc, no TLS, targets ancient
 * pre-EABI kernels (syscall number encoded in the swi immediate, not r7).
 *
 * Combined installer: flashes the bootstrap kernel (mtd1/"smf") and/or the
 * full-system JFFS2 image (mtd3/"home") in a single run, since there is no
 * shell available in Cacko's recovery mode to invoke separate steps.
 *
 * Flash targets are read at runtime from piko.cfg in the datapath (SD card).
 * If no config file is present, compile-time defaults apply:
 *   -DPIKO_FLASH_MTD1  : mtd1 kernel slot (nandlogical, offset 917504)
 *   -DPIKO_FLASH_MTD3  : mtd3 home partition (eraseall+nandcp)
 *   (default when unspecified: MTD3)
 *
 * piko.cfg format (lines starting with '#' are comments):
 *   target <mtd_dev> <file> <start_addr> <max_size> <raw> <erase_only>
 *
 * Example:
 *   # Flash the bootstrap kernel only
 *   target /dev/mtd1 zImage 917504 1294336 0 0
 *
 * raw=0  -> nandlogical MEMWRITELADDR (mtd1/smf only)
 * raw=1  -> eraseall + nandcp (mtd2/mtd3)
 * erase_only=1 -> bulk-erase the partition and skip write+verify
 *
 * WARNING: NEVER use raw=1 on mtd1. See docs/DEADLETTER-RAW-FLAG.md.
 * WARNING: NEVER exceed start_addr=917504 / max_size=1294336 on mtd1.
 *          See docs/DEADLETTER-MTD1-OFFSET.md and the comment block below. */

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
SYSCALL3(sys_ioctl,  54)

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define SEEK_SET 0
#define SEEK_END 2

/* nandlogical's Sharp-specific "logical address" layer is only wired up
 * for the smf/kernel partition in this recovery environment; root/home
 * are flashed with the genuine Cacko recovery tools /sbin/eraseall and
 * /sbin/nandcp instead (see flash_one_raw() below), matching exactly
 * what updater.sh itself does for ISLOGICAL=0 targets. */

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

/* Forward-declared: defined near itoa_ below, hand-rolled because this
 * freestanding OABI build has no libgcc softdiv to call into. */
static unsigned long udiv10(unsigned long n, unsigned long *rem);

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
    while (u > 0) {
        unsigned long r;
        u = udiv10(u, &r);
        tmp[i++] = '0' + (char)r;
    }
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
    puts_(out);
}

#define TMP_CHUNK    "/tmp/update/tmpdata.bin"
#define TMP_VERIFY   "/tmp/update/verify.bin"
#define TMP_LOG      "/tmp/update/nandlog.txt"
#define CHUNK_SIZE   524288
/* mtd1/"smf" physical partition size, per /proc/mtd ("mtd1: 00700000 ..."). */
#define SMF_PART_SIZE 0x700000

static char chunkbuf[CHUNK_SIZE];
static char verifybuf[CHUNK_SIZE];

struct flash_target {
    const char *mtd_dev;
    const char *file;
    long start_addr;
    long max_size;
    int raw;        /* 0 = nandlogical LADDR (mtd1 only); 1 = eraseall+nandcp */
    int erase_only; /* 1 = bulk-erase only, skip write+verify */
};

/* -------------------------------------------------------------------------
 * Config file parser
 * Reads piko.cfg from datapath (already chdir'd there before this runs).
 * Format: one target per non-comment line:
 *   target <mtd_dev> <file> <start_addr> <max_size> <raw> <erase_only>
 * ------------------------------------------------------------------------- */

#define CFG_FILE      "piko.cfg"
#define MAX_CFG_TARGETS 8
#define CFG_FIELD_LEN   32

static struct flash_target cfg_targets[MAX_CFG_TARGETS];
static char cfg_mtd_buf[MAX_CFG_TARGETS][CFG_FIELD_LEN];
static char cfg_file_buf[MAX_CFG_TARGETS][CFG_FIELD_LEN];
static int  cfg_count = 0;

static void skip_ws(const char **p, const char *end)
{
    while (*p < end && (**p == ' ' || **p == '\t')) (*p)++;
}

static int parse_str_field(const char **p, const char *end, char *out, int maxlen)
{
    skip_ws(p, end);
    int n = 0;
    while (*p < end && **p != ' ' && **p != '\t' && **p != '\n' && **p != '\r') {
        if (n < maxlen - 1) out[n++] = **p;
        (*p)++;
    }
    out[n] = 0;
    return n > 0 ? 0 : -1;
}

static int parse_long_field(const char **p, const char *end, long *out)
{
    skip_ws(p, end);
    if (*p >= end || **p < '0' || **p > '9') return -1;
    unsigned long v = 0;
    while (*p < end && **p >= '0' && **p <= '9') {
        v = v * 10 + (unsigned long)(**p - '0');
        (*p)++;
    }
    *out = (long)v;
    return 0;
}

static int startswith(const char *p, const char *end, const char *kw)
{
    while (*kw) {
        if (p >= end || *p != *kw) return 0;
        p++; kw++;
    }
    return 1;
}

static void parse_config(const char *buf, long len)
{
    const char *p   = buf;
    const char *end = buf + len;

    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;

        const char *lp = p;
        skip_ws(&lp, nl);

        if (lp < nl && *lp != '#' && startswith(lp, nl, "target")) {
            lp += 6; /* skip "target" */
            if (cfg_count < MAX_CFG_TARGETS) {
                int idx = cfg_count;
                long sa = 0, ms = 0, rw = 0, eo = 0;
                if (parse_str_field(&lp, nl, cfg_mtd_buf[idx],  CFG_FIELD_LEN) == 0 &&
                    parse_str_field(&lp, nl, cfg_file_buf[idx], CFG_FIELD_LEN) == 0 &&
                    parse_long_field(&lp, nl, &sa) == 0 &&
                    parse_long_field(&lp, nl, &ms) == 0 &&
                    parse_long_field(&lp, nl, &rw) == 0 &&
                    parse_long_field(&lp, nl, &eo) == 0) {
                    cfg_targets[idx].mtd_dev    = cfg_mtd_buf[idx];
                    cfg_targets[idx].file       = cfg_file_buf[idx];
                    cfg_targets[idx].start_addr = sa;
                    cfg_targets[idx].max_size   = ms;
                    cfg_targets[idx].raw        = (int)rw;
                    cfg_targets[idx].erase_only = (int)eo;
                    cfg_count++;
                } else {
                    puts_("Piko Install: warning: malformed target line in " CFG_FILE "\n");
                }
            } else {
                puts_("Piko Install: warning: too many targets in " CFG_FILE ", ignoring extras\n");
            }
        }

        p = nl + 1;
    }
}

static void load_config(void)
{
    static char cfgbuf[4096];
    long fd = sys_open((long)CFG_FILE, O_RDONLY, 0);
    if (fd < 0) return;

    long total = 0, n;
    while (total < (long)sizeof(cfgbuf) &&
           (n = sys_read(fd, (long)(cfgbuf + total), sizeof(cfgbuf) - total)) > 0)
        total += n;
    sys_close(fd);

    puts_("Piko Install: reading targets from " CFG_FILE "\n");
    parse_config(cfgbuf, total);
}

/* -------------------------------------------------------------------------
 * Compile-time default targets (used when no piko.cfg is found).
 *
 * Back to Cacko's own conservative 1,294,336-byte kernel budget for mtd1.
 * The full physical "smf" partition is 7,340,032 bytes, and it looked
 * like there was plenty of unused room past Cacko's own budget -- but a
 * kernel that used that extra room (well within the partition's nominal
 * size) still bricked the device. Root cause is not fully confirmed, but
 * the FTL's reserved/spare block pool is tiny (24 blocks, per the "Sharp
 * SL FTL: 448 blocks used (424 logical, 24 reserved)" boot message), so a
 * write several times larger than anything Cacko's own installer ever
 * wrote at that logical range plausibly forced it to reclaim physical
 * blocks holding the recovery firmware. Never exceed this budget again --
 * always flash the small JFFS2+KEXEC bootstrap kernel here, never a
 * full-featured single-stage build.
 *
 * raw flag is per-partition, NEVER copy-paste it between mtd1 and
 * mtd2/mtd3 -- see docs/DEADLETTER-RAW-FLAG.md for the full incident
 * writeup. Short version: mtd1/smf ONLY understands Sharp's proprietary
 * nandlogical MEMWRITELADDR layer (raw=0, matching Cacko's own
 * kernel-flash.sh exactly). raw=1 routes to flash_one_raw(), which
 * shells out to eraseall+nandcp -- correct for mtd2/mtd3 only. */
#if defined(PIKO_FLASH_MTD1)
static const struct flash_target default_targets[] = {
    { "/dev/mtd1", "zImage", 917504, 1294336, 0, 0 },
};
#elif defined(PIKO_FLASH_MTD3)
static const struct flash_target default_targets[] = {
    { "/dev/mtd3", "mtd3.jffs2", 0, 71303168, 1, 0 },
};
#else
static const struct flash_target default_targets[] = {
    { "/dev/mtd3", "mtd3.jffs2", 0, 71303168, 1, 0 },
};
#endif
#define NUM_DEFAULT_TARGETS 1

/* -------------------------------------------------------------------------
 * nandlogical helpers (mtd1 / raw=0 path)
 * ------------------------------------------------------------------------- */

/* armv5te has no hardware divider, and this freestanding build has no
 * libgcc to call into for __udivsi3/__umodsi3 (this toolchain's libgcc.a
 * is EABI-only; this binary is deliberately OABI, see the file header) --
 * so division-by-10 is hand-rolled here via restoring binary division,
 * same spirit as every other syscall/libc primitive in this file. */
static unsigned long udiv10(unsigned long n, unsigned long *rem)
{
    unsigned long q = 0, r = 0;
    int i;
    for (i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1UL);
        if (r >= 10) { r -= 10; q |= (1UL << i); }
    }
    if (rem) *rem = r;
    return q;
}

static void itoa_(long v, char *out)
{
    int i = 0;
    char tmp[24];
    unsigned long u = (unsigned long)v;
    if (u == 0) { out[0] = '0'; out[1] = 0; return; }
    while (u > 0) {
        unsigned long r;
        u = udiv10(u, &r);
        tmp[i++] = '0' + (char)r;
    }
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

static int run_nandlogical(const char *mtd_dev, const char *mode, const char *addr_s, const char *size_s, const char *file)
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
        char *argv[] = { "/sbin/nandlogical", (char *)mtd_dev, (char *)mode, (char *)addr_s, (char *)size_s, (char *)file, 0 };
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

    dump_log();
    sys_unlink((long)TMP_LOG);
    if ((status & 0x7f) != 0) return -1;
    if (((status >> 8) & 0xff) != 0) return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * FTL logical-block diagnostic (mtd1 / raw=0 path only)
 *
 * nandlogical WRITE + a byte-perfect nandlogical READ verify (the existing
 * flash_one() success path) only proves the logical address round-trips
 * through nandlogical consistently -- it does NOT prove that logical
 * address is actually backed by a real, OOB-tagged physical block the way
 * the cold-boot path expects. Confirmed 2026-07-29 against a genuine
 * factory .dbk (see docs/DEADLETTER-MTD1-OFFSET.md and the FTL forensics
 * session that found it): the Sharp FTL stores 3 redundant copies of each
 * physical block's logical block number in OOB bytes 8-13 of that block's
 * first page (format: 16-bit value = (logical_block << 1) | even_parity,
 * see flash/pico-smf-write.c's ftl_encode/ftl_decode for the original
 * derivation). Physical block order has no fixed relationship to logical
 * order at all (block 0 mapped to logical 16 in that forensic sample) --
 * so the only trustworthy way to confirm a write "took" at the FTL level
 * is to scan every physical block's OOB and see which one(s), if any,
 * currently claim the logical range we just wrote.
 *
 * This scans the whole mtd1/smf partition (physical block 0 upward) via
 * raw MEMREADOOB ioctls -- cheap (448 small ioctls for the 7MB partition)
 * and safe (read-only). For each physical block whose OOB decodes to a
 * logical block inside [target_logical, target_logical + num_blocks), it
 * prints "physical P -> logical L". At the end it reports how many of the
 * expected logical blocks were actually found mapped anywhere -- if that
 * count is less than num_blocks, some logical blocks in the just-written
 * range have no OOB-tagged physical home at all, which would explain a
 * byte-perfect nandlogical verify that still doesn't survive a cold boot.
 * ------------------------------------------------------------------------- */

#define FTL_OOB_LEN         16
#define FTL_BLOCK_SIZE      16384
#define SHARPSL_ERASED_OOB  0xFFFF

struct mtd_oob_buf_ {
    unsigned long start;
    unsigned long length;
    unsigned char *ptr;
};

#define MEMREADOOB 0xc00c4d04

static int ftl_popcount16(unsigned int v)
{
    int c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

/* Returns the decoded logical block number, or -1 if erased/unmapped/bad parity. */
static int ftl_decode_oob(unsigned int us)
{
    if (us == SHARPSL_ERASED_OOB) return -1;
    if (ftl_popcount16(us) & 1) return -1;
    return (int)((us >> 1) & 0x3FF);
}

static void diagnose_ftl_mapping(const char *mtd_dev, long start_addr, long datasize)
{
    long target_logical = start_addr / FTL_BLOCK_SIZE;
    long num_blocks = (datasize + FTL_BLOCK_SIZE - 1) / FTL_BLOCK_SIZE;
    long num_phys_blocks = SMF_PART_SIZE / FTL_BLOCK_SIZE;

    puts_("Piko Install: --- FTL mapping diagnostic begin (logical ");
    putnum(target_logical);
    puts_("..");
    putnum(target_logical + num_blocks - 1);
    puts_(") ---\n");

    long fd = sys_open((long)mtd_dev, O_RDONLY, 0);
    if (fd < 0) {
        puts_("Piko Install: FTL diagnostic: cannot open ");
        puts_(mtd_dev);
        puts_(", skipping\n");
        return;
    }

    static char found[512]; /* one flag per logical block in range, up to 512 */
    long i;
    for (i = 0; i < num_blocks && i < 512; i++) found[i] = 0;

    static unsigned char oob[FTL_OOB_LEN];
    long phys;
    for (phys = 0; phys < num_phys_blocks; phys++) {
        struct mtd_oob_buf_ req;
        req.start  = (unsigned long)(phys * FTL_BLOCK_SIZE);
        req.length = FTL_OOB_LEN;
        req.ptr    = oob;
        for (i = 0; i < FTL_OOB_LEN; i++) oob[i] = 0xFF;

        if (sys_ioctl(fd, MEMREADOOB, (long)&req) < 0) continue;

        unsigned int c0 = (unsigned int)(oob[8]  | (oob[9]  << 8));
        unsigned int c1 = (unsigned int)(oob[10] | (oob[11] << 8));
        unsigned int c2 = (unsigned int)(oob[12] | (oob[13] << 8));
        int d0 = ftl_decode_oob(c0);
        int d1 = ftl_decode_oob(c1);
        int d2 = ftl_decode_oob(c2);

        int logical = -1;
        if (d0 >= 0 && d0 == d1) logical = d0;
        else if (d0 >= 0 && d0 == d2) logical = d0;
        else if (d1 >= 0 && d1 == d2) logical = d1;
        else if (d0 >= 0) logical = d0;
        else if (d1 >= 0) logical = d1;
        else if (d2 >= 0) logical = d2;

        if (logical >= target_logical && logical < target_logical + num_blocks) {
            puts_("Piko Install: FTL: physical ");
            putnum(phys);
            puts_(" -> logical ");
            putnum(logical);
            puts_("\n");
            long rel = logical - target_logical;
            if (rel < 512) found[rel] = 1;
        }
    }
    sys_close(fd);

    long coverage = 0;
    for (i = 0; i < num_blocks && i < 512; i++) coverage += found[i];
    puts_("Piko Install: FTL mapping diagnostic: ");
    putnum(coverage);
    puts_(" of ");
    putnum(num_blocks);
    puts_(" expected logical block(s) found mapped to a physical block.\n");
    if (coverage < num_blocks) {
        puts_("Piko Install: FTL WARNING: not every logical block in this write's range has an OOB-tagged physical home.\n");
    }
    puts_("Piko Install: --- FTL mapping diagnostic end ---\n");
}

/* -------------------------------------------------------------------------
 * eraseall + nandcp helpers (mtd2/mtd3 / raw=1 path)
 *
 * genuine updater.sh (Cacko's own installer) never touches mtd2/mtd3 via
 * raw MEMERASE+write() at all: for anything with ISLOGICAL=0 (rootfs,
 * home) it shells out to two dedicated Sharp/Cacko recovery tools:
 *   /sbin/eraseall $TARGET_MTD
 *   /sbin/nandcp -a $ADDR $CHUNK $TARGET_MTD
 * nandcp reports back where it actually finished via a line containing
 * "mtd address START-END(...)" on stdout; updater.sh parses END out with
 * `fgrep "mtd address" | cut -d- -f2 | cut -d'(' -f1` and feeds it in as
 * the next chunk's -a address. Our own hand-rolled MEMERASE+write() approach
 * never got data to land past the first eraseblock.
 * ------------------------------------------------------------------------- */

#define NANDCP_CHUNK 1048576 /* matches updater.sh's ONESIZE for root/home */
static char nandcpbuf[NANDCP_CHUNK];
/* Sized for full 1MB chunks -- must NOT reuse the CHUNK_SIZE verifybuf. */
static char verify_raw_buf[NANDCP_CHUNK];
/* Single large sys_read() spanning multiple erase blocks reports full
 * success but silently fails past the first block on this hardware.
 * Read back in erase-block-sized sub-reads with an explicit lseek() each. */
#define ERASE_SIZE 16384

#define MAX_CHUNKS 128
static long chunk_addrs[MAX_CHUNKS];
static long chunk_lens[MAX_CHUNKS];

static int parse_next_addr(const char *buf, long len, long *out)
{
    const char *needle = "mtd address";
    long nlen = (long)strlen_(needle);
    long i;
    for (i = 0; i + nlen <= len; i++) {
        long k;
        int match = 1;
        for (k = 0; k < nlen; k++) {
            if (buf[i + k] != needle[k]) { match = 0; break; }
        }
        if (!match) continue;

        long j = i + nlen;
        while (j < len && buf[j] != '-' && buf[j] != '\n') j++;
        if (j >= len || buf[j] != '-') continue;
        j++;
        while (j < len && (buf[j] == ' ' || buf[j] == '\t')) j++;

        int is_hex = 0;
        if (j + 1 < len && buf[j] == '0' && (buf[j + 1] == 'x' || buf[j + 1] == 'X')) {
            is_hex = 1;
            j += 2;
        }

        unsigned long val = 0;
        int got = 0;
        while (j < len) {
            char c = buf[j];
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (is_hex && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (is_hex && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else break;
            val = val * (is_hex ? 16u : 10u) + (unsigned long)digit;
            got = 1;
            j++;
        }
        if (got) { *out = (long)val; return 0; }
    }
    return -1;
}

static int run_eraseall(const char *mtd_dev)
{
    puts_("Piko Install: eraseall ");
    puts_(mtd_dev);
    puts_(" (bulk-erasing whole partition)...\n");

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
        char *argv[] = { "/sbin/eraseall", (char *)mtd_dev, 0 };
        char *envp[] = { 0 };
        sys_execve((long)"/sbin/eraseall", (long)argv, (long)envp);
        _exit_(127);
    }
    int status = 0;
    sys_wait4(pid, (long)&status, 0, 0);
    dump_log();
    sys_unlink((long)TMP_LOG);
    if ((status & 0x7f) != 0) return -1;
    if (((status >> 8) & 0xff) != 0) return -1;
    return 0;
}

/* Writes one chunk via nandcp and parses back the address it reports as
 * "next" (nandcp handles bad-block skipping internally; we just follow
 * whatever it says, exactly like updater.sh does). Returns 0 on success
 * with *next_addr filled in, -1 on hard failure, -2 if nandcp succeeded
 * but its output couldn't be parsed (caller falls back to addr+n). */
static int run_nandcp(long addr, const char *file, const char *mtd_dev, long *next_addr)
{
    char addr_s[24];
    itoa_(addr, addr_s);

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
        char *argv[] = { "/sbin/nandcp", "-a", addr_s, (char *)file, (char *)mtd_dev, 0 };
        char *envp[] = { 0 };
        sys_execve((long)"/sbin/nandcp", (long)argv, (long)envp);
        _exit_(127);
    }
    int status = 0;
    sys_wait4(pid, (long)&status, 0, 0);

    static char logbuf[2048];
    long total = 0;
    long fd = sys_open((long)TMP_LOG, O_RDONLY, 0);
    if (fd >= 0) {
        long n;
        while (total < (long)sizeof(logbuf) &&
               (n = sys_read(fd, (long)(logbuf + total), sizeof(logbuf) - total)) > 0) {
            total += n;
        }
        sys_close(fd);
    }
    puts_("Piko Install: --- nandcp output begin ---\n");
    if (total > 0) {
        sys_write(1, (long)logbuf, total);
        if (g_logfd >= 0) sys_write(g_logfd, (long)logbuf, total);
    }
    puts_("Piko Install: --- nandcp output end ---\n");
    sys_unlink((long)TMP_LOG);

    if ((status & 0x7f) != 0) return -1;
    if (((status >> 8) & 0xff) != 0) return -1;

    if (parse_next_addr(logbuf, total, next_addr) != 0) {
        puts_("Piko Install: warning: could not parse next address from nandcp output\n");
        return -2;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Per-target flash routines
 * ------------------------------------------------------------------------- */

static int flash_erase_only(const struct flash_target *t)
{
    puts_("Piko Install: === erase-only ");
    puts_(t->mtd_dev);
    puts_(" ===\n");
    return run_eraseall(t->mtd_dev);
}

static int flash_one_raw(const struct flash_target *t)
{
    puts_("Piko Install: === flashing (eraseall+nandcp) ");
    puts_(t->file);
    puts_(" to ");
    puts_(t->mtd_dev);
    puts_(" ===\n");

    long src = sys_open((long)t->file, O_RDONLY, 0);
    if (src < 0) {
        puts_("Piko Install: file not found: ");
        puts_(t->file);
        puts_("\n");
        return -1;
    }

    long datasize = sys_lseek(src, 0, SEEK_END);
    sys_lseek(src, 0, SEEK_SET);
    if (datasize < 0) {
        puts_("Piko Install: lseek failed\n");
        sys_close(src);
        return -1;
    }
    if (datasize > t->max_size) {
        puts_("Piko Install: image too big for partition: ");
        putnum(datasize);
        puts_("\n");
        sys_close(src);
        return -1;
    }

    if (run_eraseall(t->mtd_dev) != 0) {
        puts_("Piko Install: eraseall failed for ");
        puts_(t->mtd_dev);
        puts_("\n");
        sys_close(src);
        return -1;
    }

    long addr = t->start_addr;
    long pos = 0;
    int nchunks = 0;

    while (pos < datasize) {
        long n = sys_read(src, (long)nandcpbuf, NANDCP_CHUNK);
        if (n <= 0) {
            puts_("Piko Install: read error\n");
            sys_close(src);
            return -1;
        }

        long fd = sys_open((long)TMP_CHUNK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            puts_("Piko Install: cannot open tmp chunk\n");
            sys_close(src);
            return -1;
        }
        if (sys_write(fd, (long)nandcpbuf, n) != n) {
            puts_("Piko Install: write tmp chunk failed\n");
            sys_close(fd);
            sys_close(src);
            return -1;
        }
        sys_close(fd);

        if (nchunks >= MAX_CHUNKS) {
            puts_("Piko Install: too many chunks, increase MAX_CHUNKS\n");
            sys_close(src);
            return -1;
        }
        chunk_addrs[nchunks] = addr;
        chunk_lens[nchunks]  = n;

        long next_addr = addr + n;
        int rc = run_nandcp(addr, TMP_CHUNK, t->mtd_dev, &next_addr);
        sys_unlink((long)TMP_CHUNK);
        if (rc == -1) {
            puts_("Piko Install: nandcp write failed at addr ");
            putnum(addr);
            puts_("\n");
            sys_close(src);
            return -1;
        }

        nchunks++;
        pos  += n;
        addr  = next_addr;
        puts_("Piko Install: wrote ");
        putnum(pos);
        puts_(" of ");
        putnum(datasize);
        puts_(" (next addr ");
        putnum(addr);
        puts_(")\n");
    }
    sys_close(src);

    puts_("Piko Install: write pass complete for ");
    puts_(t->file);
    puts_(". Verifying...\n");

    long mtd = sys_open((long)t->mtd_dev, O_RDONLY, 0);
    if (mtd < 0) {
        puts_("Piko Install: verify: cannot open mtd device (non-fatal, skipping verify)\n");
        return 0;
    }
    src = sys_open((long)t->file, O_RDONLY, 0);
    if (src < 0) {
        puts_("Piko Install: verify: cannot reopen file\n");
        sys_close(mtd);
        return -1;
    }

    int mismatch = 0;
    int ci;
    long filepos = 0;
    for (ci = 0; ci < nchunks && !mismatch; ci++) {
        long n = sys_read(src, (long)nandcpbuf, chunk_lens[ci]);
        if (n != chunk_lens[ci]) {
            puts_("Piko Install: verify: short read from source file\n");
            mismatch = 1;
            break;
        }

        long off;
        for (off = 0; off < n && !mismatch; off += ERASE_SIZE) {
            long sublen = n - off;
            if (sublen > ERASE_SIZE) sublen = ERASE_SIZE;

            if (sys_lseek(mtd, chunk_addrs[ci] + off, SEEK_SET) < 0) {
                puts_("Piko Install: verify: lseek failed at ");
                putnum(chunk_addrs[ci] + off);
                puts_("\n");
                mismatch = 1;
                break;
            }
            long vn = sys_read(mtd, (long)(verify_raw_buf + off), sublen);
            if (vn != sublen) {
                puts_("Piko Install: verify: short read from mtd device at ");
                putnum(chunk_addrs[ci] + off);
                puts_("\n");
                mismatch = 1;
                break;
            }

            long i;
            for (i = 0; i < sublen; i++) {
                if (nandcpbuf[off + i] != verify_raw_buf[off + i]) {
                    puts_("Piko Install: verify: MISMATCH at file offset ");
                    putnum(filepos + off + i);
                    puts_(" (phys ");
                    putnum(chunk_addrs[ci] + off + i);
                    puts_(")\n");
                    mismatch = 1;
                    break;
                }
            }
        }
        filepos += n;
    }
    sys_close(mtd);
    sys_close(src);

    if (mismatch) {
        puts_("Piko Install: VERIFY FAILED for ");
        puts_(t->file);
        puts_("\n");
        return -1;
    }

    puts_("Piko Install: VERIFY OK for ");
    puts_(t->file);
    puts_("\n");
    return 0;
}

static int flash_one(const struct flash_target *t)
{
    if (t->raw && t->erase_only)
        return flash_erase_only(t);
    if (t->raw)
        return flash_one_raw(t);

    puts_("Piko Install: === flashing (nandlogical) ");
    puts_(t->file);
    puts_(" to ");
    puts_(t->mtd_dev);
    puts_(" ===\n");

    long src = sys_open((long)t->file, O_RDONLY, 0);
    if (src < 0) {
        puts_("Piko Install: file not found: ");
        puts_(t->file);
        puts_("\n");
        return -1;
    }

    long datasize = sys_lseek(src, 0, SEEK_END);
    sys_lseek(src, 0, SEEK_SET);
    if (datasize < 0) {
        puts_("Piko Install: lseek failed\n");
        sys_close(src);
        return -1;
    }
    if (datasize > t->max_size) {
        puts_("Piko Install: image too big for partition: ");
        putnum(datasize);
        puts_("\n");
        sys_close(src);
        return -1;
    }

    puts_("Piko Install: writing ");
    putnum(datasize);
    puts_(" bytes at offset ");
    putnum(t->start_addr);
    puts_("\n");

    long pos  = 0;
    long addr = t->start_addr;

    while (pos < datasize) {
        long n = sys_read(src, (long)chunkbuf, CHUNK_SIZE);
        if (n <= 0) {
            puts_("Piko Install: read error\n");
            sys_close(src);
            return -1;
        }

        long fd = sys_open((long)TMP_CHUNK, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            puts_("Piko Install: cannot open tmp chunk\n");
            sys_close(src);
            return -1;
        }
        if (sys_write(fd, (long)chunkbuf, n) != n) {
            puts_("Piko Install: write chunk failed\n");
            sys_close(fd);
            sys_close(src);
            return -1;
        }
        sys_close(fd);

        char addr_s[24];
        char size_s[24];
        itoa_(addr, addr_s);
        itoa_(n, size_s);

        if (run_nandlogical(t->mtd_dev, "WRITE", addr_s, size_s, TMP_CHUNK) != 0) {
            puts_("Piko Install: nandlogical write failed\n");
            sys_unlink((long)TMP_CHUNK);
            sys_close(src);
            return -1;
        }

        sys_unlink((long)TMP_CHUNK);
        pos  += n;
        addr += n;
        puts_("Piko Install: wrote ");
        putnum(pos);
        puts_(" of ");
        putnum(datasize);
        puts_("\n");
    }

    sys_close(src);
    puts_("Piko Install: write pass complete for ");
    puts_(t->file);
    puts_(". Verifying...\n");

    src  = sys_open((long)t->file, O_RDONLY, 0);
    if (src < 0) {
        puts_("Piko Install: verify: cannot reopen file\n");
        return -1;
    }

    pos  = 0;
    addr = t->start_addr;
    int mismatch = 0;

    while (pos < datasize && !mismatch) {
        long n = sys_read(src, (long)chunkbuf, CHUNK_SIZE);
        if (n <= 0) {
            puts_("Piko Install: verify: read error\n");
            sys_close(src);
            return -1;
        }

        char addr_s[24];
        char size_s[24];
        itoa_(addr, addr_s);
        itoa_(n, size_s);

        sys_unlink((long)TMP_VERIFY);
        if (run_nandlogical(t->mtd_dev, "READ", addr_s, size_s, TMP_VERIFY) != 0) {
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
            puts_("Piko Install: verify: size mismatch\n");
            mismatch = 1;
            break;
        }

        long i;
        for (i = 0; i < n; i++) {
            if (chunkbuf[i] != verifybuf[i]) {
                puts_("Piko Install: verify: MISMATCH at file offset ");
                putnum(pos + i);
                puts_("\n");
                mismatch = 1;
                break;
            }
        }

        pos  += n;
        addr += n;
    }

    sys_close(src);

    if (mismatch == 1) {
        puts_("Piko Install: VERIFY FAILED for ");
        puts_(t->file);
        puts_("\n");
        return -1;
    }
    if (mismatch == -1) {
        puts_("Piko Install: verify unavailable for ");
        puts_(t->file);
        puts_(", write pass reported success.\n");
        return 0;
    }

    puts_("Piko Install: VERIFY OK for ");
    puts_(t->file);
    puts_("\n");

    diagnose_ftl_mapping(t->mtd_dev, t->start_addr, datasize);

    return 0;
}

/* Copies a small /proc file to stdout and the log, framed with markers. */
static void dump_proc_file(const char *path)
{
    char buf[512];
    long n;

    puts_("Piko Install: --- ");
    puts_(path);
    puts_(" begin ---\n");

    long fd = sys_open((long)path, O_RDONLY, 0);
    if (fd >= 0) {
        while ((n = sys_read(fd, (long)buf, sizeof(buf))) > 0) {
            sys_write(1, (long)buf, n);
            if (g_logfd >= 0) sys_write(g_logfd, (long)buf, n);
        }
        sys_close(fd);
    } else {
        puts_("Piko Install: cannot open ");
        puts_(path);
        puts_(" (absent on this kernel)\n");
    }

    puts_("Piko Install: --- ");
    puts_(path);
    puts_(" end ---\n");
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        puts_("Piko Install: usage: piko-install <datapath>\n");
        puts_("  datapath: directory on the SD card containing image files\n");
        puts_("  optional: piko.cfg in datapath overrides compile-time flash targets\n");
        _exit_(1);
    }
    const char *datapath = argv[1];

    sys_mkdir((long)"/tmp/update", 0755);

    if (sys_chdir((long)datapath) < 0) {
        puts_("Piko Install: cannot cd to datapath\n");
        _exit_(1);
    }

    g_logfd = sys_open((long)"piko-log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    /* Dump a few /proc files for reference.
     *
     * /proc/cpuinfo matters more than it looks: this recovery kernel was
     * started by the SAME bootloader that starts our bootstrap kernel, so
     * its "Hardware:" line names whichever machine_desc matched the
     * machine number the bootloader passes in r1. When our own kernel
     * reports that number matching nothing (2026-07-29 -- see
     * docs/DEADLETTER-LED-MARKERS.md), this is a free, independent
     * cross-check of what the bootloader really hands over.
     *
     * /proc/atags is only present if the recovery kernel was built with
     * CONFIG_ATAGS_PROC; it holds the raw ATAG list (including the boot
     * command line). Absent is fine -- it just gets reported as missing. */
    dump_proc_file("/proc/mtd");
    dump_proc_file("/proc/cpuinfo");
    dump_proc_file("/proc/meminfo");
    dump_proc_file("/proc/atags");

    /* Load runtime config; fall back to compile-time defaults */
    load_config();

    const struct flash_target *active_targets;
    int num_targets;
    if (cfg_count > 0) {
        puts_("Piko Install: using ");
        putnum(cfg_count);
        puts_(" target(s) from " CFG_FILE "\n");
        active_targets = cfg_targets;
        num_targets    = cfg_count;
    } else {
        puts_("Piko Install: no " CFG_FILE " found, using compile-time defaults\n");
        active_targets = default_targets;
        num_targets    = NUM_DEFAULT_TARGETS;
    }

    int failures = 0;
    int i;
    for (i = 0; i < num_targets; i++) {
        if (flash_one(&active_targets[i]) != 0) {
            failures++;
            /* Multi-target runs (e.g. mtd1 then mtd3 in one pass, see
             * docs/FLASH-MTD1-MTD3-SAFE.md) depend on later targets only
             * ever running after earlier ones are confirmed good -- do not
             * erase/write a further partition once an earlier one has
             * already failed. */
            puts_("Piko Install: target failed, aborting remaining targets.\n");
            break;
        }
    }

    if (failures) {
        puts_("Piko Install: DONE with ");
        putnum(failures);
        puts_(" failure(s). Check piko-log.txt.\n");
        _exit_(1);
    }

    puts_("Piko Install: ALL TARGETS FLASHED AND VERIFIED. Power off and reboot.\n");
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
