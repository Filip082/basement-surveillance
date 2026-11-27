#ifndef BS_TEST_NVS_H
#define BS_TEST_NVS_H
#include <stdint.h>
#include <zephyr/drivers/flash.h>
/**
 * @brief Host-only NVS configuration subset required by the real ID allocator.
 *
 * The fake checks allocator configuration and reservation behavior; it does not
 * reproduce Zephyr's NVS internals, flash writes or power-loss guarantees.
 */
struct nvs_fs {
	const struct device *flash_device; /**< Fake flash device containing the ID partition. */
	off_t offset;                     /**< Partition start offset in bytes on that device. */
	uint32_t sector_size;             /**< Size of one NVS erase sector in bytes. */
	uint16_t sector_count;            /**< Number of erase sectors assigned to the partition. */
};
int nvs_mount(struct nvs_fs *fs);
ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *value, size_t size);
ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *value, size_t size);
#endif
