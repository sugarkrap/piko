
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/sizes.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#define NAND_NOOB_LOGADDR_00		8
#define NAND_NOOB_LOGADDR_01		9
#define NAND_NOOB_LOGADDR_10		10
#define NAND_NOOB_LOGADDR_11		11
#define NAND_NOOB_LOGADDR_20		12
#define NAND_NOOB_LOGADDR_21		13

#define BLOCK_IS_RESERVED		0xffff
#define BLOCK_UNMASK_COMPLEMENT		1

#define SHARPSL_NAND_PARTS		3
#define SHARPSL_FTL_PART_SIZE		(7 * SZ_1M)
#define SHARPSL_PARTINFO1_LADDR		0x00060000
#define SHARPSL_PARTINFO2_LADDR		0x00064000

#define BOOT_MAGIC			0x424f4f54
#define FSRO_MAGIC			0x4653524f
#define FSRW_MAGIC			0x46535257

struct sharpsl_ftl {
	unsigned int logmax;
	unsigned int *log2phy;
};

static int sharpsl_nand_check_ooblayout(struct mtd_info *mtd)
{
	u8 freebytes = 0;
	int section = 0;

	while (true) {
		struct mtd_oob_region oobfree = { };
		int ret, i;

		ret = mtd_ooblayout_free(mtd, section++, &oobfree);
		if (ret)
			break;

		if (!oobfree.length || oobfree.offset > 15 ||
		    (oobfree.offset + oobfree.length) < 8)
			continue;

		i = oobfree.offset >= 8 ? oobfree.offset : 8;
		for (; i < oobfree.offset + oobfree.length && i < 16; i++)
			freebytes |= BIT(i - 8);

		if (freebytes == 0xff)
			return 0;
	}

	return -ENOTSUPP;
}

static int sharpsl_nand_read_oob(struct mtd_info *mtd, loff_t offs, u8 *buf)
{
	struct mtd_oob_ops ops = { };
	int ret;

	ops.mode = MTD_OPS_PLACE_OOB;
	ops.ooblen = mtd->oobsize;
	ops.oobbuf = buf;

	ret = mtd_read_oob(mtd, offs, &ops);
	if (ret != 0 || mtd->oobsize != ops.oobretlen)
		return -1;

	return 0;
}

static int sharpsl_nand_get_logical_num(u8 *oob)
{
	u16 us;
	int good0, good1;

	if (oob[NAND_NOOB_LOGADDR_00] == oob[NAND_NOOB_LOGADDR_10] &&
	    oob[NAND_NOOB_LOGADDR_01] == oob[NAND_NOOB_LOGADDR_11]) {
		good0 = NAND_NOOB_LOGADDR_00;
		good1 = NAND_NOOB_LOGADDR_01;
	} else if (oob[NAND_NOOB_LOGADDR_10] == oob[NAND_NOOB_LOGADDR_20] &&
		   oob[NAND_NOOB_LOGADDR_11] == oob[NAND_NOOB_LOGADDR_21]) {
		good0 = NAND_NOOB_LOGADDR_10;
		good1 = NAND_NOOB_LOGADDR_11;
	} else if (oob[NAND_NOOB_LOGADDR_20] == oob[NAND_NOOB_LOGADDR_00] &&
		   oob[NAND_NOOB_LOGADDR_21] == oob[NAND_NOOB_LOGADDR_01]) {
		good0 = NAND_NOOB_LOGADDR_20;
		good1 = NAND_NOOB_LOGADDR_21;
	} else {
		return -EINVAL;
	}

	us = oob[good0] | oob[good1] << 8;

	if (hweight16(us) & BLOCK_UNMASK_COMPLEMENT)
		return -EINVAL;

	if (us == BLOCK_IS_RESERVED)
		return BLOCK_IS_RESERVED;

	return (us >> 1) & GENMASK(9, 0);
}

static int sharpsl_nand_init_ftl(struct mtd_info *mtd, struct sharpsl_ftl *ftl)
{
	unsigned int block_num, phymax;
	int i, ret, log_num;
	loff_t block_adr;
	u8 *oob;

	oob = kzalloc(mtd->oobsize, GFP_KERNEL);
	if (!oob)
		return -ENOMEM;

	phymax = mtd_div_by_eb(SHARPSL_FTL_PART_SIZE, mtd);

	ftl->logmax = ((phymax * 95) / 100) - 1;

	ftl->log2phy = kmalloc_array(ftl->logmax, sizeof(*ftl->log2phy),
				     GFP_KERNEL);
	if (!ftl->log2phy) {
		ret = -ENOMEM;
		goto exit;
	}

	for (i = 0; i < ftl->logmax; i++)
		ftl->log2phy[i] = UINT_MAX;

	for (block_num = 0; block_num < phymax; block_num++) {
		block_adr = (loff_t)block_num * mtd->erasesize;

		if (mtd_block_isbad(mtd, block_adr))
			continue;

		if (sharpsl_nand_read_oob(mtd, block_adr, oob))
			continue;

		log_num = sharpsl_nand_get_logical_num(oob);

		if (log_num > 0 && log_num < ftl->logmax) {
			if (ftl->log2phy[log_num] == UINT_MAX)
				ftl->log2phy[log_num] = block_num;
		}
	}

	pr_info("Sharp SL FTL: %d blocks used (%d logical, %d reserved)\n",
		phymax, ftl->logmax, phymax - ftl->logmax);

	ret = 0;
exit:
	kfree(oob);
	return ret;
}

static void sharpsl_nand_cleanup_ftl(struct sharpsl_ftl *ftl)
{
	kfree(ftl->log2phy);
}

static int sharpsl_nand_read_laddr(struct mtd_info *mtd,
				   loff_t from,
				   size_t len,
				   void *buf,
				   struct sharpsl_ftl *ftl)
{
	unsigned int log_num, final_log_num;
	unsigned int block_num;
	loff_t block_adr;
	loff_t block_ofs;
	size_t retlen;
	int err;

	log_num = mtd_div_by_eb((u32)from, mtd);
	final_log_num = mtd_div_by_eb(((u32)from + len - 1), mtd);

	if (len <= 0 || log_num >= ftl->logmax || final_log_num > log_num)
		return -EINVAL;

	block_num = ftl->log2phy[log_num];
	block_adr = (loff_t)block_num * mtd->erasesize;
	block_ofs = mtd_mod_by_eb((u32)from, mtd);

	err = mtd_read(mtd, block_adr + block_ofs, len, &retlen, buf);
	if (mtd_is_bitflip(err))
		err = 0;

	if (!err && retlen != len)
		err = -EIO;

	if (err)
		pr_err("sharpslpart: error, read failed at %#llx\n",
		       block_adr + block_ofs);

	return err;
}

struct sharpsl_nand_partinfo {
	__le32 start;
	__le32 end;
	__be32 magic;
	u32 reserved;
};

static int sharpsl_nand_read_partinfo(struct mtd_info *master,
				      loff_t from,
				      size_t len,
				      struct sharpsl_nand_partinfo *buf,
				      struct sharpsl_ftl *ftl)
{
	int ret;

	ret = sharpsl_nand_read_laddr(master, from, len, buf, ftl);
	if (ret)
		return ret;

	if (be32_to_cpu(buf[0].magic) != BOOT_MAGIC ||
	    be32_to_cpu(buf[1].magic) != FSRO_MAGIC ||
	    be32_to_cpu(buf[2].magic) != FSRW_MAGIC) {
		pr_err("sharpslpart: magic values mismatch\n");
		return -EINVAL;
	}

	buf[2].end = cpu_to_le32(master->size);

	if (le32_to_cpu(buf[0].end) <= le32_to_cpu(buf[0].start) ||
	    le32_to_cpu(buf[1].start) < le32_to_cpu(buf[0].end) ||
	    le32_to_cpu(buf[1].end) <= le32_to_cpu(buf[1].start) ||
	    le32_to_cpu(buf[2].start) < le32_to_cpu(buf[1].end) ||
	    le32_to_cpu(buf[2].end) <= le32_to_cpu(buf[2].start)) {
		pr_err("sharpslpart: partition sizes mismatch\n");
		return -EINVAL;
	}

	return 0;
}

static int sharpsl_parse_mtd_partitions(struct mtd_info *master,
					const struct mtd_partition **pparts,
					struct mtd_part_parser_data *data)
{
	struct sharpsl_ftl ftl;
	struct sharpsl_nand_partinfo buf[SHARPSL_NAND_PARTS];
	struct mtd_partition *sharpsl_nand_parts;
	int err;

	err = sharpsl_nand_check_ooblayout(master);
	if (err)
		return err;

	err = sharpsl_nand_init_ftl(master, &ftl);
	if (err)
		return err;

	pr_info("sharpslpart: try reading first partition table\n");
	err = sharpsl_nand_read_partinfo(master,
					 SHARPSL_PARTINFO1_LADDR,
					 sizeof(buf), buf, &ftl);
	if (err) {
		pr_warn("sharpslpart: first partition table is invalid, retry using the second\n");
		err = sharpsl_nand_read_partinfo(master,
						 SHARPSL_PARTINFO2_LADDR,
						 sizeof(buf), buf, &ftl);
	}

	sharpsl_nand_cleanup_ftl(&ftl);

	if (err) {
		pr_err("sharpslpart: both partition tables are invalid\n");
		return err;
	}

	sharpsl_nand_parts = kzalloc_objs(*sharpsl_nand_parts,
					  SHARPSL_NAND_PARTS);
	if (!sharpsl_nand_parts)
		return -ENOMEM;

	sharpsl_nand_parts[0].name = "smf";
	sharpsl_nand_parts[0].offset = le32_to_cpu(buf[0].start);
	sharpsl_nand_parts[0].size = le32_to_cpu(buf[0].end) -
				     le32_to_cpu(buf[0].start);

	sharpsl_nand_parts[1].name = "root";
	sharpsl_nand_parts[1].offset = le32_to_cpu(buf[1].start);
	sharpsl_nand_parts[1].size = le32_to_cpu(buf[1].end) -
				     le32_to_cpu(buf[1].start);

	sharpsl_nand_parts[2].name = "home";
	sharpsl_nand_parts[2].offset = le32_to_cpu(buf[2].start);
	sharpsl_nand_parts[2].size = le32_to_cpu(buf[2].end) -
				     le32_to_cpu(buf[2].start);

	*pparts = sharpsl_nand_parts;
	return SHARPSL_NAND_PARTS;
}

static struct mtd_part_parser sharpsl_mtd_parser = {
	.parse_fn = sharpsl_parse_mtd_partitions,
	.name = "sharpslpart",
};
module_mtd_part_parser(sharpsl_mtd_parser);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Andrea Adami <andrea.adami@gmail.com>");
MODULE_DESCRIPTION("MTD partitioning for NAND flash on Sharp SL Series");
