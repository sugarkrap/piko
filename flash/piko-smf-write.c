/*
 * piko-smf-write.c v3 - FTL-aware in-system SMF kernel slot writer
 *
 * The Sharp SL FTL stores logical->physical block mappings in the NAND OOB
 * (out-of-band) area.  For 512-byte pages with 16-byte OOB, the layout is:
 *
 *   bytes 0-3,6-7 : hardware ECC (written by NAND controller)
 *   bytes 4-5     : bad-block marker area
 *   bytes 8-13    : 3 copies of 16-bit logical block number (even parity)
 *   bytes 14-15   : unused (0xFF)
 *
 * The 16-bit FTL value: us = (logical_block_num << 1) | even_parity_bit
 * See kernel drivers/mtd/parsers/sharpslpart.c and drivers/mtd/nand/ecc.c
 * (nand_ooblayout_free_sp) for the canonical FTL format documentation.
 *
 * The PREVIOUS raw MEMERASE+write() approach wiped OOB entirely, so the
 * bootloader's FTL scan could no longer map logical -> physical, causing
 * cold-boot failure.  This version uses MEMWRITE with MTD_OPS_AUTO_OOB
 * which writes data AND free OOB bytes (8-15) together in one page program,
 * preserving the FTL mapping.
 *
 * Usage: piko-smf-write [--compare] <mtd_dev> <image> <laddr> <max_size> [backup_path]
 *        piko-smf-write --backup <mtd_dev> <outfile>
 *   laddr     : decimal logical start address (e.g. 917504 = 0xE0000)
 *   max_size  : maximum image size in bytes (e.g. 1294336)
 *   backup    : optional path to dump full MTD before writing
 *
 * --compare is READ-ONLY: it scans the FTL and compares the logical range
 * against <image> without erasing or writing anything. Exit status is the
 * whole interface:
 *   0 = flash already holds exactly this image (nothing to do)
 *   3 = flash differs (a write would be needed)
 *   1 = could not tell (I/O error, bad geometry, unmapped blocks)
 *
 * That 0-vs-3 distinction is what lets piko-update ship the bootstrap
 * image in every package and still touch NAND only on the rare release
 * that actually changes it -- see userspace/src/piko-update.c. Any answer
 * other than a confident 0 must be treated as "do not skip the write".
 */

#include <errno.h>
#include <fcntl.h>
#include <mtd/mtd-abi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Sharp SL FTL constants.
 * oobavail=8 for 512-byte pages + 16-byte OOB with 6-byte HW ECC:
 *   ECC at bytes 0-3,6-7; free at bytes 8-15.
 * (see nand_ooblayout_free_sp in drivers/mtd/nand/ecc.c)
 */
#define SHARPSL_FTL_OOBAVAIL    8   /* free OOB bytes per page */
#define SHARPSL_BLOCK_RESERVED  0xFFFF
#define SHARPSL_BLOCK_UNMASK_COMPLEMENT 1

#define COPY_BUFSZ 65536

/* Count set bits in a 16-bit value (popcount). */
static int popcount16(uint16_t v)
{
    int c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

/*
 * Encode a logical block number into the 16-bit Sharp FTL OOB value.
 * Format: bits 10:1 = block_num, bit 0 = even-parity bit.
 */
static uint16_t ftl_encode(unsigned int lognum)
{
    uint16_t us = (uint16_t)((lognum & 0x3FF) << 1);
    if (popcount16(us) & SHARPSL_BLOCK_UNMASK_COMPLEMENT)
        us |= 1;  /* set parity bit to achieve even parity */
    return us;
}

/*
 * Decode a 16-bit FTL OOB value to a logical block number.
 * Returns -1 if parity is wrong or value is reserved/erased.
 */
static int ftl_decode(uint16_t us)
{
    if (us == 0xFFFF)
        return -1;  /* erased / unmapped */
    if (us == SHARPSL_BLOCK_RESERVED)
        return -1;  /* wear-leveling reserve */
    if (popcount16(us) & SHARPSL_BLOCK_UNMASK_COMPLEMENT)
        return -1;  /* bad parity */
    return (int)((us >> 1) & 0x3FF);
}

/*
 * Write the FTL logical block number into an 8-byte free-OOB buffer
 * (the buffer passed to MEMWRITE AUTO_OOB, where byte 0 maps to actual
 * OOB byte 8, byte 1 -> OOB byte 9, etc.).
 * Stores 3 redundant copies of the 16-bit encoded value in bytes 0-5.
 */
static void ftl_set_oob(uint8_t *free_oob, unsigned int lognum)
{
    uint16_t us = ftl_encode(lognum);
    free_oob[0] = us & 0xFF;
    free_oob[1] = (us >> 8) & 0xFF;
    free_oob[2] = us & 0xFF;
    free_oob[3] = (us >> 8) & 0xFF;
    free_oob[4] = us & 0xFF;
    free_oob[5] = (us >> 8) & 0xFF;
    free_oob[6] = 0xFF;
    free_oob[7] = 0xFF;
}

/*
 * Read the logical block number assigned to a physical block by reading the
 * OOB of the block's first page.  Returns the logical block number, or -1
 * if the block is unmapped / bad / corrupt OOB.
 *
 * Uses MEMREADOOB (reads raw OOB bytes) and examines bytes 8-13.
 */
static int read_block_lognum(int fd, uint32_t phys_page_addr, uint32_t oobsize)
{
    uint8_t oob[16];
    struct mtd_oob_buf oob_op;
    uint16_t c0, c1, c2;

    if (oobsize > sizeof(oob))
        oobsize = sizeof(oob);

    memset(oob, 0xFF, sizeof(oob));
    oob_op.start  = phys_page_addr;
    oob_op.length = oobsize;
    oob_op.ptr    = oob;

    if (ioctl(fd, MEMREADOOB, &oob_op) < 0)
        return -1;

    /* Three 16-bit copies in OOB bytes 8-9, 10-11, 12-13 */
    c0 = (uint16_t)(oob[8]  | ((uint16_t)oob[9]  << 8));
    c1 = (uint16_t)(oob[10] | ((uint16_t)oob[11] << 8));
    c2 = (uint16_t)(oob[12] | ((uint16_t)oob[13] << 8));

    /* Pick first agreeing pair (majority voting from sharpslpart.c) */
    uint16_t us;
    if (c0 == c1)       us = c0;
    else if (c1 == c2)  us = c1;
    else if (c2 == c0)  us = c2;
    else return -1;     /* all three disagree - corrupt */

    return ftl_decode(us);
}

/*
 * Back up the entire MTD device to backup_path by sequential read.
 */
static int backup_mtd(const char *mtddev, const char *backup_path,
                      uint32_t total_size)
{
    int in_fd  = open(mtddev, O_RDONLY);
    int out_fd = -1;
    uint8_t *buf = NULL;
    int ret = -1;

    if (in_fd < 0) { perror("open mtd for backup"); return -1; }

    out_fd = open(backup_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) { perror("open backup output"); goto out; }

    buf = malloc(COPY_BUFSZ);
    if (!buf) { perror("malloc backup buf"); goto out; }

    uint32_t left = total_size;
    while (left > 0) {
        size_t chunk = left > COPY_BUFSZ ? COPY_BUFSZ : left;
        ssize_t n = read(in_fd, buf, chunk);
        if (n <= 0) { perror("read mtd backup"); goto out; }
        if (write(out_fd, buf, (size_t)n) != n) { perror("write backup"); goto out; }
        left -= (uint32_t)n;
    }
    fsync(out_fd);
    ret = 0;

out:
    free(buf);
    if (in_fd  >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);
    return ret;
}

/*
 * Re-read the MTD and the backup file side by side and require them to
 * match byte for byte.
 *
 * backup_mtd() above checks that every write() returned, which says the
 * bytes reached the JFFS2 page cache -- not that they reached flash, and
 * not that the file is the length it should be. On this board the backup
 * is the entire recovery story for a bad smf write (no serial, no USB,
 * last spare board -- AGENTS.md), so "the copy ran without erroring" is
 * not a strong enough claim to erase a bootstrap partition on.
 */
static int verify_backup(const char *mtddev, const char *backup_path,
                         uint32_t total_size)
{
    int in_fd  = open(mtddev, O_RDONLY);
    int bak_fd = open(backup_path, O_RDONLY);
    uint8_t *a = NULL, *b = NULL;
    struct stat st;
    int ret = -1;
    uint32_t off = 0;

    if (in_fd < 0)  { perror("open mtd for backup verify"); goto out; }
    if (bak_fd < 0) { perror("open backup for verify"); goto out; }

    if (fstat(bak_fd, &st) < 0) { perror("stat backup"); goto out; }
    if ((uint32_t)st.st_size != total_size) {
        fprintf(stderr, "backup verify FAILED: %s is %ld bytes, expected %u\n",
                backup_path, (long)st.st_size, total_size);
        goto out;
    }

    a = malloc(COPY_BUFSZ);
    b = malloc(COPY_BUFSZ);
    if (!a || !b) { perror("malloc verify bufs"); goto out; }

    while (off < total_size) {
        size_t chunk = (total_size - off) > COPY_BUFSZ ? COPY_BUFSZ : (total_size - off);
        ssize_t n1 = read(in_fd, a, chunk);
        ssize_t n2 = read(bak_fd, b, chunk);

        if (n1 <= 0 || n2 != n1) {
            fprintf(stderr, "backup verify FAILED: short read at offset %u\n", off);
            goto out;
        }
        if (memcmp(a, b, (size_t)n1) != 0) {
            fprintf(stderr, "backup verify FAILED: mismatch near offset %u\n", off);
            goto out;
        }
        off += (uint32_t)n1;
    }

    printf("backup verified: %s (%u bytes) matches %s\n",
           backup_path, total_size, mtddev);
    ret = 0;

out:
    free(a); free(b);
    if (in_fd  >= 0) close(in_fd);
    if (bak_fd >= 0) close(bak_fd);
    return ret;
}

/*
 * Scan every physical block's OOB and build the FTL logical->physical map.
 * log2phy[] gets UINT32_MAX for logical blocks with no physical home;
 * phy_free[] is set for physical blocks carrying no valid logical number
 * (erased, or wear-levelling spares) so the writer can allocate from them.
 *
 * Read-only. Shared by the write path and --compare specifically so the
 * two can never disagree about where a given logical block actually lives
 * -- a compare that read the wrong blocks would report "already correct"
 * and silently skip a write the board needed.
 */
static void ftl_scan(int fd, uint32_t total_blocks, uint32_t erasesize,
                     uint32_t oobsize, uint32_t *log2phy, uint8_t *phy_free)
{
    uint32_t mapped = 0, free_count = 0;

    for (uint32_t i = 0; i < total_blocks; i++)
        log2phy[i] = UINT32_MAX;

    for (uint32_t p = 0; p < total_blocks; p++) {
        loff_t baddr = (loff_t)(p * erasesize);

        int bad = ioctl(fd, MEMGETBADBLOCK, &baddr);
        if (bad > 0) continue;  /* skip bad blocks */

        int lognum = read_block_lognum(fd, p * erasesize, oobsize);
        if (lognum >= 0 && (uint32_t)lognum < total_blocks) {
            if (log2phy[lognum] == UINT32_MAX) {
                log2phy[lognum] = p;
                mapped++;
            }
        } else {
            phy_free[p] = 1;
            free_count++;
        }
    }
    printf("FTL scan: %u mapped, %u free/unallocated\n", mapped, free_count);
}

/*
 * --compare implementation. Returns 0 identical, 3 different, 1 undetermined.
 *
 * The tail of the last erase block is compared against 0xFF padding, which
 * is exactly what the write path lays down (Step 5 memsets to 0xFF before
 * copying the final partial chunk) and what its own Step 7 verify expects.
 * Keeping the two identical is the point -- otherwise a freshly written
 * image would compare as "different" forever and re-flash on every run.
 */
static int do_compare(int fd, const char *image_path, uint32_t laddr,
                      uint32_t image_size, uint32_t erasesize,
                      uint32_t oobsize, uint32_t total_blocks)
{
    uint32_t log_start  = laddr / erasesize;
    uint32_t num_blocks = (image_size + erasesize - 1) / erasesize;
    uint32_t *log2phy   = malloc(sizeof(uint32_t) * total_blocks);
    uint8_t  *phy_free  = calloc(total_blocks, 1);
    uint8_t  *src_buf   = malloc(erasesize);
    uint8_t  *dst_buf   = malloc(erasesize);
    int src_fd = -1;
    int rc = 1;
    uint32_t remaining = image_size;

    if (!log2phy || !phy_free || !src_buf || !dst_buf) {
        perror("malloc compare buffers");
        goto out;
    }

    src_fd = open(image_path, O_RDONLY);
    if (src_fd < 0) { perror("open image for compare"); goto out; }

    ftl_scan(fd, total_blocks, erasesize, oobsize, log2phy, phy_free);

    for (uint32_t i = 0; i < num_blocks; i++) {
        uint32_t L = log_start + i;
        uint32_t chunk = remaining < erasesize ? remaining : erasesize;
        uint32_t got = 0;
        off_t off;
        ssize_t rn;

        if (log2phy[L] == UINT32_MAX) {
            /* No physical block backs this logical one, so there is
             * nothing to compare against -- report "undetermined", never
             * "identical", so the caller writes rather than skipping. */
            printf("compare: logical %u is unmapped\n", L);
            goto out;
        }

        memset(src_buf, 0xFF, erasesize);
        memset(dst_buf, 0x00, erasesize);

        while (got < chunk) {
            ssize_t n = read(src_fd, src_buf + got, chunk - got);
            if (n <= 0) { perror("read image for compare"); goto out; }
            got += (uint32_t)n;
        }

        off = (off_t)log2phy[L] * (off_t)erasesize;
        rn = pread(fd, dst_buf, erasesize, off);
        if (rn < 0) { perror("pread mtd for compare"); goto out; }
        if ((uint32_t)rn != erasesize) {
            fprintf(stderr, "compare: short read at physical %u\n", log2phy[L]);
            goto out;
        }

        if (memcmp(src_buf, dst_buf, erasesize) != 0) {
            printf("compare: differs at logical %u (physical %u)\n", L, log2phy[L]);
            rc = 3;
            goto out;
        }

        remaining -= chunk;
    }

    printf("compare: flash already holds this exact image (%u bytes)\n", image_size);
    rc = 0;

out:
    if (src_fd >= 0) close(src_fd);
    free(log2phy); free(phy_free); free(src_buf); free(dst_buf);
    return rc;
}

int main(int argc, char **argv)
{
    int compare_only = 0;
    int argbase = 1;

    /* piko-smf-write --backup <mtddev> <outfile>
     * Dump the whole partition and prove the dump is readable and correct,
     * with no erase or write anywhere in the path. Split out from the
     * writer's own optional backup step so a caller can take the backup,
     * confirm it, and only then decide to commit -- see smf_commit() in
     * userspace/src/piko-update.c. */
    if (argc > 1 && strcmp(argv[1], "--backup") == 0) {
        struct mtd_info_user bmi;
        int bfd;

        if (argc != 4) {
            fprintf(stderr, "usage: %s --backup <mtddev> <outfile>\n", argv[0]);
            return 2;
        }
        bfd = open(argv[2], O_RDONLY);
        if (bfd < 0) { perror("open mtd"); return 1; }
        if (ioctl(bfd, MEMGETINFO, &bmi) < 0) {
            perror("MEMGETINFO"); close(bfd); return 1;
        }
        close(bfd);

        printf("backing up %s (%u bytes) to %s\n", argv[2], bmi.size, argv[3]);
        if (backup_mtd(argv[2], argv[3], bmi.size) != 0)
            return 1;
        if (verify_backup(argv[2], argv[3], bmi.size) != 0)
            return 1;
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--compare") == 0) {
        compare_only = 1;
        argbase = 2;
    }

    if (argc - argbase < 4 || argc - argbase > 5) {
        fprintf(stderr,
            "usage: %s [--compare] <mtddev> <image> <laddr> <max_size> [backup_file]\n"
            "  laddr    : decimal logical start address (e.g. 917504 = 0xE0000)\n"
            "  max_size : max image bytes (e.g. 1294336)\n"
            "  --compare: read-only; exit 0 = flash matches, 3 = differs, 1 = unknown\n",
            argv[0]);
        return 2;
    }

    const char *mtd_dev   = argv[argbase];
    const char *image_path = argv[argbase + 1];
    uint32_t laddr    = (uint32_t)strtoul(argv[argbase + 2], NULL, 0);
    uint32_t max_size = (uint32_t)strtoul(argv[argbase + 3], NULL, 0);
    const char *backup_path = (argc - argbase == 5) ? argv[argbase + 4] : NULL;

    /* Open MTD device and get geometry. --compare never needs write access,
     * so don't ask for it -- an O_RDONLY fd cannot erase this partition no
     * matter what goes wrong below. */
    int fd = open(mtd_dev, compare_only ? O_RDONLY : O_RDWR);
    if (fd < 0) { perror("open mtd"); return 1; }

    struct mtd_info_user mi;
    if (ioctl(fd, MEMGETINFO, &mi) < 0) { perror("MEMGETINFO"); close(fd); return 1; }

    uint32_t erasesize      = mi.erasesize;  /* 0x4000 = 16384 */
    uint32_t writesize      = mi.writesize;  /* 0x200  = 512   */
    uint32_t oobsize        = mi.oobsize;    /* 0x10   = 16    */
    uint32_t total_size     = mi.size;       /* 0x700000 = 7MB */
    uint32_t oobavail       = SHARPSL_FTL_OOBAVAIL;
    uint32_t pages_per_block = erasesize / writesize;  /* 32 */
    uint32_t total_blocks   = total_size / erasesize;  /* 448 */

    /* Get image size. */
    struct stat st;
    if (stat(image_path, &st) < 0) { perror("stat image"); close(fd); return 1; }
    uint32_t image_size = (uint32_t)st.st_size;

    if (image_size == 0) {
        fprintf(stderr, "error: image is empty\n"); close(fd); return 1;
    }
    if (image_size > max_size) {
        fprintf(stderr, "error: image %u > max %u\n", image_size, max_size);
        close(fd); return 1;
    }
    if (laddr % erasesize) {
        fprintf(stderr, "error: laddr 0x%x not block-aligned\n", laddr);
        close(fd); return 1;
    }

    uint32_t log_start  = laddr / erasesize;
    uint32_t num_blocks = (image_size + erasesize - 1) / erasesize;
    uint32_t log_end    = log_start + num_blocks;  /* exclusive */

    printf("mtd=%s size=0x%08x erase=0x%08x write=0x%08x oob=0x%02x\n",
           mtd_dev, total_size, erasesize, writesize, oobsize);
    printf("image=%s bytes=%u laddr=0x%08x max=%u blocks=%u..%u\n",
           image_path, image_size, laddr, max_size, log_start, log_end - 1);

    if (log_end > total_blocks) {
        fprintf(stderr, "error: write range exceeds MTD (%u blocks)\n", total_blocks);
        close(fd); return 1;
    }

    if (compare_only) {
        int rc = do_compare(fd, image_path, laddr, image_size,
                            erasesize, oobsize, total_blocks);
        close(fd);
        return rc;
    }

    /* ------------------------------------------------------------------ *
     * Step 1: Scan FTL to build logical->physical map.                   *
     * ------------------------------------------------------------------ */
    printf("scanning FTL (%u physical blocks)...\n", total_blocks);

    uint32_t *log2phy   = malloc(sizeof(uint32_t) * total_blocks);
    uint8_t  *phy_free  = calloc(total_blocks, 1);
    if (!log2phy || !phy_free) {
        perror("malloc FTL tables"); free(log2phy); free(phy_free);
        close(fd); return 1;
    }

    ftl_scan(fd, total_blocks, erasesize, oobsize, log2phy, phy_free);

    /* ------------------------------------------------------------------ *
     * Step 2: Choose physical blocks for each target logical block.       *
     * If a logical block is already mapped, reuse its physical block.     *
     * If unmapped (wiped by a previous bad write), find a free block.     *
     * ------------------------------------------------------------------ */
    uint32_t *target_phy = malloc(sizeof(uint32_t) * num_blocks);
    if (!target_phy) {
        perror("malloc target_phy"); free(log2phy); free(phy_free);
        close(fd); return 1;
    }

    uint32_t free_cursor = 0;
    for (uint32_t i = 0; i < num_blocks; i++) {
        uint32_t L = log_start + i;

        if (log2phy[L] != UINT32_MAX) {
            target_phy[i] = log2phy[L];
            /* remove from free pool so we don't double-assign */
            phy_free[log2phy[L]] = 0;
        } else {
            /* logical block unmapped - find any free physical block */
            uint32_t found = UINT32_MAX;
            while (free_cursor < total_blocks) {
                if (phy_free[free_cursor]) {
                    found = free_cursor;
                    phy_free[free_cursor] = 0;
                    free_cursor++;
                    break;
                }
                free_cursor++;
            }
            if (found == UINT32_MAX) {
                fprintf(stderr,
                    "error: no free physical block for logical %u\n", L);
                free(target_phy); free(log2phy); free(phy_free);
                close(fd); return 1;
            }
            target_phy[i] = found;
            printf("  logical %u: unmapped -> allocating physical block %u\n",
                   L, found);
        }
    }

    /* ------------------------------------------------------------------ *
     * Step 3: Optional backup of full MTD.                               *
     * ------------------------------------------------------------------ */
    if (backup_path) {
        printf("backing up full mtd to %s\n", backup_path);
        if (backup_mtd(mtd_dev, backup_path, total_size) != 0 ||
            verify_backup(mtd_dev, backup_path, total_size) != 0) {
            fprintf(stderr, "refusing to erase: backup could not be confirmed\n");
            free(target_phy); free(log2phy); free(phy_free);
            close(fd); return 1;
        }
    }

    /* ------------------------------------------------------------------ *
     * Step 4: Erase target physical blocks.                              *
     * ------------------------------------------------------------------ */
    printf("erasing %u blocks...\n", num_blocks);
    for (uint32_t i = 0; i < num_blocks; i++) {
        struct erase_info_user ei;
        ei.start  = target_phy[i] * erasesize;
        ei.length = erasesize;
        if (ioctl(fd, MEMERASE, &ei) < 0) {
            perror("MEMERASE");
            fprintf(stderr, "  erase failed at physical block %u (0x%08x)\n",
                    target_phy[i], ei.start);
            free(target_phy); free(log2phy); free(phy_free);
            close(fd); return 1;
        }
    }

    /* ------------------------------------------------------------------ *
     * Step 5: Open image and write blocks with FTL OOB via MEMWRITE.    *
     *                                                                    *
     * MEMWRITE with MTD_OPS_AUTO_OOB writes data pages AND the free OOB *
     * bytes (bytes 8-15) together in one NAND page-program operation.    *
     * The hardware ECC engine fills OOB bytes 0-3,6-7 automatically.    *
     *                                                                    *
     * Per-block OOB buffer layout (pages_per_block * oobavail bytes):   *
     *   bytes  0..7 : page 0 free-OOB (logical block num, 3 copies)     *
     *   bytes  8..15: page 1 free-OOB (0xFF - not used by FTL)          *
     *   ...                                                              *
     * ------------------------------------------------------------------ */
    printf("writing %u blocks with FTL OOB (MEMWRITE AUTO_OOB)...\n", num_blocks);

    int img_fd = open(image_path, O_RDONLY);
    if (img_fd < 0) {
        perror("open image"); free(target_phy); free(log2phy); free(phy_free);
        close(fd); return 1;
    }

    uint32_t oob_block_len = pages_per_block * oobavail;  /* 32 * 8 = 256 */
    uint8_t *data_buf = malloc(erasesize);
    uint8_t *oob_buf  = malloc(oob_block_len);
    if (!data_buf || !oob_buf) {
        perror("malloc write buffers");
        free(data_buf); free(oob_buf);
        free(target_phy); free(log2phy); free(phy_free);
        close(img_fd); close(fd); return 1;
    }

    uint32_t img_remaining = image_size;

    for (uint32_t i = 0; i < num_blocks; i++) {
        uint32_t L = log_start + i;
        uint32_t P = target_phy[i];

        /* Read up to one full block of image data, pad remainder with 0xFF. */
        uint32_t chunk = img_remaining < erasesize ? img_remaining : erasesize;
        memset(data_buf, 0xFF, erasesize);
        uint32_t got = 0;
        while (got < chunk) {
            ssize_t n = read(img_fd, data_buf + got, chunk - got);
            if (n <= 0) {
                perror("read image");
                free(data_buf); free(oob_buf);
                free(target_phy); free(log2phy); free(phy_free);
                close(img_fd); close(fd); return 1;
            }
            got += (uint32_t)n;
        }
        img_remaining -= chunk;

        /*
         * Build OOB buffer for this block.
         * Only page 0's OOB carries the logical block number; the FTL
         * (sharpslpart.c) only reads OOB of the first page per block.
         * All other page OOB bytes are 0xFF.
         */
        memset(oob_buf, 0xFF, oob_block_len);
        ftl_set_oob(oob_buf, L);  /* first page (bytes 0-7 of oob_buf) */

        struct mtd_write_req wr;
        memset(&wr, 0, sizeof(wr));
        wr.start    = (uint64_t)(P * erasesize);
        wr.len      = (uint64_t)erasesize;
        wr.ooblen   = (uint64_t)oob_block_len;
        wr.usr_data = (uint64_t)(uintptr_t)data_buf;
        wr.usr_oob  = (uint64_t)(uintptr_t)oob_buf;
        wr.mode     = MTD_OPS_AUTO_OOB;

        if (ioctl(fd, MEMWRITE, &wr) < 0) {
            perror("MEMWRITE");
            fprintf(stderr,
                "  write failed: logical %u -> physical %u (0x%08x)\n",
                L, P, P * erasesize);
            free(data_buf); free(oob_buf);
            free(target_phy); free(log2phy); free(phy_free);
            close(img_fd); close(fd); return 1;
        }
    }

    close(img_fd);
    free(data_buf);
    free(oob_buf);

    /* ------------------------------------------------------------------ *
     * Step 6: Verify FTL mapping by re-reading OOB.                     *
     * ------------------------------------------------------------------ */
    printf("verifying FTL OOB mapping...\n");
    int verify_ok = 1;
    for (uint32_t i = 0; i < num_blocks; i++) {
        uint32_t L = log_start + i;
        uint32_t P = target_phy[i];
        int got_lognum = read_block_lognum(fd, P * erasesize, oobsize);
        if (got_lognum != (int)L) {
            fprintf(stderr,
                "  FAIL: physical %u: expected logical %u, got %d\n",
                P, L, got_lognum);
            verify_ok = 0;
        }
    }

    /* ------------------------------------------------------------------ *
     * Step 7: Verify payload bytes by reading back written data blocks.  *
     * ------------------------------------------------------------------ */
    if (verify_ok) {
        printf("verifying data payload...\n");

        int src_fd = open(image_path, O_RDONLY);
        if (src_fd < 0) {
            perror("open image for verify");
            verify_ok = 0;
        } else {
            uint8_t *src_buf = malloc(erasesize);
            uint8_t *dst_buf = malloc(erasesize);
            if (!src_buf || !dst_buf) {
                perror("malloc verify buffers");
                free(src_buf);
                free(dst_buf);
                close(src_fd);
                verify_ok = 0;
            } else {
                uint32_t remaining = image_size;

                for (uint32_t i = 0; i < num_blocks && verify_ok; i++) {
                    uint32_t chunk = remaining < erasesize ? remaining : erasesize;
                    uint32_t P = target_phy[i];
                    off_t off = (off_t)P * (off_t)erasesize;

                    memset(src_buf, 0xFF, erasesize);
                    memset(dst_buf, 0x00, erasesize);

                    uint32_t got = 0;
                    while (got < chunk) {
                        ssize_t n = read(src_fd, src_buf + got, chunk - got);
                        if (n <= 0) {
                            perror("read image for verify");
                            verify_ok = 0;
                            break;
                        }
                        got += (uint32_t)n;
                    }
                    if (!verify_ok)
                        break;

                    ssize_t rn = pread(fd, dst_buf, erasesize, off);
                    if (rn < 0) {
                        perror("pread mtd for verify");
                        verify_ok = 0;
                        break;
                    }
                    if ((uint32_t)rn != erasesize) {
                        fprintf(stderr,
                                "  FAIL: short readback at physical %u: got %zd expected %u\n",
                                P, rn, erasesize);
                        verify_ok = 0;
                        break;
                    }

                    if (memcmp(src_buf, dst_buf, erasesize) != 0) {
                        uint32_t bad = 0;
                        while (bad < erasesize && src_buf[bad] == dst_buf[bad])
                            bad++;
                        fprintf(stderr,
                                "  FAIL: data mismatch at logical %u physical %u block_ofs 0x%04x\n",
                                log_start + i, P, bad);
                        verify_ok = 0;
                        break;
                    }

                    if (remaining >= chunk)
                        remaining -= chunk;
                    else
                        remaining = 0;
                }

                free(src_buf);
                free(dst_buf);
                close(src_fd);
            }
        }
    }

    free(target_phy);
    free(log2phy);
    free(phy_free);
    close(fd);

    if (!verify_ok) {
        fprintf(stderr, "verification FAILED (FTL mapping and/or payload)\n");
        return 1;
    }

    printf("SUCCESS: %u blocks at logical 0x%08x (physical + payload verified)\n",
           num_blocks, laddr);
    return 0;
}
