
#include <linux/module.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/gpio-pxa.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/apm-emulation.h>
#include <linux/io.h>
#include <linux/spi/corgi_lcd.h>

#include <asm/irq.h>
#include <asm/mach-types.h>

#include "corgi.h"
#include "pxa2xx-regs.h"
#include "sharpsl_pm.h"

#include "generic.h"

#define SHARPSL_CHARGE_ON_VOLT         0x99
#define SHARPSL_CHARGE_ON_TEMP         0xe0
#define SHARPSL_CHARGE_ON_ACIN_HIGH    0x9b
#define SHARPSL_CHARGE_ON_ACIN_LOW     0x34
#define SHARPSL_FATAL_ACIN_VOLT        182
#define SHARPSL_FATAL_NOACIN_VOLT      170

static void corgi_charger_init(void)
{
	gpio_request(CORGI_GPIO_ADC_TEMP_ON, "ADC Temp On");
	gpio_direction_output(CORGI_GPIO_ADC_TEMP_ON, 0);
	gpio_request(CORGI_GPIO_CHRG_ON, "Charger On");
	gpio_direction_output(CORGI_GPIO_CHRG_ON, 1);
	gpio_request(CORGI_GPIO_CHRG_UKN, "Charger Unknown");
	gpio_direction_output(CORGI_GPIO_CHRG_UKN, 0);
	gpio_request(CORGI_GPIO_AC_IN, "Charger Detection");
	gpio_direction_input(CORGI_GPIO_AC_IN);
	gpio_request(CORGI_GPIO_KEY_INT, "Key Interrupt");
	gpio_direction_input(CORGI_GPIO_KEY_INT);
	gpio_request(CORGI_GPIO_WAKEUP, "System wakeup notification");
	gpio_direction_input(CORGI_GPIO_WAKEUP);
}

static void corgi_measure_temp(int on)
{
	gpio_set_value(CORGI_GPIO_ADC_TEMP_ON, on);
}

static void corgi_charge(int on)
{
	if (on) {
		if (machine_is_corgi() && (sharpsl_pm.flags & SHARPSL_SUSPENDED))
			gpio_set_value(CORGI_GPIO_CHRG_UKN, 1);
		else
			gpio_set_value(CORGI_GPIO_CHRG_UKN, 0);
	} else {
		gpio_set_value(CORGI_GPIO_CHRG_UKN, 0);
	}
}

static void corgi_discharge(int on)
{
	gpio_set_value(CORGI_GPIO_DISCHARGE_ON, on);
}

static void corgi_presuspend(void)
{
}

static void corgi_postsuspend(void)
{
}

static int corgi_should_wakeup(unsigned int resume_on_alarm)
{
	int is_resume = 0;

	dev_dbg(sharpsl_pm.dev, "PEDR = %x, GPIO_AC_IN = %d, "
		"GPIO_CHRG_FULL = %d, GPIO_KEY_INT = %d, GPIO_WAKEUP = %d\n",
		PEDR, gpio_get_value(CORGI_GPIO_AC_IN),
		gpio_get_value(CORGI_GPIO_CHRG_FULL),
		gpio_get_value(CORGI_GPIO_KEY_INT),
		gpio_get_value(CORGI_GPIO_WAKEUP));

	if ((PEDR & GPIO_bit(CORGI_GPIO_AC_IN))) {
		if (sharpsl_pm.machinfo->read_devdata(SHARPSL_STATUS_ACIN)) {
			dev_dbg(sharpsl_pm.dev, "ac insert\n");
			sharpsl_pm.flags |= SHARPSL_DO_OFFLINE_CHRG;
		} else {
			dev_dbg(sharpsl_pm.dev, "ac remove\n");
			sharpsl_pm_led(SHARPSL_LED_OFF);
			sharpsl_pm.machinfo->charge(0);
			sharpsl_pm.charge_mode = CHRG_OFF;
		}
	}

	if ((PEDR & GPIO_bit(CORGI_GPIO_CHRG_FULL)))
		dev_dbg(sharpsl_pm.dev, "Charge full interrupt\n");

	if (PEDR & GPIO_bit(CORGI_GPIO_KEY_INT))
		is_resume |= GPIO_bit(CORGI_GPIO_KEY_INT);

	if (PEDR & GPIO_bit(CORGI_GPIO_WAKEUP))
		is_resume |= GPIO_bit(CORGI_GPIO_WAKEUP);

	if (resume_on_alarm && (PEDR & PWER_RTC))
		is_resume |= PWER_RTC;

	dev_dbg(sharpsl_pm.dev, "is_resume: %x\n",is_resume);
	return is_resume;
}

static bool corgi_charger_wakeup(void)
{
	return !gpio_get_value(CORGI_GPIO_AC_IN) ||
		!gpio_get_value(CORGI_GPIO_KEY_INT) ||
		!gpio_get_value(CORGI_GPIO_WAKEUP);
}

static unsigned long corgipm_read_devdata(int type)
{
	switch(type) {
	case SHARPSL_STATUS_ACIN: {
		return gpio_get_value(CORGI_GPIO_AC_IN);
	}
	case SHARPSL_STATUS_LOCK:
		return gpio_get_value(sharpsl_pm.machinfo->gpio_batlock);
	case SHARPSL_STATUS_CHRGFULL:
		return gpio_get_value(sharpsl_pm.machinfo->gpio_batfull);
	case SHARPSL_STATUS_FATAL:
		return gpio_get_value(sharpsl_pm.machinfo->gpio_fatal);
	case SHARPSL_ACIN_VOLT:
		return sharpsl_pm_pxa_read_max1111(MAX1111_ACIN_VOLT);
	case SHARPSL_BATT_TEMP:
		return sharpsl_pm_pxa_read_max1111(MAX1111_BATT_TEMP);
	case SHARPSL_BATT_VOLT:
	default:
		return sharpsl_pm_pxa_read_max1111(MAX1111_BATT_VOLT);
	}
}

static struct sharpsl_charger_machinfo corgi_pm_machinfo = {
	.init            = corgi_charger_init,
	.exit            = NULL,
	.gpio_batlock    = CORGI_GPIO_BAT_COVER,
	.gpio_acin       = CORGI_GPIO_AC_IN,
	.gpio_batfull    = CORGI_GPIO_CHRG_FULL,
	.discharge       = corgi_discharge,
	.charge          = corgi_charge,
	.measure_temp    = corgi_measure_temp,
	.presuspend      = corgi_presuspend,
	.postsuspend     = corgi_postsuspend,
	.read_devdata    = corgipm_read_devdata,
	.charger_wakeup  = corgi_charger_wakeup,
	.should_wakeup   = corgi_should_wakeup,
#if defined(CONFIG_LCD_CORGI)
	.backlight_limit = corgi_lcd_limit_intensity,
#endif
	.charge_on_volt	  = SHARPSL_CHARGE_ON_VOLT,
	.charge_on_temp	  = SHARPSL_CHARGE_ON_TEMP,
	.charge_acin_high = SHARPSL_CHARGE_ON_ACIN_HIGH,
	.charge_acin_low  = SHARPSL_CHARGE_ON_ACIN_LOW,
	.fatal_acin_volt  = SHARPSL_FATAL_ACIN_VOLT,
	.fatal_noacin_volt= SHARPSL_FATAL_NOACIN_VOLT,
	.bat_levels       = 40,
	.bat_levels_noac  = sharpsl_battery_levels_noac,
	.bat_levels_acin  = sharpsl_battery_levels_acin,
	.status_high_acin = 188,
	.status_low_acin  = 181,
	.status_high_noac = 185,
	.status_low_noac  = 178,
};

static struct platform_device *corgipm_device;

#define MACH_TYPE_SHARP_LEGACY		196
#define MACH_TYPE_SHARP_BOOTLOADER	19

static int corgipm_machine_supported(void)
{
	return machine_is_corgi() || machine_is_shepherd()
		|| machine_is_husky()
		|| machine_arch_type == MACH_TYPE_SHARP_LEGACY
		|| machine_arch_type == MACH_TYPE_SHARP_BOOTLOADER;
}

static int corgipm_init(void)
{
	int ret;

	if (!corgipm_machine_supported())
		return -ENODEV;

	corgipm_device = platform_device_alloc("sharpsl-pm", -1);
	if (!corgipm_device)
		return -ENOMEM;

	if (!machine_is_corgi())
	    corgi_pm_machinfo.batfull_irq = 1;

	corgipm_device->dev.platform_data = &corgi_pm_machinfo;
	ret = platform_device_add(corgipm_device);

	if (ret)
		platform_device_put(corgipm_device);

	return ret;
}

static void corgipm_exit(void)
{
	platform_device_unregister(corgipm_device);
}

module_init(corgipm_init);
module_exit(corgipm_exit);
