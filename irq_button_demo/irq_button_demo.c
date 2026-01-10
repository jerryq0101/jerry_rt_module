#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/irqreturn.h> // sometimes included transitively

// This is GPIO pin 17 on the pi
#define GLOBAL_GPIO_NUMBER 529

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jerry Qi");
MODULE_DESCRIPTION("Simple GPIO threaded IRQ demo");

static int button_irq = -1;
static u64 last_irq_time = 0;

// Handles the top half (CPU runs this when the hardware signal immediately arrives)
static irqreturn_t my_irq_handler(int irq, void *dev_id)
{
	// Ok this potentially handles some quick stuff
	// signal to the kernel that says ok partially handled the interrupt, now schedule threaded handler to finsh it
	return IRQ_WAKE_THREAD;
}

// The IRQ thread that is woken by top half
// Should be scheduled immediatley because it is the highest priority
static irqreturn_t my_irq_thread(int irq, void *dev_id)
{
	u64 now = ktime_get_ns();
	if (now - last_irq_time < 10 * 1000 * 1000ULL) {
		return IRQ_HANDLED;
	}

	last_irq_time = now;
	pr_info("irqdemo: Button IRQ thread fired (irq=%d)\n", irq);

	// signal to kernel that finished processing this interrupt
	return IRQ_HANDLED;
}

static int __init irqdemo_init(void)
{
	int ret;

	pr_info("irqdemo: init\n");

	// (Optional but better) request and configure GPIO as input
	ret = gpio_request(GLOBAL_GPIO_NUMBER, "irqdemo_button");
	if (ret) {
		pr_err("irqdemo: gpio_request(%d) failed: %d\n", GLOBAL_GPIO_NUMBER, ret);
		return ret;
	}

	// it should be an input
	ret = gpio_direction_input(GLOBAL_GPIO_NUMBER);
	if (ret) {
		pr_err("irqdemo: gpio_direction_input(%d) failed: %d\n", GLOBAL_GPIO_NUMBER, ret);
		gpio_free(GLOBAL_GPIO_NUMBER);
		return ret;
	}

	// Gets the actual IRQ ID the kernel uses for this GPIO
	// IRQ lines are already fixed
	// Interrupt / IRQ line  - a hardware wire inside the chip that the CPU listens to
	// if this GPIO line gets signaled, CPU will see that IRQ line respond.
	// we need to make sure CPU seeing that IRQ line respond => runs the right things.

	button_irq = gpio_to_irq(GLOBAL_GPIO_NUMBER);
	if (button_irq < 0) {
		pr_err("irqdemo: gpio_to_irq(%d) failed: %d\n", GLOBAL_GPIO_NUMBER, button_irq);
		gpio_free(GLOBAL_GPIO_NUMBER);
		return button_irq;
	}

	pr_info("irqdemo: GPIO %d mapped to IRQ %d\n", GLOBAL_GPIO_NUMBER, button_irq);

	// We request this IRQ line to be mapped to top and bottom of irq handling
	ret = request_threaded_irq(
		button_irq,
		my_irq_handler, // top half
		my_irq_thread, // threaded handler
		IRQF_TRIGGER_RISING | IRQF_ONESHOT, // Fire interrupt on rising edge of GPIO | Don't reenter handler until thread finishes (GPIO might bounce, and the bouncing signal can cause another IRQ handler thread to run, which can cause an IRQ race -> bad state, don't allow for concurrency to happen)
		"buttonclicked",
		NULL // There can be multiple people using same IRQ line. If mutliple people using the same IRQ, then all handlers get called, each handler would have to check if that interrupt was menat for the handler.
	);

	if (ret) {
		pr_err("irqdemo: request_threaded_irq failed: %d\n", ret);
		gpio_free(GLOBAL_GPIO_NUMBER);
		button_irq = -1;
		return ret;
	}

	pr_info("irqdemo: registered threaded IRQ on GPIO %d (irq=%d)\n", GLOBAL_GPIO_NUMBER, button_irq);

	return 0;
}

static void __exit irqdemo_exit(void)
{
	pr_info("irqdemo: exit\n");

	if (button_irq >= 0) {
		free_irq(button_irq, NULL);
		button_irq = -1;
	}

	gpio_free(GLOBAL_GPIO_NUMBER);
}

module_init(irqdemo_init);
module_exit(irqdemo_exit);
