/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/* Register an event to raise in the existing master-node main thread. */
int noon_clock_init(struct k_sem *wake_sem, atomic_t *requests, int noon_bit);

/*
 * Synchronize a SOFTWARE clock to the current LOCAL time (24-hour clock).
 * The timer then generates one event at the next 12:00:00 and every 24 hours,
 * while the device remains powered in Zephyr System ON mode.
 *
 * WARNING: nRF52840 RTC does not know calendar time. Call this from an actual
 * time source (RTC chip, USB, etc.) after every cold boot. No BLE code is
 * modified and this function does NOT magically discover local time.
 * DST changes / clock drift require another synchronization.
 */
int noon_clock_set_local_time(int hour, int minute, int second);
