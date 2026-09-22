// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/platform_data/irda-pxaficp.h>

#include <media/rc-core.h>

#define DRIVER_NAME	"piko-cir"
#define DEVICE_NAME	"piko consumer IR"

#define CARRIER_DEFAULT		38000
#define DUTY_CYCLE_DEFAULT	33

static int tx_gpio = 47;
static int rx_gpio = 46;
static int pwdown_gpio = 22;
static bool pwdown_inverted;
static bool rx_active_low = true;
static int carrier_gap = 150;
static int rx_edges;

module_param(tx_gpio, int, 0444);
module_param(rx_gpio, int, 0444);
module_param(pwdown_gpio, int, 0444);
module_param(pwdown_inverted, bool, 0444);
module_param(rx_active_low, bool, 0444);
module_param(carrier_gap, int, 0644);
module_param(rx_edges, int, 0644);

struct piko_cir {
	struct rc_dev	*rcdev;
	unsigned int	carrier;
	unsigned int	duty_cycle;

	spinlock_t	lock;
	struct hrtimer	gap;
	int		irq;
	ktime_t		last_edge;
	ktime_t		pulse_start;
	bool		in_pulse;
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

static int piko_cir_gap_us(void)
{
	return clamp(carrier_gap, 10, 10000);
}

static bool piko_cir_lit(void)
{
	return !!gpio_get_value(rx_gpio) != rx_active_low;
}

static void piko_cir_emit(struct piko_cir *cir, bool pulse, s64 us)
{
	struct ir_raw_event ev = {};

	if (us < 1)
		us = 1;

	ev.pulse = pulse;
	ev.duration = min_t(s64, us, IR_MAX_DURATION);

	ir_raw_event_store_with_timeout(cir->rcdev, &ev);
}

static irqreturn_t piko_cir_rx_irq(int irq, void *dev_id)
{
	struct piko_cir *cir = dev_id;
	ktime_t now = ktime_get();
	unsigned long flags;

	spin_lock_irqsave(&cir->lock, flags);

	rx_edges++;

	if (!cir->in_pulse) {
		piko_cir_emit(cir, false, ktime_us_delta(now, cir->last_edge));
		cir->pulse_start = now;
		cir->in_pulse = true;
		hrtimer_start(&cir->gap, us_to_ktime(piko_cir_gap_us()),
			      HRTIMER_MODE_REL);
	}

	cir->last_edge = now;

	spin_unlock_irqrestore(&cir->lock, flags);

	return IRQ_HANDLED;
}

static enum hrtimer_restart piko_cir_gap(struct hrtimer *t)
{
	struct piko_cir *cir = container_of(t, struct piko_cir, gap);
	enum hrtimer_restart ret = HRTIMER_NORESTART;
	ktime_t now = ktime_get();
	unsigned long flags;
	s64 idle;
	int gap;

	spin_lock_irqsave(&cir->lock, flags);

	if (piko_cir_lit())
		cir->last_edge = now;

	gap = piko_cir_gap_us();
	idle = ktime_us_delta(now, cir->last_edge);
	if (idle < gap) {
		hrtimer_forward_now(t, us_to_ktime(gap - idle));
		ret = HRTIMER_RESTART;
		goto out;
	}

	piko_cir_emit(cir, true,
		      ktime_us_delta(cir->last_edge, cir->pulse_start));
	cir->in_pulse = false;

out:
	spin_unlock_irqrestore(&cir->lock, flags);

	if (ret == HRTIMER_NORESTART)
		ir_raw_event_handle(cir->rcdev);

	return ret;
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
	spin_lock_init(&piko_cir->lock);
	hrtimer_setup(&piko_cir->gap, piko_cir_gap, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);

	if (gpio_is_valid(pwdown_gpio)) {
		err = gpio_request(pwdown_gpio, "piko cir power");
		if (err) {
			pr_err(DRIVER_NAME ": gpio %d busy, run  irmode off  first\n",
			       pwdown_gpio);
			goto err_free;
		}
	}

	err = gpio_request(tx_gpio, "piko cir tx");
	if (err) {
		pr_err(DRIVER_NAME ": gpio %d busy\n", tx_gpio);
		goto err_pwdown_gpio;
	}

	err = gpio_request(rx_gpio, "piko cir rx");
	if (err) {
		pr_err(DRIVER_NAME ": gpio %d busy\n", rx_gpio);
		goto err_tx_gpio;
	}

	pxa2xx_transceiver_mode(NULL, IR_OFF);

	if (gpio_is_valid(pwdown_gpio)) {
		err = gpio_direction_output(pwdown_gpio,
					    !false ^ !!pwdown_inverted);
		if (err)
			goto err_rx_gpio;
	}

	err = gpio_direction_output(tx_gpio, 0);
	if (err)
		goto err_rx_gpio;

	err = gpio_direction_input(rx_gpio);
	if (err)
		goto err_rx_gpio;

	err = gpio_to_irq(rx_gpio);
	if (err < 0)
		goto err_rx_gpio;
	piko_cir->irq = err;

	rcdev = rc_allocate_device(RC_DRIVER_IR_RAW);
	if (!rcdev) {
		err = -ENOMEM;
		goto err_rx_gpio;
	}

	rcdev->priv = piko_cir;
	rcdev->driver_name = DRIVER_NAME;
	rcdev->device_name = DEVICE_NAME;
	rcdev->input_phys = DRIVER_NAME "/input0";
	rcdev->input_id.bustype = BUS_HOST;
	rcdev->input_id.vendor = 0x0001;
	rcdev->input_id.product = 0x0001;
	rcdev->input_id.version = 0x0100;
	rcdev->map_name = RC_MAP_EMPTY;
	rcdev->allowed_protocols = RC_PROTO_BIT_ALL_IR_DECODER;
	rcdev->rx_resolution = 1;
	rcdev->min_timeout = 1;
	rcdev->timeout = IR_DEFAULT_TIMEOUT;
	rcdev->max_timeout = 10 * IR_DEFAULT_TIMEOUT;
	rcdev->tx_ir = piko_cir_tx;
	rcdev->s_tx_carrier = piko_cir_set_carrier;
	rcdev->s_tx_duty_cycle = piko_cir_set_duty_cycle;

	err = rc_register_device(rcdev);
	if (err)
		goto err_rcdev;

	piko_cir->rcdev = rcdev;
	piko_cir_power(true);
	piko_cir->last_edge = ktime_get();

	err = request_irq(piko_cir->irq, piko_cir_rx_irq,
			  rx_active_low ? IRQF_TRIGGER_FALLING
					: IRQF_TRIGGER_RISING,
			  DRIVER_NAME, piko_cir);
	if (err)
		goto err_power;

	pr_info(DRIVER_NAME ": tx gpio %d at %u Hz %u%%, rx gpio %d irq %d gap %d us\n",
		tx_gpio, piko_cir->carrier, piko_cir->duty_cycle,
		rx_gpio, piko_cir->irq, piko_cir_gap_us());

	return 0;

err_power:
	piko_cir_power(false);
	rc_unregister_device(rcdev);
	rcdev = NULL;
err_rcdev:
	rc_free_device(rcdev);
err_rx_gpio:
	gpio_free(rx_gpio);
err_tx_gpio:
	gpio_free(tx_gpio);
err_pwdown_gpio:
	if (gpio_is_valid(pwdown_gpio))
		gpio_free(pwdown_gpio);
err_free:
	kfree(piko_cir);
	piko_cir = NULL;

	return err;
}

static void __exit piko_cir_exit(void)
{
	free_irq(piko_cir->irq, piko_cir);
	hrtimer_cancel(&piko_cir->gap);

	gpio_set_value(tx_gpio, 0);
	piko_cir_power(false);

	rc_unregister_device(piko_cir->rcdev);

	if (gpio_is_valid(pwdown_gpio))
		gpio_free(pwdown_gpio);
	gpio_free(rx_gpio);
	gpio_free(tx_gpio);

	kfree(piko_cir);
	piko_cir = NULL;
}

module_init(piko_cir_init);
module_exit(piko_cir_exit);

MODULE_DESCRIPTION("piko consumer IR send and receive on the Zaurus IrDA transceiver");
MODULE_LICENSE("GPL");
