
#ifndef _MTD_SHARPSL_H
#define _MTD_SHARPSL_H

#include <linux/mtd/rawnand.h>
#include <linux/mtd/partitions.h>

struct sharpsl_nand_platform_data {
	struct nand_bbt_descr	*badblock_pattern;
	const struct mtd_ooblayout_ops *ecc_layout;
	struct mtd_partition	*partitions;
	unsigned int		nr_partitions;
	const char *const	*part_parsers;
};

#endif
