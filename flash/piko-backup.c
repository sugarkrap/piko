/* Freestanding ARM OABI Linux binary: no libc, no TLS, targets ancient
 * pre-EABI kernels (syscall number encoded in the swi immediate, not r7).
 *
 * Read-only backup tool: dumps mtd2 ("root") and mtd3 ("home") -- Cacko's
 * recovery-kernel partition numbering, same as piko-install always used --
 * to files on the SD card. Nothing here writes to NAND at all.
 *
 * nandlogical's LADDR read/write translation is only wired up for mtd1
 * ("smf") in this recovery kernel, confirmed earlier in this project
 * (MEMREADLADDR/MEMWRITELADDR return -EINVAL on mtd2/mtd3 at every offset
 * tried). Plain raw read() on the /dev/mtdN character device, however, is
 * the one raw-MTD code path that's actually proven reliable on this
 * hardware -- it's exactly what flash_one_raw()'s verify step in
 * piko-install-final.c already used successfully. Reading has none of the
 * erase/write granularity problems that made raw writes unreliable. */

#define OABI_BASE 0x900000

#define SYSCALL1(fname, nr) \
static long fname(long a0) { \
    register long r0 asm("r0") = a0; \
    asm volatile ("swi %1" : "+r"(r0) : "i"(OABI_BASE + (nr)) : "memory"); \
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

SYSCALL1(sys_exit,   1)
SYSCALL3(sys_read,   3)
SYSCALL3(sys_write,  4)
SYSCALL3(sys_open,   5)
SYSCALL1(sys_close,  6)
SYSCALL1(sys_chdir,  12)
SYSCALL3(sys_lseek,  19)

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000
#define SEEK_SET 0

static void _exit_(int code) { sys_exit(code); __builtin_unreachable(); }

int raise(int sig) { (void)sig; _exit_(139); return 0; }

typedef unsigned long size_t;

static size_t strlen_(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

static void puts_(const char *s)
{
    sys_write(1, (long)s, strlen_(s));
}

static void putnum(long v)
{
    char out[24];
    char tmp[24];
    int i = 0;
    int j = 0;
    unsigned long u = (unsigned long)v;
    if (u == 0) { out[j++] = '0'; out[j] = 0; puts_(out); return; }
    while (u > 0) { tmp[i++] = '0' + (u % 10); u /= 10; }
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
    puts_(out);
}

#define BACKUP_CHUNK 1048576
#define SKIP_SIZE    16384 /* NAND erase-block size on this hardware --
    granularity used to isolate and skip past a single bad block instead
    of losing the whole surrounding 1MB chunk to one read error. */
static char backupbuf[BACKUP_CHUNK];

static void zero_fill(char *buf, long n)
{
    long i;
    for (i = 0; i < n; i++) buf[i] = 0;
}

struct backup_target {
    const char *mtd_dev;
    const char *out_file;
    long size;
};

/* mtd1/mtd2/mtd3: Cacko recovery kernel's own numbering (same as always
 * used for flashing) -- "smf" (0x700000), "root" (0x3500000) and "home"
 * (0x4400000), matching this board's real /proc/mtd sizes. smf holds
 * Sharp's own partition-table metadata (sharpslpart), not just the
 * kernel -- needed for anything (like QEMU) that wants to parse root/home
 * as real sub-partitions rather than just raw dumps. */
static const struct backup_target targets[] = {
    { "/dev/mtd1", "smf-backup.bin",  0x700000 },
    { "/dev/mtd2", "root-backup.bin", 0x3500000 },
    { "/dev/mtd3", "home-backup.bin", 0x4400000 },
};
#define NUM_TARGETS 3

static int backup_one(const struct backup_target *t)
{
    puts_("Piko Backup: === reading ");
    puts_(t->mtd_dev);
    puts_(" to ");
    puts_(t->out_file);
    puts_(" ===\n");

    long mtd = sys_open((long)t->mtd_dev, O_RDONLY, 0);
    if (mtd < 0) {
        puts_("Piko Backup: cannot open ");
        puts_(t->mtd_dev);
        puts_("\n");
        return -1;
    }

    long out = sys_open((long)t->out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        puts_("Piko Backup: cannot open output file\n");
        sys_close(mtd);
        return -1;
    }

    long pos = 0;
    long bad_blocks = 0;
    while (pos < t->size) {
        long want = t->size - pos;
        if (want > BACKUP_CHUNK) want = BACKUP_CHUNK;

        long n = sys_read(mtd, (long)backupbuf, want);
        if (n < 0) {
            /* Drop to erase-block granularity to isolate exactly which
             * block(s) within this chunk are bad, instead of losing the
             * whole chunk. Zero-fill just the bad block(s) so the output
             * file stays aligned with the source partition's offsets. */
            puts_("Piko Backup: read error in chunk at ");
            putnum(pos);
            puts_(", retrying at block granularity\n");

            long subpos = pos;
            long subend = pos + want;
            while (subpos < subend) {
                long subwant = subend - subpos;
                if (subwant > SKIP_SIZE) subwant = SKIP_SIZE;

                if (sys_lseek(mtd, subpos, SEEK_SET) < 0) {
                    puts_("Piko Backup: lseek failed at ");
                    putnum(subpos);
                    puts_("\n");
                    sys_close(mtd);
                    sys_close(out);
                    return -1;
                }

                long sn = sys_read(mtd, (long)backupbuf, subwant);
                if (sn < 0) {
                    puts_("Piko Backup: bad block at ");
                    putnum(subpos);
                    puts_(", zero-filling and skipping\n");
                    zero_fill(backupbuf, subwant);
                    sn = subwant;
                    bad_blocks++;
                }
                if (sn == 0) {
                    puts_("Piko Backup: unexpected EOF at ");
                    putnum(subpos);
                    puts_("\n");
                    sys_close(mtd);
                    sys_close(out);
                    return -1;
                }

                long sw = sys_write(out, (long)backupbuf, sn);
                if (sw != sn) {
                    puts_("Piko Backup: short write to output file at ");
                    putnum(subpos);
                    puts_("\n");
                    sys_close(mtd);
                    sys_close(out);
                    return -1;
                }
                subpos += sn;
            }

            if (sys_lseek(mtd, subend, SEEK_SET) < 0) {
                puts_("Piko Backup: lseek failed at ");
                putnum(subend);
                puts_("\n");
                sys_close(mtd);
                sys_close(out);
                return -1;
            }
            pos = subend;
            puts_("Piko Backup: read ");
            putnum(pos);
            puts_(" of ");
            putnum(t->size);
            puts_("\n");
            continue;
        }
        if (n == 0) {
            puts_("Piko Backup: unexpected EOF at ");
            putnum(pos);
            puts_(" of ");
            putnum(t->size);
            puts_("\n");
            break;
        }

        long w = sys_write(out, (long)backupbuf, n);
        if (w != n) {
            puts_("Piko Backup: short write to output file at ");
            putnum(pos);
            puts_("\n");
            sys_close(mtd);
            sys_close(out);
            return -1;
        }

        pos += n;
        puts_("Piko Backup: read ");
        putnum(pos);
        puts_(" of ");
        putnum(t->size);
        puts_("\n");
    }

    sys_close(mtd);
    sys_close(out);

    puts_("Piko Backup: done, wrote ");
    putnum(pos);
    puts_(" bytes to ");
    puts_(t->out_file);
    puts_(" (");
    putnum(bad_blocks);
    puts_(" bad block(s) zero-filled)\n");
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        puts_("Piko Backup: usage: piko-backup <datapath>\n");
        _exit_(1);
    }
    const char *datapath = argv[1];

    if (sys_chdir((long)datapath) < 0) {
        puts_("Piko Backup: cannot cd to datapath\n");
        _exit_(1);
    }

    int failures = 0;
    int i;
    for (i = 0; i < NUM_TARGETS; i++) {
        if (backup_one(&targets[i]) != 0)
            failures++;
    }

    if (failures) {
        puts_("Piko Backup: DONE with ");
        putnum(failures);
        puts_(" failure(s).\n");
        _exit_(1);
    }

    puts_("Piko Backup: ALL PARTITIONS BACKED UP SUCCESSFULLY.\n");
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
