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
		int raw = gpio_get_value(CORGI_GPIO_AC_IN);
		int acin;
		/*
		 * FIXED (2026-07-27): the previous unconditional `!raw` here
		 * assumed AC_IN is active-low, matching mainline's Corgi/Shepherd
		 * wiring (see spitz_pm.c's equivalent, also `!raw` -- same
		 * convention). Live testing on this actual Husky board (charger
		 * plugged in AND actively charging) showed raw=1 the whole time,
		 * which `!raw` turned into acin=0 (offline) -- sharpsl_ac_isr()
		 * then believed the charger had been removed and called
		 * sharpsl_charge_off(), which explicitly forces the orange LED's
		 * sharpsl-charge trigger to LED_OFF every time, even though real
		 * charging kept happening (CORGI_GPIO_CHRG_ON is permanently held
		 * open in corgi_charger_init(), so hardware charging wasn't
		 * actually gated by this software mistake) -- this is what made
		 * the LED appear to not light up all the time. Same class of
		 * bug as the Husky SD write-protect polarity quirk in corgi.c's
		 * husky_mci_gpio_table (GPIO_ACTIVE_HIGH override) -- Husky wiring
		 * for this GPIO simply differs from stock Corgi/Shepherd. Only
		 * flip polarity for Husky so real Corgi/Shepherd boards (if ever
		 * used with this kernel) keep the mainline-correct behavior.
		 */
		acin = machine_is_husky() ? raw : !raw;
		return acin;
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
	 * Recalibrated 2026-07-26 for this specific (heavily aged) battery pack:
	 * mainline's stock thresholds (188/178/185/175) assume a fresh battery
	 * that peaks around raw ADC 213. This unit's real-world MAX1111 readings
	 * top out around 140-145 even at/near full charge (confirmed against the
	 * stock charger + Cacko, which does not treat this as low/critical) --
	 * offset every threshold down by 40 to match. See drivers/sharpsl_pm.c's
	 * battery_levels tables (offset by the same -40) and the disabled
	 * critical-suspend trigger in sharpsl_battery_thread() for the rest of
	 * this recalibration.
	 */
	.status_high_acin = 148,
	.status_low_acin  = 138,
	.status_high_noac = 145,
	.status_low_noac  = 135,
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
