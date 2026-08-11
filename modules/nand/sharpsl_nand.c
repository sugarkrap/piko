
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/rawnand.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/sharpsl.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/io.h>

struct sharpsl_nand {
	struct nand_controller	controller;
	struct nand_chip	chip;

	void __iomem		*io;
};

static inline struct sharpsl_nand *mtd_to_sharpsl(struct mtd_info *mtd)
{
	return container_of(mtd_to_nand(mtd), struct sharpsl_nand, chip);
}

#define ECCLPLB		0x00
#define ECCLPUB		0x04
#define ECCCP		0x08
#define ECCCNTR		0x0C
#define ECCCLRR		0x10
#define FLASHIO		0x14
#define FLASHCTL	0x18

#define FLRYBY		(1 << 5)
#define FLCE1		(1 << 4)
#define FLWP		(1 << 3)
#define FLALE		(1 << 2)
#define FLCLE		(1 << 1)
#define FLCE0		(1 << 0)

static void sharpsl_nand_hwcontrol(struct nand_chip *chip, int cmd,
				   unsigned int ctrl)
{
	struct sharpsl_nand *sharpsl = mtd_to_sharpsl(nand_to_mtd(chip));

	if (ctrl & NAND_CTRL_CHANGE) {
		unsigned char bits = ctrl & 0x07;

		bits |= (ctrl & 0x01) << 4;

		bits ^= 0x11;

		writeb((readb(sharpsl->io + FLASHCTL) & ~0x17) | bits, sharpsl->io + FLASHCTL);
	}

	if (cmd != NAND_CMD_NONE)
		writeb(cmd, chip->legacy.IO_ADDR_W);
}

static int sharpsl_nand_dev_ready(struct nand_chip *chip)
{
	struct sharpsl_nand *sharpsl = mtd_to_sharpsl(nand_to_mtd(chip));
	return !((readb(sharpsl->io + FLASHCTL) & FLRYBY) == 0);
}

static void sharpsl_nand_enable_hwecc(struct nand_chip *chip, int mode)
{
	struct sharpsl_nand *sharpsl = mtd_to_sharpsl(nand_to_mtd(chip));
	writeb(0, sharpsl->io + ECCCLRR);
}

static int sharpsl_nand_calculate_ecc(struct nand_chip *chip,
				      const u_char * dat, u_char * ecc_code)
{
	struct sharpsl_nand *sharpsl = mtd_to_sharpsl(nand_to_mtd(chip));
	ecc_code[0] = ~readb(sharpsl->io + ECCLPUB);
	ecc_code[1] = ~readb(sharpsl->io + ECCLPLB);
	ecc_code[2] = (~readb(sharpsl->io + ECCCP) << 2) | 0x03;
	return readb(sharpsl->io + ECCCNTR) != 0;
}

static int sharpsl_attach_chip(struct nand_chip *chip)
{
	if (chip->ecc.engine_type != NAND_ECC_ENGINE_TYPE_ON_HOST)
		return 0;

	chip->ecc.size = 256;
	chip->ecc.bytes = 3;
	chip->ecc.strength = 1;
	chip->ecc.hwctl = sharpsl_nand_enable_hwecc;
	chip->ecc.calculate = sharpsl_nand_calculate_ecc;
	chip->ecc.correct = rawnand_sw_hamming_correct;

	chip->ecc.options |= NAND_ECC_GENERIC_ERASED_CHECK;

	return 0;
}

static const struct nand_controller_ops sharpsl_ops = {
	.attach_chip = sharpsl_attach_chip,
};

static int sharpsl_nand_probe(struct platform_device *pdev)
{
	struct nand_chip *this;
	struct mtd_info *mtd;
	struct resource *r;
	int err = 0;
	struct sharpsl_nand *sharpsl;
	struct sharpsl_nand_platform_data *data = dev_get_platdata(&pdev->dev);

	if (!data) {
		dev_err(&pdev->dev, "no platform data!\n");
		return -EINVAL;
	}

	sharpsl = kzalloc_obj(struct sharpsl_nand);
	if (!sharpsl)
		return -ENOMEM;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r) {
		dev_err(&pdev->dev, "no io memory resource defined!\n");
		err = -ENODEV;
		goto err_get_res;
	}

	sharpsl->io = ioremap(r->start, resource_size(r));
	if (!sharpsl->io) {
		dev_err(&pdev->dev, "ioremap to access Sharp SL NAND chip failed\n");
		err = -EIO;
		goto err_ioremap;
	}

	this = (struct nand_chip *)(&sharpsl->chip);

	nand_controller_init(&sharpsl->controller);
	sharpsl->controller.ops = &sharpsl_ops;
	this->controller = &sharpsl->controller;

	mtd = nand_to_mtd(this);
	mtd->dev.parent = &pdev->dev;
	mtd_set_ooblayout(mtd, data->ecc_layout);

	platform_set_drvdata(pdev, sharpsl);

	writeb(readb(sharpsl->io + FLASHCTL) | FLWP, sharpsl->io + FLASHCTL);

	this->legacy.IO_ADDR_R = sharpsl->io + FLASHIO;
	this->legacy.IO_ADDR_W = sharpsl->io + FLASHIO;
	this->legacy.cmd_ctrl = sharpsl_nand_hwcontrol;
	this->legacy.dev_ready = sharpsl_nand_dev_ready;
	this->legacy.chip_delay = 15;
	this->badblock_pattern = data->badblock_pattern;
	this->ecc.engine_type = NAND_ECC_ENGINE_TYPE_ON_HOST;

	err = nand_scan(this, 1);
	if (err)
		goto err_scan;

	mtd->name = "sharpsl-nand";

	err = mtd_device_parse_register(mtd, data->part_parsers, NULL,
					data->partitions, data->nr_partitions);
	if (err)
		goto err_add;

	return 0;

err_add:
	nand_cleanup(this);

err_scan:
	iounmap(sharpsl->io);
err_ioremap:
err_get_res:
	kfree(sharpsl);
	return err;
}

static void sharpsl_nand_remove(struct platform_device *pdev)
{
	struct sharpsl_nand *sharpsl = platform_get_drvdata(pdev);
	struct nand_chip *chip = &sharpsl->chip;
	int ret;

	ret = mtd_device_unregister(nand_to_mtd(chip));
	WARN_ON(ret);

	nand_cleanup(chip);

	iounmap(sharpsl->io);

	kfree(sharpsl);
}

static struct platform_driver sharpsl_nand_driver = {
	.driver = {
		.name	= "sharpsl-nand",
	},
	.probe		= sharpsl_nand_probe,
	.remove		= sharpsl_nand_remove,
};

module_platform_driver(sharpsl_nand_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard Purdie <rpurdie@rpsys.net>");
MODULE_DESCRIPTION("Device specific logic for NAND flash on Sharp SL-C7xx Series");
