// SPDX-License-Identifier: GPL-2.0-only
/*
 * Battery and Power Management code for the Sharp SL-C7xx
 *
 * Copyright (c) 2005 Richard Purdie
 */

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

#define SHARPSL_CHARGE_ON_VOLT         0x99  /* 2.9V */
#define SHARPSL_CHARGE_ON_TEMP         0xe0  /* 2.9V */
#define SHARPSL_CHARGE_ON_ACIN_HIGH    0x9b  /* 6V */
#define SHARPSL_CHARGE_ON_ACIN_LOW     0x34  /* 2V */
#define SHARPSL_FATAL_ACIN_VOLT        182   /* 3.45V */
#define SHARPSL_FATAL_NOACIN_VOLT      170   /* 3.40V */

static void corgi_charger_init(void)
{
	/* gpio_request_array()/struct gpio were removed from current mainline;
	 * expand to individual gpio_request()+gpio_direction_*() calls. */
	gpio_request(CORGI_GPIO_ADC_TEMP_ON, "ADC Temp On");
	gpio_direction_output(CORGI_GPIO_ADC_TEMP_ON, 0);
	gpio_request(CORGI_GPIO_CHRG_ON, "Charger On");
	/*
	 * CORGI_GPIO_CHRG_ON also gates the orange charge LED (GPIO13) in
	 * hardware: closed (low), the LED never lights regardless of AC
	 * state; open (high), the charger circuit lights/extinguishes it
	 * automatically based on whether an adapter is actually plugged in.
	 * Hold it open permanently from boot so the LED always tracks real
	 * charging state -- see corgi_charge() below, which no longer
	 * touches this line. GPIO13 itself is the other half of the same
	 * enable and is driven high once at boot and never again; no
	 * software AC check is involved on either line (corgi.c, "Corgi
	 * LEDs").
	 */
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
	/*
	 * CORGI_GPIO_CHRG_ON is intentionally never toggled here -- it is
	 * held permanently open by corgi_charger_init() so the orange charge
	 * LED always works. Only CHRG_UKN (the suspend-time charge-bypass
	 * line) is managed by this function now.
	 */
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

/*
 * Check what brought us out of the suspend.
 * Return: 0 to sleep, otherwise wake
 */
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
			/* charge on */
			dev_dbg(sharpsl_pm.dev, "ac insert\n");
			sharpsl_pm.flags |= SHARPSL_DO_OFFLINE_CHRG;
		} else {
			/* charge off */
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
		/*
		 * REVERTED (2026-08-03): the 2026-07-27 change below special-cased
		 * Husky to `raw` (active-high) based on one observation of the
		 * orange LED not lighting during charging. That diagnosis doesn't
		 * hold up: since 2026-07-31 (commit 9571005) the LED is driven
		 * directly by hardware -- GPIO13 is an enable, not something
		 * software touches -- so a lit LED is ground truth for real
		 * charger presence, independent of this GPIO entirely. Live
		 * testing now (charger connected, orange LED lit, confirmed by
		 * reseating the cable) shows `raw` reads 0 the whole time, which
		 * under the Husky special case produced acin=0 (offline) while
		 * actually charging -- the exact inverted reading that made
		 * mb-applet-battery show the wrong icon. Plain `!raw`, matching
		 * mainline's Corgi/Shepherd/Spitz convention (see spitz_pm.c's
		 * equivalent), reports this correctly. Whatever caused the
		 * original LED symptom, it wasn't AC_IN polarity.
		 */
		return !gpio_get_value(CORGI_GPIO_AC_IN);
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
	/*
	 * Reverted 2026-08-03: the 2026-07-26 recalibration (148/138/145/135)
	 * assumed this pack peaks around raw ADC 140-145, but live readings on
	 * this board (raw ADC ~198 on the BATT_VOLT channel while unplugged)
	 * contradict that -- back to mainline's stock thresholds, which match
	 * what this board actually reports. See sharpsl_pm.c's battery_levels
	 * tables for the matching revert; the disabled critical-suspend
	 * trigger in sharpsl_battery_thread() is unrelated and stays as-is.
	 *
	 * status_low_acin/status_low_noac corrected 2026-08-03 to 181/178
	 * (from mainline's 178/175) after cross-checking against the actual
	 * Sharp-built kernels: SL-C760 and SL-C860 Cacko 1.23 zImage.bin
	 * (kernel 2.4.18-rmk7-pxa3-embedix, both boards being the same
	 * PXA255 hardware under different model-string ROMs) contain the
	 * identical compiled comparison `voltage > 181` and `voltage > 178`
	 * against the retry-averaged MAX1111 battery-voltage read, at the
	 * byte-identical code address in both kernels. No equivalent
	 * evidence was found for the two status_high_* thresholds, which
	 * remain mainline's values.
	 */
	.status_high_acin = 188,
	.status_low_acin  = 181,
	.status_high_noac = 185,
	.status_low_noac  = 178,
};

static struct platform_device *corgipm_device;

/*
 * This board does not boot as corgi, shepherd or husky. It comes up with
 * Sharp's legacy machine number 196 -- or the 19 the bootloader actually
 * passes in r1 -- both of which are matched by MACHINE_START entries at the
 * bottom of corgi.c using the same .init_machine as Corgi, so the hardware
 * is brought up identically. See that comment and
 * docs/DEADLETTER-MACHINE-ID-196.md.
 *
 * The stock gate below is the three machine_is_*() checks alone, which are
 * therefore all false here: corgipm_init() returned -ENODEV, the
 * "sharpsl-pm" platform device was never registered, and
 * apm_get_power_status stayed NULL. /proc/apm then reports nothing but the
 * kernel's own defaults from proc_apm_show() --
 *
 *     1.13 1.2 0x02 0xff 0xff 0xff -1% -1 ?
 *
 * -- i.e. AC status, battery status and charge all "unknown". Userspace
 * battery monitors show a read error and can never see the charger being
 * plugged in. There was no sharpsl-pm entry in /sys/devices/platform/ at
 * all, which is the quick way to confirm this.
 *
 * Kept as its own predicate rather than extended inline, because the
 * batfull_irq check below deliberately keys off machine_is_corgi() alone.
 */
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

	/*
	 * machine_is_corgi() is false for 196/19, so this board takes the
	 * batfull_irq path -- which is correct: the single Sharp machine
	 * descriptor that carries nr 196 calls itself "SHARP Shepherd".
	 */
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
