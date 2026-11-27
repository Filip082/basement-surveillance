/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PIR is connected to PCF8575 P04, and the expander /INT line wakes the nRF.
 * Camera image reception remains in camera_node.c. The 12:00 event uses
 * a Zephyr timer after the local clock has been synchronized.
 */

#include <errno.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "camera_node.h"
#include "cam_wake.h"
#include "noon_clock.h"

LOG_MODULE_REGISTER(master_node, LOG_LEVEL_DBG);

/* Existing on-board LED (active-low physically on the XIAO). */
static const struct gpio_dt_spec status_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* A short LED indication without delaying the camera request. */
static void status_led_off(struct k_work *work)
{
    ARG_UNUSED(work);
    (void)gpio_pin_set_dt(&status_led, 0);
}

K_WORK_DELAYABLE_DEFINE(status_led_off_work, status_led_off);

BUILD_ASSERT(DT_NODE_HAS_PROP(DT_PATH(zephyr_user), pir_gpios),
             "the board overlay must define zephyr,user pir-gpios");

/* Real PIR input on PCF8575 P04; /INT is wired to XIAO D0. */
static const struct gpio_dt_spec pir = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pir_gpios);
static struct gpio_callback pir_cb_data;
static K_SEM_DEFINE(event_sem, 0, 1);

#define REQ_IMAGE 0
#define REQ_NOON  1
static atomic_t requests;

#define PIR_DEBOUNCE_MS 300

/* PIR input change callback: debounced; wakes the main thread. */
static void trigger_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	static int64_t last_ms;
	int64_t now = k_uptime_get();

	if (now - last_ms < PIR_DEBOUNCE_MS) {
		return;
	}
	last_ms = now;
	atomic_set_bit(&requests, REQ_IMAGE);
	k_sem_give(&event_sem);
}

static int input_init(const struct gpio_dt_spec *spec, struct gpio_callback *cb,
		      gpio_callback_handler_t isr)
{
	if (!gpio_is_ready_dt(spec)) {
        LOG_ERR("input GPIO controller not ready");
        return -ENODEV;
    }

    // PCF8575: nie konfigurujemy GPIO_INPUT
    // ze wzgledu na blad sterownika w Zephyr 4.4.2.

    gpio_init_callback(cb, isr, BIT(spec->pin));

    int err = gpio_add_callback(spec->port, cb);
    if (err) {
        return err;
    }

    return gpio_pin_interrupt_configure_dt(
        spec, GPIO_INT_EDGE_TO_ACTIVE
    );
}

int main(void)
{
    LOG_INF("master-node up on %s", CONFIG_BOARD_TARGET);

    /* PCF8575 is initialized by Zephyr. Its /INT line is on XIAO D0. */
    if (!gpio_is_ready_dt(&status_led) ||
        gpio_pin_configure_dt(&status_led, GPIO_OUTPUT_INACTIVE) != 0) {
        LOG_ERR("status LED not ready");
        return -ENODEV;
    }

    /* No changes to BLE initialization or camera_node_* functions. */
    if (camera_node_init() || input_init(&pir, &pir_cb_data, trigger_isr)) {
        LOG_ERR("setup failed - check the board overlay / PCF INT on D0");
        return -ENODEV;
    }

    /* Clear any pending PCF8575 change and establish an initial PIR state. */
    int initial_pir = gpio_pin_get_dt(&pir);
    if (initial_pir < 0) {
        LOG_ERR("PIR initial read over I2C failed: %d", initial_pir);
        return initial_pir;
    }

    if (noon_clock_init(&event_sem, &requests, REQ_NOON) != 0) {
        LOG_ERR("RTC timer initialization failed");
        return -EINVAL;
    }

#if CONFIG_APP_RTC_BOOT_HOUR >= 0
    /* Demonstration only: requires an actually known local time at power-on. */
    (void)noon_clock_set_local_time(CONFIG_APP_RTC_BOOT_HOUR,
                                    CONFIG_APP_RTC_BOOT_MINUTE,
                                    CONFIG_APP_RTC_BOOT_SECOND);
#else
    LOG_WRN("No wall clock: noon alarm unarmed; sync with noon_clock_set_local_time()");
#endif

    while(true) { //nie zgadzam się na żadne for(;;)
        LOG_INF("sleeping until PIR / RTC event");
        k_sem_take(&event_sem, K_FOREVER);

        /* Timeout callback does not access I2C. Handle the alarm here. */
        if (atomic_test_and_clear_bit(&requests, REQ_NOON)) {
            LOG_INF("RTC noon alarm: camera wake pulse 80 ms");
            cam_wake_pulse(80);
        }

        if (atomic_test_and_clear_bit(&requests, REQ_IMAGE)) {
            /* Zephyr 4.4 PCF857x callbacks signal BOTH edges regardless
             * of requested trigger. Only PIR HIGH may request an image. */
            int pir_state = gpio_pin_get_dt(&pir);
            if (pir_state < 0) {
                LOG_ERR("PIR read failed: %d", pir_state);
            } else if (pir_state == 1) {
                /* Preserve the LED indication on a valid interrupt. */
                (void)gpio_pin_set_dt(&status_led, 1);
                (void)k_work_reschedule(&status_led_off_work, K_SECONDS(2));

                /* take_image(): unchanged BLE, 30 ms low on PCF8575 P06
                 * (CONFIG_APP_IMAGE_WAKE_PULSE_MS=30 on XIAO only). */
                /*if (take_image()) {
                    const struct stored_image *img = get_image();
                    if (img != NULL) {
                        // >>> picture is in RAM: img->data, img->len, img->status. <<< 
                        LOG_INF("image ready: %u bytes, %s", img->len,
                                image_status_str(img->status));
                    }
                }*/
            } else {
                LOG_DBG("PCF interrupt ignored: PIR is LOW");
            }
		   LOG_DBG("PIR detected.");
        }

    }
    return 0;
}
