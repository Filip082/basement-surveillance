#ifndef BS_TEST_NVS_H
#define BS_TEST_NVS_H
#include <stdint.h>
#include <zephyr/drivers/flash.h>
struct nvs_fs {
	const struct device *flash_device;
	off_t offset;
	uint32_t sector_size;
	uint16_t sector_count;
};
int nvs_mount(struct nvs_fs *fs);
ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *value, size_t size);
ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *value, size_t size);
#endif
