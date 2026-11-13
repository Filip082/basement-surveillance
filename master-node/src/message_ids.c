/* SPDX-License-Identifier: Apache-2.0 */
#include "message_ids.h"

#include <errno.h>
#include <stdbool.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>

#define ID_RECORD 1U
#define RESERVATION_SIZE 32U

static struct nvs_fs ids;
static uint32_t next_id;
static uint32_t reserved_until;
static bool mounted;

int bs_message_ids_init(void)
{
	if (mounted) {
		return -EALREADY;
	}
	const struct flash_area *area;
	struct flash_pages_info page;
	int ret = flash_area_open(PARTITION_ID(storage_partition), &area);

	if (ret < 0) {
		return ret;
	}
	ids.flash_device = area->fa_dev;
	ids.offset = area->fa_off;
	ret = flash_get_page_info_by_offs(ids.flash_device, ids.offset, &page);
	if (ret == 0 && (area->fa_size / page.size < 2U || area->fa_size % page.size != 0 ||
			 area->fa_size / page.size > UINT16_MAX)) {
		ret = -EINVAL;
	}
	if (ret == 0) {
		ids.sector_size = page.size;
		ids.sector_count = area->fa_size / page.size;
		ret = nvs_mount(&ids);
	}
	flash_area_close(area);
	if (ret < 0) {
		return ret;
	}
	uint8_t value[4];
	ssize_t bytes = nvs_read(&ids, ID_RECORD, value, sizeof(value));

	if (bytes == -ENOENT) {
		next_id = 1;
	} else if (bytes != (ssize_t)sizeof(value)) {
		return bytes < 0 ? (int)bytes : -EIO;
	} else {
		next_id = sys_get_le32(value);
		if (next_id == 0) {
			return -EIO;
		}
	}
	reserved_until = next_id;
	mounted = true;
	return 0;
}

int bs_message_id_next(uint32_t *message_id)
{
	if (!mounted || message_id == NULL) {
		return -EINVAL;
	}
	if (next_id == reserved_until) {
		if (next_id > UINT32_MAX - RESERVATION_SIZE) {
			return -EOVERFLOW;
		}
		uint8_t value[4];
		uint32_t high_water = next_id + RESERVATION_SIZE;

		sys_put_le32(high_water, value);
		ssize_t ret = nvs_write(&ids, ID_RECORD, value, sizeof(value));

		if (ret < 0) {
			return (int)ret;
		}
		if (ret != 0 && ret != (ssize_t)sizeof(value)) {
			return -EIO;
		}
		reserved_until = high_water;
	}
	*message_id = next_id++;
	return 0;
}
