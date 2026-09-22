// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_data/irda-pxaficp.h>

#include <media/rc-core.h>

#define DRIVER_NAME	"piko-cir-tx"
#define DEVICE_NAME	"piko IR blaster"

#define CARRIER_DEFAULT		38000
#define DUTY_CYCLE_DEFAULT	33

static int tx_gpio = 47;
static int pwdown_gpio = 22;
static bool pwdown_inverted;

module_param(tx_gpio, int, 0444);
module_param(pwdown_gpio, int, 0444);
module_param(pwdown_inverted, bool, 0444);

struct piko_cir {
	struct rc_dev	*rcdev;
	unsigned int	carrier;
	unsigned int	duty_cycle;
};

static struct piko_cir *piko_cir;

static void piko_cir_power(bool on)
{
	if (gpio_is_valid(pwdown_gpio))
		gpio_set_value(pwdown_gpio, !on ^ !!pwdown_inverted);
}

static void delay_until(ktime_t until)
{
	s32 delta;

	while (true) {
		delta = ktime_us_delta(until, ktime_get());
		if (delta <= 0)
			return;

		if (delta >= 1000) {
			mdelay(delta / 1000);
			continue;
		}

		udelay(delta);
		break;
	}
}

static void piko_cir_tx_modulated(struct piko_cir *cir, unsigned int *txbuf,
				  unsigned int count)
{
	unsigned int pulse, space;
	ktime_t edge;
	s32 delta;
	int i;

	pulse = DIV_ROUND_CLOSEST(cir->duty_cycle * (NSEC_PER_SEC / 100),
				  cir->carrier);
	space = DIV_ROUND_CLOSEST((100 - cir->duty_cycle) * (NSEC_PER_SEC / 100),
				  cir->carrier);

	edge = ktime_get();

	for (i = 0; i < count; i++) {
		if (i % 2) {
			edge = ktime_add_us(edge, txbuf[i]);
			delay_until(edge);
		} else {
			ktime_t last = ktime_add_us(edge, txbuf[i]);

			while (ktime_before(ktime_get(), last)) {
				gpio_set_value(tx_gpio, 1);
				edge = ktime_add_ns(edge, pulse);
				delta = ktime_to_ns(ktime_sub(edge, ktime_get()));
				if (delta > 0)
					ndelay(delta);
				gpio_set_value(tx_gpio, 0);
				edge = ktime_add_ns(edge, space);
				delta = ktime_to_ns(ktime_sub(edge, ktime_get()));
				if (delta > 0)
					ndelay(delta);
			}

			edge = last;
		}
	}
}

static void piko_cir_tx_unmodulated(struct piko_cir *cir, unsigned int *txbuf,
				    unsigned int count)
{
	ktime_t edge;
	int i;

	edge = ktime_get();

	for (i = 0; i < count; i++) {
		gpio_set_value(tx_gpio, !(i % 2));
		edge = ktime_add_us(edge, txbuf[i]);
		delay_until(edge);
	}

	gpio_set_value(tx_gpio, 0);
}

static int piko_cir_tx(struct rc_dev *dev, unsigned int *txbuf,
		       unsigned int count)
{
	struct piko_cir *cir = dev->priv;
	unsigned long flags;

	local_irq_save(flags);
	if (cir->carrier)
		piko_cir_tx_modulated(cir, txbuf, count);
	else
		piko_cir_tx_unmodulated(cir, txbuf, count);
	gpio_set_value(tx_gpio, 0);
	local_irq_restore(flags);

	return count;
}

static int piko_cir_set_carrier(struct rc_dev *dev, u32 carrier)
{
	struct piko_cir *cir = dev->priv;

	if (carrier > 500000)
		return -EINVAL;

	cir->carrier = carrier;

	return 0;
}

static int piko_cir_set_duty_cycle(struct rc_dev *dev, u32 duty_cycle)
{
	struct piko_cir *cir = dev->priv;

	cir->duty_cycle = duty_cycle;

	return 0;
}

static int __init piko_cir_init(void)
{
	struct rc_dev *rcdev;
	int err;

	piko_cir = kzalloc(sizeof(*piko_cir), GFP_KERNEL);
	if (!piko_cir)
		return -ENOMEM;

	piko_cir->carrier = CARRIER_DEFAULT;
	piko_cir->duty_cycle = DUTY_CYCLE_DEFAULT;

	err = gpio_request(tx_gpio, "piko cir tx");
	if (err) {
		pr_err(DRIVER_NAME ": gpio %d busy, is pxaficp_ir loaded?\n",
		       tx_gpio);
		goto err_free;
	}

	err = gpio_direction_output(tx_gpio, 0);
	if (err)
		goto err_tx_gpio;

	if (gpio_is_valid(pwdown_gpio)) {
		err = gpio_request(pwdown_gpio, "piko cir power");
		if (err) {
			pr_err(DRIVER_NAME ": gpio %d busy, is pxaficp_ir loaded?\n",
			       pwdown_gpio);
			goto err_tx_gpio;
		}

		err = gpio_direction_output(pwdown_gpio,
					    !false ^ !!pwdown_inverted);
		if (err)
			goto err_pwdown_gpio;
	}

	rcdev = rc_allocate_device(RC_DRIVER_IR_RAW_TX);
	if (!rcdev) {
		err = -ENOMEM;
		goto err_pwdown_gpio;
	}

	rcdev->priv = piko_cir;
	rcdev->driver_name = DRIVER_NAME;
	rcdev->device_name = DEVICE_NAME;
	rcdev->tx_ir = piko_cir_tx;
	rcdev->s_tx_carrier = piko_cir_set_carrier;
	rcdev->s_tx_duty_cycle = piko_cir_set_duty_cycle;

	err = rc_register_device(rcdev);
	if (err)
		goto err_rcdev;

	piko_cir->rcdev = rcdev;

	pxa2xx_transceiver_mode(NULL, IR_OFF);
	piko_cir_power(true);

	pr_info(DRIVER_NAME ": tx on gpio %d, %u Hz at %u%%\n",
		tx_gpio, piko_cir->carrier, piko_cir->duty_cycle);

	return 0;

err_rcdev:
	rc_free_device(rcdev);
err_pwdown_gpio:
	if (gpio_is_valid(pwdown_gpio))
		gpio_free(pwdown_gpio);
err_tx_gpio:
	gpio_free(tx_gpio);
err_free:
	kfree(piko_cir);
	piko_cir = NULL;

	return err;
}

static void __exit piko_cir_exit(void)
{
	gpio_set_value(tx_gpio, 0);
	piko_cir_power(false);

	rc_unregister_device(piko_cir->rcdev);

	if (gpio_is_valid(pwdown_gpio))
		gpio_free(pwdown_gpio);
	gpio_free(tx_gpio);

	kfree(piko_cir);
	piko_cir = NULL;
}

module_init(piko_cir_init);
module_exit(piko_cir_exit);

MODULE_DESCRIPTION("piko consumer IR transmitter on the Zaurus IrDA transceiver");
MODULE_LICENSE("GPL");
