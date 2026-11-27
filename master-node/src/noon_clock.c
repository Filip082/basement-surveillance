/* SPDX-License-Identifier: Apache-2.0 */
#include "noon_clock.h"

#include <errno.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(noon_clock, LOG_LEVEL_INF);

#define DAY_SECONDS 86400
#define NOON_SECONDS (12 * 60 * 60)

static struct k_sem *notification_sem;
static atomic_t *request_word;
static int request_bit;

/*
 * Zephyr's kernel timeouts use the nRF52840 low-frequency RTC system timer.
 * This callback runs in timer/interrupt context: never run I2C here.
 */
static void noon_alarm_expired(struct k_timer *timer)
{
    ARG_UNUSED(timer);

    if (notification_sem != NULL && request_word != NULL) {
        atomic_set_bit(request_word, request_bit);
        k_sem_give(notification_sem);
    }
}

K_TIMER_DEFINE(noon_alarm_timer, noon_alarm_expired, NULL);

int noon_clock_init(struct k_sem *wake_sem, atomic_t *requests, int noon_bit)
{
    if (wake_sem == NULL || requests == NULL || noon_bit < 0 ||
        noon_bit >= (int)(sizeof(atomic_val_t) * 8U)) {
        return -EINVAL;
    }

    notification_sem = wake_sem;
    request_word = requests;
    request_bit = noon_bit;

    return 0;
}

int noon_clock_set_local_time(int hour, int minute, int second)
{
    if (notification_sem == NULL || request_word == NULL) {
        return -EAGAIN;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return -EINVAL;
    }

    const int since_midnight = hour * 3600 + minute * 60 + second;
    int until_noon = NOON_SECONDS - since_midnight;

    if (until_noon < 0) {
        until_noon += DAY_SECONDS;
    }
    /* An exact 12:00:00 synchronization schedules the event immediately. */
    if (until_noon == 0) {
        until_noon = 1;
    }

    k_timer_start(&noon_alarm_timer,
                  K_SECONDS(until_noon), K_SECONDS(DAY_SECONDS));

    LOG_INF("Clock synchronized to %02d:%02d:%02d (local); next noon in %d s",
            hour, minute, second, until_noon);
    return 0;
}
