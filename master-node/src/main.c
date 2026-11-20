/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(master_node, LOG_LEVEL_DBG);

#define LED DT_ALIAS(led0)
#define PIR DT_PATH(zephyr_user)

#define PCF_NODE DT_NODELABEL(pcf8575)

static const struct device *pcf = DEVICE_DT_GET(PCF_NODE);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED, gpios);
static const struct gpio_dt_spec wake_pin = GPIO_DT_SPEC_GET(PIR, wake_gpios);

K_SEM_DEFINE(wake_sem,0,1);

static struct gpio_callback wake_cb;

static void wake_interrupt(const struct device* port, struct gpio_callback* cb, gpio_port_pins_t pins){
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_sem_give(&wake_sem);
}

int set_output(uint8_t output)
{
    if (output >= 16) return -EINVAL;
    uint16_t value = BIT(output);

    return gpio_port_set_masked_raw(pcf, 0xFFFF, value);
}

int main(void)
{
	if(!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&wake_pin)){
		LOG_DBG("Error 0");
		return -1;
	}
    if (!device_is_ready(pcf)) {
        LOG_DBG("PCF8575 driver not ready!");
        return -1;
    }

	if(gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE)) {
		LOG_DBG("Error 1");
		return -1;
	}
	if(gpio_pin_configure_dt(&wake_pin, GPIO_INPUT | GPIO_PULL_DOWN)) {
		LOG_DBG("Error 2");
		return -1;
	}

	gpio_init_callback(&wake_cb, wake_interrupt, BIT(wake_pin.pin));
	if(gpio_add_callback(wake_pin.port ,&wake_cb)) {
		LOG_DBG("Error 3");
		return -1;
	}
	if(gpio_pin_interrupt_configure_dt(&wake_pin, GPIO_INT_EDGE_TO_ACTIVE)) {
		LOG_DBG("Error 4");
		return -1;
	}

    for (int i = 0; i < 16; i++) {
        if (gpio_pin_configure(pcf, i, GPIO_OUTPUT_INACTIVE)) {
            LOG_DBG("PCF8575 GPIO error: %d", i);
            return -1;
        }
    }

	
	LOG_INF("master-node up on %s", CONFIG_BOARD_TARGET);

	while(true){
		k_sem_take(&wake_sem, K_FOREVER);

		gpio_pin_set_dt(&led, 1);
		LOG_DBG("Sensed movement");

        int ret = set_output(3);

        if (ret < 0) {
            LOG_DBG("PCF8575 write failed: %d", ret);
        } else {
            LOG_DBG("PCF8575 successfuly send 1 to port 3");
        }

		k_msleep(2000);

        gpio_port_set_masked_raw(pcf, 0xFFFF, 0);
		gpio_pin_set_dt(&led, 0);
	}

	return 0;
}