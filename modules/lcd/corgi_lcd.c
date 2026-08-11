
#include <linux/backlight.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/lcd.h>
#include <linux/spi/spi.h>
#include <linux/spi/corgi_lcd.h>
#include <linux/slab.h>
#include <asm/mach/sharpsl_param.h>

#define POWER_IS_ON(pwr)	((pwr) <= LCD_POWER_REDUCED)

#define RESCTL_ADRS     0x00
#define PHACTRL_ADRS    0x01
#define DUTYCTRL_ADRS   0x02
#define POWERREG0_ADRS  0x03
#define POWERREG1_ADRS  0x04
#define GPOR3_ADRS      0x05
#define PICTRL_ADRS     0x06
#define POLCTRL_ADRS    0x07

#define RESCTL_QVGA     0x01
#define RESCTL_VGA      0x00

#define POWER1_VW_ON    0x01
#define POWER1_GVSS_ON  0x02
#define POWER1_VDD_ON   0x04

#define POWER1_VW_OFF   0x00
#define POWER1_GVSS_OFF 0x00
#define POWER1_VDD_OFF  0x00

#define POWER0_COM_DCLK 0x01
#define POWER0_COM_DOUT 0x02
#define POWER0_DAC_ON   0x04
#define POWER0_COM_ON   0x08
#define POWER0_VCC5_ON  0x10

#define POWER0_DAC_OFF  0x00
#define POWER0_COM_OFF  0x00
#define POWER0_VCC5_OFF 0x00

#define PICTRL_INIT_STATE      0x01
#define PICTRL_INIOFF          0x02
#define PICTRL_POWER_DOWN      0x04
#define PICTRL_COM_SIGNAL_OFF  0x08
#define PICTRL_DAC_SIGNAL_OFF  0x10

#define POLCTRL_SYNC_POL_FALL  0x01
#define POLCTRL_EN_POL_FALL    0x02
#define POLCTRL_DATA_POL_FALL  0x04
#define POLCTRL_SYNC_ACT_H     0x08
#define POLCTRL_EN_ACT_L       0x10

#define POLCTRL_SYNC_POL_RISE  0x00
#define POLCTRL_EN_POL_RISE    0x00
#define POLCTRL_DATA_POL_RISE  0x00
#define POLCTRL_SYNC_ACT_L     0x00
#define POLCTRL_EN_ACT_H       0x00

#define PHACTRL_PHASE_MANUAL   0x01
#define DEFAULT_PHAD_QVGA     (9)
#define DEFAULT_COMADJ        (125)

struct corgi_lcd {
	struct spi_device	*spi_dev;
	struct lcd_device	*lcd_dev;
	struct backlight_device	*bl_dev;

	int	limit_mask;
	int	intensity;
	int	power;
	int	mode;
	char	buf[2];

	struct gpio_desc *backlight_on;
	struct gpio_desc *backlight_cont;

	void (*kick_battery)(void);
};

static int corgi_ssp_lcdtg_send(struct corgi_lcd *lcd, int reg, uint8_t val);

static struct corgi_lcd *the_corgi_lcd;
static unsigned long corgibl_flags;
#define CORGIBL_SUSPENDED     0x01
#define CORGIBL_BATTLOW       0x02

static int comadj_override = -1;
static int phadadj_override = -1;

static bool assume_powered;
module_param(assume_powered, bool, 0444);
MODULE_PARM_DESC(assume_powered,
	"panel is already lit by an earlier kernel: adopt it at probe instead of re-running the power-on sequence");

static void corgi_lcd_power_on(struct corgi_lcd *lcd);

static int corgi_lcd_comadj(const char **src)
{
	int param = (int)sharpsl_param.comadj;

	if (comadj_override >= 0) {
		if (src)
			*src = "module param override";
		return comadj_override;
	}
	if (param >= 0) {
		if (src)
			*src = "sharpsl_param ('CMAD' present)";
		return param;
	}
	if (src)
		*src = "built-in default -- param block MISSING";
	return DEFAULT_COMADJ;
}

#define DEFAULT_PHAD_VGA      (6)

static int corgi_lcd_phadadj(void)
{
	int param = (int)sharpsl_param.phadadj;

	if (phadadj_override >= 0)
		return phadadj_override;
	if (param >= 0)
		return param;

	return DEFAULT_PHAD_VGA;
}

static int corgi_lcd_reapply(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_int(val, kp);

	if (ret)
		return ret;

	if (the_corgi_lcd && POWER_IS_ON(the_corgi_lcd->power))
		corgi_lcd_power_on(the_corgi_lcd);

	return 0;
}

static const struct kernel_param_ops corgi_lcd_reapply_ops = {
	.set = corgi_lcd_reapply,
	.get = param_get_int,
};

module_param_cb(comadj, &corgi_lcd_reapply_ops, &comadj_override, 0644);
MODULE_PARM_DESC(comadj,
	"panel VCOM DC bias 0-255; -1 = sharpsl_param, else default 125. Tune to kill vertical crosstalk.");

module_param_cb(phadadj, &corgi_lcd_reapply_ops, &phadadj_override, 0644);
MODULE_PARM_DESC(phadadj,
	"VGA phase adjust 0-15; -1 = sharpsl_param, else manual-phase only.");

static void lcdtg_ssp_i2c_send(struct corgi_lcd *lcd, uint8_t data)
{
	corgi_ssp_lcdtg_send(lcd, POWERREG0_ADRS, data);
	udelay(10);
}

static void lcdtg_i2c_send_bit(struct corgi_lcd *lcd, uint8_t data)
{
	lcdtg_ssp_i2c_send(lcd, data);
	lcdtg_ssp_i2c_send(lcd, data | POWER0_COM_DCLK);
	lcdtg_ssp_i2c_send(lcd, data);
}

static void lcdtg_i2c_send_start(struct corgi_lcd *lcd, uint8_t base)
{
	lcdtg_ssp_i2c_send(lcd, base | POWER0_COM_DCLK | POWER0_COM_DOUT);
	lcdtg_ssp_i2c_send(lcd, base | POWER0_COM_DCLK);
	lcdtg_ssp_i2c_send(lcd, base);
}

static void lcdtg_i2c_send_stop(struct corgi_lcd *lcd, uint8_t base)
{
	lcdtg_ssp_i2c_send(lcd, base);
	lcdtg_ssp_i2c_send(lcd, base | POWER0_COM_DCLK);
	lcdtg_ssp_i2c_send(lcd, base | POWER0_COM_DCLK | POWER0_COM_DOUT);
}

static void lcdtg_i2c_send_byte(struct corgi_lcd *lcd,
				uint8_t base, uint8_t data)
{
	int i;

	for (i = 0; i < 8; i++) {
		if (data & 0x80)
			lcdtg_i2c_send_bit(lcd, base | POWER0_COM_DOUT);
		else
			lcdtg_i2c_send_bit(lcd, base);
		data <<= 1;
	}
}

static void lcdtg_i2c_wait_ack(struct corgi_lcd *lcd, uint8_t base)
{
	lcdtg_i2c_send_bit(lcd, base);
}

static void lcdtg_set_common_voltage(struct corgi_lcd *lcd,
				     uint8_t base_data, uint8_t data)
{
	lcdtg_i2c_send_start(lcd, base_data);
	lcdtg_i2c_send_byte(lcd, base_data, 0x9c);
	lcdtg_i2c_wait_ack(lcd, base_data);
	lcdtg_i2c_send_byte(lcd, base_data, 0x00);
	lcdtg_i2c_wait_ack(lcd, base_data);
	lcdtg_i2c_send_byte(lcd, base_data, data);
	lcdtg_i2c_wait_ack(lcd, base_data);
	lcdtg_i2c_send_stop(lcd, base_data);
}

static int corgi_ssp_lcdtg_send(struct corgi_lcd *lcd, int adrs, uint8_t data)
{
	struct spi_message msg;
	struct spi_transfer xfer = {
		.len		= 1,
		.cs_change	= 0,
		.tx_buf		= lcd->buf,
	};

	lcd->buf[0] = ((adrs & 0x07) << 5) | (data & 0x1f);
	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	return spi_sync(lcd->spi_dev, &msg);
}

static void lcdtg_set_phadadj(struct corgi_lcd *lcd, int mode)
{
	int adj;

	switch (mode) {
	case CORGI_LCD_MODE_VGA:
		adj = corgi_lcd_phadadj();
		adj = (adj < 0) ? PHACTRL_PHASE_MANUAL :
				  PHACTRL_PHASE_MANUAL | ((adj & 0xf) << 1);
		break;
	case CORGI_LCD_MODE_QVGA:
	default:
		adj = (DEFAULT_PHAD_QVGA << 1) | PHACTRL_PHASE_MANUAL;
		break;
	}

	corgi_ssp_lcdtg_send(lcd, PHACTRL_ADRS, adj);
}

static void corgi_lcd_power_on(struct corgi_lcd *lcd)
{
	int comadj;

	corgi_ssp_lcdtg_send(lcd, PICTRL_ADRS,
			PICTRL_POWER_DOWN | PICTRL_INIOFF |
			PICTRL_INIT_STATE | PICTRL_COM_SIGNAL_OFF |
			PICTRL_DAC_SIGNAL_OFF);

	corgi_ssp_lcdtg_send(lcd, POWERREG0_ADRS,
			POWER0_COM_DCLK | POWER0_COM_DOUT | POWER0_DAC_OFF |
			POWER0_COM_OFF | POWER0_VCC5_OFF);

	corgi_ssp_lcdtg_send(lcd, POWERREG1_ADRS,
			POWER1_VW_OFF | POWER1_GVSS_OFF | POWER1_VDD_OFF);

	corgi_ssp_lcdtg_send(lcd, POWERREG1_ADRS,
			POWER1_VW_OFF | POWER1_GVSS_OFF | POWER1_VDD_ON);
	mdelay(3);

	corgi_ssp_lcdtg_send(lcd, POWERREG0_ADRS,
			POWER0_COM_DCLK | POWER0_COM_DOUT | POWER0_DAC_ON |
			POWER0_COM_OFF | POWER0_VCC5_OFF);

	corgi_ssp_lcdtg_send(lcd, PICTRL_ADRS,
			PICTRL_INIT_STATE | PICTRL_COM_SIGNAL_OFF);

	comadj = corgi_lcd_comadj(NULL);

	lcdtg_set_common_voltage(lcd, POWER0_DAC_ON | POWER0_COM_OFF |
				 POWER0_VCC5_OFF, comadj);

	corgi_ssp_lcdtg_send(lcd, POWERREG0_ADRS,
			POWER0_COM_DCLK | POWER0_COM_DOUT | POWER0_DAC_ON |
			POWER0_COM_OFF | POWER0_VCC5_ON);

	corgi_ssp_lcdtg_send(lcd, POWERREG1_ADRS,
			POWER1_VW_OFF | POWER1_GVSS_ON | POWER1_VDD_ON);
	mdelay(2);

	corgi_ssp_lcdtg_send(lcd, PICTRL_ADRS, PICTRL_INIT_STATE);

	corgi_ssp_lcdtg_send(lcd, POWERREG0_ADRS,
			POWER0_COM_DCLK | POWER0_COM_DOUT | POWER0_DAC_ON |
			POWER0_COM_ON | POWER0_VCC5_ON);

	corgi_ssp_lcdtg_send(lcd, POWERREG1_ADRS,
			POWER1_VW_ON | POWER1_GVSS_ON | POWER1_VDD_ON);

	corgi_ssp_lcdtg_send(lcd, PICTRL_ADRS, 0);

	lcdtg_set_phadadj(lcd, lcd->mode);

	corgi_ssp_lcdtg_send(lcd, POLCTRL_ADRS,
			POLCTRL_SYNC_POL_RISE | POLCTRL_EN_POL_RISE |
			POLCTRL_DATA_POL_RISE | POLCTRL_SYNC_ACT_L |
			POLCTRL_EN_ACT_H);
	udelay(1000);

	switch (lcd->mode) {
	case CORGI_LCD_MODE_VGA:
		corgi_ssp_lcdtg_send(lcd, RESCTL_ADRS, RESCTL_VGA);
		break;
	case CORGI_LCD_MODE_QVGA:
	default:
		corgi_ssp_lcdtg_send(lcd, RESCTL_ADRS, RESCTL_QVGA);
		break;
	}
}

static void corgi_lcd_power_off(struct corgi_lcd *lcd)
{
	msleep(34);

	corgi_ssp_lcdtg_send(lcd, POWERREG1_ADRS,
			POWER1_VW_OFF | POWER1_GVSS_ON | POWER1_VDD_ON);

	corgi_ssp_lcdtg_send(lcd, PICTRL_ADRS, PICTRL_COM_SIGNAL_OFF);
	corgi_ssp_lcdtg_send(lcd, POWERREG0_ADRS,
			POWER0_DAC_ON | POWER0_COM_OFF | POWER0_VCC5_ON);

	lcdtg_set_common_voltage(lcd, POWER0_DAC_ON | POWER0_COM_OFF |
			POWER0_VCC5_ON, 0);

	corgi_ssp_lcdtg_send(lcd, POWERREG1_ADRS,
			POWER1_VW_OFF | POWER1_GVSS_OFF | POWER1_VDD_ON);

	corgi_ssp_lcdtg_send(lcd, POWERREG0_ADRS,
			POWER0_DAC_ON | POWER0_COM_OFF | POWER0_VCC5_OFF);

	corgi_ssp_lcdtg_send(lcd, PICTRL_ADRS,
			PICTRL_INIOFF | PICTRL_DAC_SIGNAL_OFF |
			PICTRL_POWER_DOWN | PICTRL_COM_SIGNAL_OFF);

	corgi_ssp_lcdtg_send(lcd, POWERREG0_ADRS,
			POWER0_DAC_OFF | POWER0_COM_OFF | POWER0_VCC5_OFF);

	corgi_ssp_lcdtg_send(lcd, POWERREG1_ADRS,
			POWER1_VW_OFF | POWER1_GVSS_OFF | POWER1_VDD_OFF);
}

static int corgi_lcd_set_mode(struct lcd_device *ld, u32 xres, u32 yres)
{
	struct corgi_lcd *lcd = lcd_get_data(ld);
	int mode = CORGI_LCD_MODE_QVGA;

	if (xres == 640 || xres == 480)
		mode = CORGI_LCD_MODE_VGA;

	if (lcd->mode == mode)
		return 0;

	lcdtg_set_phadadj(lcd, mode);

	switch (mode) {
	case CORGI_LCD_MODE_VGA:
		corgi_ssp_lcdtg_send(lcd, RESCTL_ADRS, RESCTL_VGA);
		break;
	case CORGI_LCD_MODE_QVGA:
	default:
		corgi_ssp_lcdtg_send(lcd, RESCTL_ADRS, RESCTL_QVGA);
		break;
	}

	lcd->mode = mode;
	return 0;
}

static int corgi_lcd_set_power(struct lcd_device *ld, int power)
{
	struct corgi_lcd *lcd = lcd_get_data(ld);

	if (POWER_IS_ON(power) && !POWER_IS_ON(lcd->power))
		corgi_lcd_power_on(lcd);

	if (!POWER_IS_ON(power) && POWER_IS_ON(lcd->power))
		corgi_lcd_power_off(lcd);

	lcd->power = power;
	return 0;
}

static int corgi_lcd_get_power(struct lcd_device *ld)
{
	struct corgi_lcd *lcd = lcd_get_data(ld);

	return lcd->power;
}

static const struct lcd_ops corgi_lcd_ops = {
	.get_power	= corgi_lcd_get_power,
	.set_power	= corgi_lcd_set_power,
	.set_mode	= corgi_lcd_set_mode,
};

static int corgi_bl_get_intensity(struct backlight_device *bd)
{
	struct corgi_lcd *lcd = bl_get_data(bd);

	return lcd->intensity;
}

static int corgi_bl_set_intensity(struct corgi_lcd *lcd, int intensity)
{
	int cont;

	if (intensity > 0x10)
		intensity += 0x10;

	corgi_ssp_lcdtg_send(lcd, DUTYCTRL_ADRS, intensity);

	cont = !!(intensity & 0x20);

	if (lcd->backlight_cont)
		gpiod_set_value_cansleep(lcd->backlight_cont, cont);

	if (lcd->backlight_on)
		gpiod_set_value_cansleep(lcd->backlight_on, intensity);

	if (lcd->kick_battery)
		lcd->kick_battery();

	lcd->intensity = intensity;
	return 0;
}

static int corgi_bl_update_status(struct backlight_device *bd)
{
	struct corgi_lcd *lcd = bl_get_data(bd);
	int intensity = backlight_get_brightness(bd);

	if (corgibl_flags & CORGIBL_SUSPENDED)
		intensity = 0;

	if ((corgibl_flags & CORGIBL_BATTLOW) && intensity > lcd->limit_mask)
		intensity = lcd->limit_mask;

	return corgi_bl_set_intensity(lcd, intensity);
}

void corgi_lcd_limit_intensity(int limit)
{
	if (limit)
		corgibl_flags |= CORGIBL_BATTLOW;
	else
		corgibl_flags &= ~CORGIBL_BATTLOW;

	backlight_update_status(the_corgi_lcd->bl_dev);
}
EXPORT_SYMBOL(corgi_lcd_limit_intensity);

static const struct backlight_ops corgi_bl_ops = {
	.get_brightness	= corgi_bl_get_intensity,
	.update_status  = corgi_bl_update_status,
};

#ifdef CONFIG_PM_SLEEP
static int corgi_lcd_suspend(struct device *dev)
{
	struct corgi_lcd *lcd = dev_get_drvdata(dev);

	corgibl_flags |= CORGIBL_SUSPENDED;
	corgi_bl_set_intensity(lcd, 0);
	corgi_lcd_set_power(lcd->lcd_dev, LCD_POWER_OFF);
	return 0;
}

static int corgi_lcd_resume(struct device *dev)
{
	struct corgi_lcd *lcd = dev_get_drvdata(dev);

	corgibl_flags &= ~CORGIBL_SUSPENDED;
	corgi_lcd_set_power(lcd->lcd_dev, LCD_POWER_ON);
	backlight_update_status(lcd->bl_dev);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(corgi_lcd_pm_ops, corgi_lcd_suspend, corgi_lcd_resume);

static int setup_gpio_backlight(struct corgi_lcd *lcd,
				struct corgi_lcd_platform_data *pdata)
{
	struct spi_device *spi = lcd->spi_dev;

	lcd->backlight_on = devm_gpiod_get_optional(&spi->dev, "BL_ON",
						    assume_powered ?
						      GPIOD_ASIS : GPIOD_OUT_LOW);
	if (IS_ERR(lcd->backlight_on))
		return PTR_ERR(lcd->backlight_on);

	lcd->backlight_cont = devm_gpiod_get_optional(&spi->dev, "BL_CONT",
						      assume_powered ?
							GPIOD_ASIS : GPIOD_OUT_LOW);
	if (IS_ERR(lcd->backlight_cont))
		return PTR_ERR(lcd->backlight_cont);

	return 0;
}

static int corgi_lcd_probe(struct spi_device *spi)
{
	struct backlight_properties props;
	struct corgi_lcd_platform_data *pdata = dev_get_platdata(&spi->dev);
	struct corgi_lcd *lcd;
	int ret = 0;

	if (pdata == NULL) {
		dev_err(&spi->dev, "platform data not available\n");
		return -EINVAL;
	}

	lcd = devm_kzalloc(&spi->dev, sizeof(struct corgi_lcd), GFP_KERNEL);
	if (!lcd)
		return -ENOMEM;

	lcd->spi_dev = spi;

	lcd->lcd_dev = devm_lcd_device_register(&spi->dev, "corgi_lcd",
						&spi->dev, lcd, &corgi_lcd_ops);
	if (IS_ERR(lcd->lcd_dev))
		return PTR_ERR(lcd->lcd_dev);

	lcd->power = LCD_POWER_OFF;
	lcd->mode = (pdata) ? pdata->init_mode : CORGI_LCD_MODE_VGA;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = pdata->max_intensity;
	lcd->bl_dev = devm_backlight_device_register(&spi->dev, "corgi_bl",
						&spi->dev, lcd, &corgi_bl_ops,
						&props);
	if (IS_ERR(lcd->bl_dev))
		return PTR_ERR(lcd->bl_dev);

	lcd->bl_dev->props.brightness = pdata->default_intensity;
	lcd->bl_dev->props.power = BACKLIGHT_POWER_ON;

	ret = setup_gpio_backlight(lcd, pdata);
	if (ret)
		return ret;

	lcd->kick_battery = pdata->kick_battery;

	spi_set_drvdata(spi, lcd);

	if (assume_powered) {
		lcd->power = LCD_POWER_ON;
		dev_info(&spi->dev,
			 "panel assumed already powered, skipping power-on\n");
	} else {
		corgi_lcd_set_power(lcd->lcd_dev, LCD_POWER_ON);
	}

	backlight_update_status(lcd->bl_dev);

	lcd->limit_mask = pdata->limit_mask;
	the_corgi_lcd = lcd;

	{
		const char *src = NULL;
		int comadj = corgi_lcd_comadj(&src);

		dev_info(&spi->dev,
			 "VCOM comadj=%d from %s; phadadj=%d; battery adadj=%d\n",
			 comadj, src, corgi_lcd_phadadj(),
			 (int)sharpsl_param.adadj);
	}

	return 0;
}

static void corgi_lcd_remove(struct spi_device *spi)
{
	struct corgi_lcd *lcd = spi_get_drvdata(spi);

	lcd->bl_dev->props.power = BACKLIGHT_POWER_ON;
	lcd->bl_dev->props.brightness = 0;
	backlight_update_status(lcd->bl_dev);
	corgi_lcd_set_power(lcd->lcd_dev, LCD_POWER_OFF);
}

static struct spi_driver corgi_lcd_driver = {
	.driver		= {
		.name	= "corgi-lcd",
		.pm	= &corgi_lcd_pm_ops,
	},
	.probe		= corgi_lcd_probe,
	.remove		= corgi_lcd_remove,
};

module_spi_driver(corgi_lcd_driver);

MODULE_DESCRIPTION("LCD and backlight driver for SHARP C7x0/Cxx00");
MODULE_AUTHOR("Eric Miao <eric.miao@marvell.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("spi:corgi-lcd");
