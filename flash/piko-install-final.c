/* Freestanding ARM OABI Linux binary: no libc, no TLS, targets ancient
 * pre-EABI kernels (syscall number encoded in the swi immediate, not r7).
 *
 * This is the project's own combined installer: flashes the bootstrap
 * kernel (mtd1/"smf"), the full-system JFFS2 image (mtd2/"root"), and an
 * empty JFFS2 image (mtd3/"home") in a single run, since there is no
 * shell available in Cacko's recovery mode to invoke separate steps. */

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

#define TMP_CHUNK    "/tmp/update/tmpdata.bin"
#define TMP_VERIFY   "/tmp/update/verify.bin"
#define TMP_LOG      "/tmp/update/nandlog.txt"
#define CHUNK_SIZE   524288

static char chunkbuf[CHUNK_SIZE];
static char verifybuf[CHUNK_SIZE];

struct flash_target {
    const char *mtd_dev;
    const char *file;
    long start_addr;
    long max_size;
    int raw; /* 1 = direct MTD char-device erase+write; 0 = nandlogical LADDR */
    int erase_only; /* 1 = just eraseall this mtd, skip write+verify entirely */
};

/* Back to Cacko's own conservative 1,294,336-byte kernel budget for mtd1.
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
 * mtd2 ("root", Cacko's original intact install) stays untouched --
 * root-backup.bin/home-backup.bin already confirm it's a genuine,
 * working Cacko rootfs, and the whole point of targeting "home" for the
 * stage-2 kexec payload instead of "root" is to never have to touch it.
 *
 * *** raw flag is per-partition, NEVER copy-paste it between mtd1 and
 * mtd2/mtd3 -- see docs/DEADLETTER-RAW-FLAG.md for the full incident
 * writeup. Short version: mtd1/smf ONLY understands Sharp's proprietary
 * nandlogical MEMWRITELADDR layer (raw=0 here, matching Cacko's own
 * kernel-flash.sh exactly). raw=1 routes to flash_one_raw(), which
 * shells out to eraseall+nandcp -- genuine Cacko tooling, but ONLY
 * wired up for mtd2/mtd3. Using raw=1 on mtd1 runs eraseall across the
 * WHOLE physical 7,340,032-byte smf partition (not just the ~1.3MB
 * kernel region any correct write ever touches), wiping ~34 of the ~43
 * redundant recovery-critical regions documented in
 * docs/DEADLETTER-MTD2-MTD3.md Part 2 that no normal-budget write has
 * ever come near -- while nandcp itself silently no-ops on mtd1 (it's
 * not wired up there), so nothing gets written back. This happened for
 * real on this exact board 2026-07-22: recovery mode stopped responding
 * to the normal SD-card "Update" trigger entirely (that trigger's own
 * bootstrap apparently lives in one of the wiped regions). Recovered
 * only via the D+M service/diag menu's whole-NAND restore from a
 * genuine factory systc760.dbk (this board is a physical SL-C760 twin,
 * confirmed by the recovery menu itself refusing a systc860.dbk sourced
 * from the original bricked SL-C860 -- get the model-correct image from
 * https://www.trisoft.de, per-model download pages). ***
 *
 * mtd3 ("home", recovery-kernel numbering -- this is /dev/mtdblock2 in
 * OUR kernel's own numbering, see initramfs/rootfs-minimal/init) uses
 * eraseall+nandcp (raw=1) -- correct for mtd3, per the above. Carries
 * the stage-2 kexec payload: kexec itself was crashing with SIGILL on
 * real hardware from VFP instructions baked into uClibc's/libgcc's
 * prebuilt code (memcpy/memmove, then six __gnu_Unwind_*_VFP libgcc
 * helpers actually reachable from kexec's own EHABI unwind dispatch) --
 * both fixed via strong-symbol overrides linked ahead of the static
 * archives, confirmed clean by disassembly. Never actually verified
 * end-to-end on hardware before the raw-flag incident above interrupted
 * testing.
 *
 * mtd3 is NOT in this run. A combined mtd1+mtd3 run (both in one
 * piko-install invocation, NUM_TARGETS=2) bricked the board a second
 * time on 2026-07-22, immediately after the raw-flag incident and its
 * D+M recovery -- root cause not confirmed, but that run stacked too
 * many never-independently-tested variables at once (fixed raw flag,
 * fixed kexec, a freshly D+M-restored board's reset FTL/bad-block
 * state, AND two different flash mechanisms back to back in one
 * process, all untested in combination). Recovered again via D+M +
 * SYSTC760.DBK (see docs/DEADLETTER-NAND-RECOVERY.md). Going forward:
 * one target per run, confirm real-hardware boot before adding the
 * next. mtd1 alone this time; mtd3 only after mtd1 is confirmed
 * booting cold.
 *
 * *** start_addr was WRONGLY left at 0 through several of tonight's
 * failed attempts. flash/kernel-flash.sh (Cacko's own genuine
 * installer, read line by line, not just referenced from memory) uses
 * ADDR=917504, unambiguously, as a plain shell variable -- ZERO room
 * for misreading it as anything else. nandlogical's "logical address"
 * is very likely NOT a fixed linear offset -- it depends on the
 * current bad-block/wear-leveling table, so offset 0 landing somewhere
 * bootable on an old, long-lived NAND state (this session's earlier
 * "successful" tests) does not mean it lands correctly on a freshly
 * D+M-restored, freshly Cacko-reinstalled board (every test tonight).
 * Cacko's bootloader almost certainly always looks for the kernel at
 * logical offset 917504; writing at 0 may only ever have "worked" by
 * accident on old NAND state. Fixed 2026-07-22, confirmed working on
 * real hardware -- this board now cold-boots our bootstrap kernel
 * again. See docs/DEADLETTER-MTD1-OFFSET.md.
 *
 * mtd3 (home) this run: full stage-2 service stack + WiFi/SSH deliverable.
 * Boot to a shell was already confirmed working; this adds: a real
 * busybox (module tools, network tools, user tools, mdev, [ -- the
 * previous busybox was nearly bare) with a bb_syscall.c shim (uClibc here
 * exports no syscall(), and __NR_* can resolve OABI-numbered same as the
 * kexec_load bug -- shim normalizes it, see docs/DEADLETTER-KEXEC-SYSCALL.md);
 * BusyBox init + /etc/inittab + /etc/init.d/rcS as the service stack (no
 * systemd -- far too heavy for 64MB/400MHz); users root/zaurus (ash) and
 * piko/piko (zsh, skeleton in /home/piko); kernel modules (PCMCIA +
 * hostap/Prism2 + lib80211 + g_ether) rebuilt fresh into /lib/modules
 * + depmod; wireless-tools 29 + wpa_supplicant 2.10 (WEXT driver,
 * WPA-PSK/TKIP + WEP, CONFIG_TLS=internal -- no EAP needed, watch for the
 * `ifdef` treats any defined value including "n" as true gotcha in its
 * Makefile); mdev.conf auto-modprobes hostap_cs on card insertion and
 * runs /etc/wifi-up.sh once wlan0 appears (wpa_supplicant + udhcpc).
 * mtd1 is NOT reflashed. See docs/DEADLETTER-STAGE2-INIT.md. *** */
/* mtd3 (home) this run: FIX. The "packet kernel won't kexec / boots old
 * bootstrap" symptom was a config regression, not a placement bug: the
 * stage-2 config had reverted to CONFIG_INITRAMFS_SOURCE=<bootstrap cpio>
 * + CMDLINE without root=/init (the no-initramfs + root=home + init=/init
 * settings were applied at build time but never saved back to
 * .config.stage2-full-featured-backup). So the packet zImage-full embedded
 * the bootstrap initramfs (~180KB -- THAT was the size jump, not PACKET,
 * which adds ~10KB) and ran the bootstrap /init (mount home + kexec) again
 * instead of the service stack. Rebuilt zImage-full with the correct model
 * (no embedded initramfs, root=/dev/mtdblock2, init=/init) + PACKET; now
 * 1.51MB, kexecs like the original working stage-2 kernel. Config saved
 * back this time. mtd1 keeps the slimmed -d diagnostic bootstrap (harmless;
 * it kexecs fine). */
/* Build-time flash profile selector:
 *   -DPIKO_FLASH_MTD1 : mtd1 kernel slot only (nandlogical, offset 917504)
 *   -DPIKO_FLASH_MTD3 : mtd3 home partition only (eraseall+nandcp)
 * If unspecified, default to mtd3 for safety with the current workflow. */
#if defined(PIKO_FLASH_MTD1)
static const struct flash_target targets[] = {
    { "/dev/mtd1", "zImage", 917504, 1294336, 0, 0 },
};
#elif defined(PIKO_FLASH_MTD3)
static const struct flash_target targets[] = {
    { "/dev/mtd3", "mtd3.jffs2", 0, 71303168, 1, 0 },
};
#else
static const struct flash_target targets[] = {
    { "/dev/mtd3", "mtd3.jffs2", 0, 71303168, 1, 0 },
};
#endif
#define NUM_TARGETS 1

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

/* genuine updater.sh (Cacko's own installer) never touches mtd2/mtd3 via
 * raw MEMERASE+write() at all: for anything with ISLOGICAL=0 (rootfs,
 * home) it shells out to two dedicated Sharp/Cacko recovery tools:
 *   /sbin/eraseall $TARGET_MTD                (bulk-erases the WHOLE
 *                                              partition once, up front)
 *   /sbin/nandcp -a $ADDR $CHUNK $TARGET_MTD  (writes one chunk, and
 *                                              itself handles bad-block-
 *                                              aware address translation)
 * nandcp reports back where it actually finished via a line containing
 * "mtd address START-END(...)" on stdout; updater.sh parses END out with
 * `fgrep "mtd address" | cut -d- -f2 | cut -d'(' -f1` and feeds it in as
 * the next chunk's -a address instead of computing offsets itself. Our
 * own hand-rolled MEMERASE+write() approach never got data to land past
 * the first eraseblock -- shelling out to the real tools instead. */
#define NANDCP_CHUNK 1048576 /* matches updater.sh's ONESIZE for root/home */
static char nandcpbuf[NANDCP_CHUNK];
/* Dedicated verify readback buffer sized for full 1MB chunks -- must NOT
 * reuse the old CHUNK_SIZE(524288)-sized verifybuf, since a sys_read() of
 * a full NANDCP_CHUNK into that undersized buffer overflows it. */
static char verify_raw_buf[NANDCP_CHUNK];
#define ERASE_SIZE 16384 /* NAND erase-block size on this hardware. A
    single large sys_read() spanning multiple erase blocks reports full
    success (correct byte count, no error) but silently fails to advance
    past the first block -- confirmed via piko-log.txt: nandcp itself
    reports a clean write (file size == copy size, bad block: 0) for the
    exact chunk that later verify-mismatches at file offset 16384, one
    erase block in. Read back in erase-block-sized sub-reads with an
    explicit lseek() before each, instead of one big multi-block read. */

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

static int flash_erase_only(const struct flash_target *t)
{
    puts_("Piko Install: === erase-only ");
    puts_(t->mtd_dev);
    puts_(" (writing parked for now) ===\n");
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
        puts_("Piko Install: file not found on card: ");
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
        chunk_lens[nchunks] = n;

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
        pos += n;
        addr = next_addr;
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

    puts_("Piko Install: === flashing ");
    puts_(t->file);
    puts_(" to ");
    puts_(t->mtd_dev);
    puts_(" ===\n");

    long src = sys_open((long)t->file, O_RDONLY, 0);
    if (src < 0) {
        puts_("Piko Install: file not found on card: ");
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

    long pos = 0;
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
        pos += n;
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

    src = sys_open((long)t->file, O_RDONLY, 0);
    if (src < 0) {
        puts_("Piko Install: verify: cannot reopen file\n");
        return -1;
    }

    pos = 0;
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

        pos += n;
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
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        puts_("Piko Install: usage: piko-install <datapath>\n");
        _exit_(1);
    }
    const char *datapath = argv[1];

    sys_mkdir((long)"/tmp/update", 0755);

    if (sys_chdir((long)datapath) < 0) {
        puts_("Piko Install: cannot cd to datapath\n");
        _exit_(1);
    }

    g_logfd = sys_open((long)"piko-log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

    {
        long fd = sys_open((long)"/proc/mtd", O_RDONLY, 0);
        puts_("Piko Install: --- /proc/mtd begin ---\n");
        if (fd >= 0) {
            char buf[512];
            long n;
            while ((n = sys_read(fd, (long)buf, sizeof(buf))) > 0) {
                sys_write(1, (long)buf, n);
                if (g_logfd >= 0) sys_write(g_logfd, (long)buf, n);
            }
            sys_close(fd);
        } else {
            puts_("Piko Install: cannot open /proc/mtd\n");
        }
        puts_("Piko Install: --- /proc/mtd end ---\n");
    }

    int failures = 0;
    int i;
    for (i = 0; i < NUM_TARGETS; i++) {
        if (flash_one(&targets[i]) != 0)
            failures++;
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
