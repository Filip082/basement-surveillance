#ifndef BS_TEST_FLASH_MAP_H
#define BS_TEST_FLASH_MAP_H
#include <zephyr/drivers/flash.h>
#define PARTITION_ID(label) 0
/** @brief Host-only partition description exposing the fields used by message_ids.c. */
struct flash_area {
	const struct device *fa_dev; /**< Fake backing device identity used by test assertions. */
	off_t fa_off;               /**< Partition start offset in bytes relative to the device. */
	size_t fa_size;             /**< Number of bytes assigned to the simulated partition. */
};
int flash_area_open(unsigned id, const struct flash_area **area);
void flash_area_close(const struct flash_area *area);
#endif
