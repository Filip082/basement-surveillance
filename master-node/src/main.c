/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(master_node, LOG_LEVEL_DBG);

int main(void)
{
	LOG_INF("master-node up on %s", CONFIG_BOARD_TARGET);

	for (int i = 0;; i++) {
		LOG_DBG("tick %d", i);
		/* Never spin without sleeping: the USB stack sends data from
		 * other threads, and a busy loop starves them. */
		k_msleep(1000);
	}

	return 0;
}
