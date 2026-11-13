#ifndef BS_TEST_FLASH_MAP_H
#define BS_TEST_FLASH_MAP_H
#include <zephyr/drivers/flash.h>
#define PARTITION_ID(label) 0
struct flash_area {
	const struct device *fa_dev;
	off_t fa_off;
	size_t fa_size;
};
int flash_area_open(unsigned id, const struct flash_area **area);
void flash_area_close(const struct flash_area *area);
#endif
