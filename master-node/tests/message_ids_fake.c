/* SPDX-License-Identifier: Apache-2.0
 * Compile the real allocator with a fake NVS interface. This tests reservation logic,
 * not Zephyr's NVS implementation or the physical flash's power-loss behavior.
 */
#include <assert.h>
#include <string.h>
#include "../src/message_ids.c"

static const struct device flash_device;
static const struct flash_area area = {
	.fa_dev = &flash_device, .fa_off = 0xec000, .fa_size = 32768,
};
static uint8_t record[4];
static bool have_record;
static int write_error;
static int read_error;
static unsigned writes;

int flash_area_open(unsigned id, const struct flash_area **output)
{
	assert(id == 0);
	*output = &area;
	return 0;
}

void flash_area_close(const struct flash_area *input)
{
	assert(input == &area);
}

int flash_get_page_info_by_offs(const struct device *dev, off_t offset,
			       struct flash_pages_info *page)
{
	assert(dev == &flash_device && offset == area.fa_off);
	page->size = 4096;
	return 0;
}

int nvs_mount(struct nvs_fs *fs)
{
	assert(fs->sector_size == 4096 && fs->sector_count == 8);
	return 0;
}

ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *value, size_t size)
{
	assert(fs == &ids && id == ID_RECORD && size == sizeof(record));
	if (read_error) {
		return read_error;
	}
	if (!have_record) {
		return -ENOENT;
	}
	memcpy(value, record, size);
	return (ssize_t)size;
}

ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *value, size_t size)
{
	assert(fs == &ids && id == ID_RECORD && size == sizeof(record));
	if (write_error) {
		return write_error;
	}
	memcpy(record, value, size);
	have_record = true;
	++writes;
	return (ssize_t)size;
}

void fake_ids_reboot(void)
{
	mounted = false;
	next_id = 0;
	reserved_until = 0;
	memset(&ids, 0, sizeof(ids));
}

void fake_ids_erase(void)
{
	fake_ids_reboot();
	have_record = false;
	write_error = 0;
	read_error = 0;
	writes = 0;
}

void fake_ids_write_error(int error) { write_error = error; }
void fake_ids_read_error(int error) { read_error = error; }
unsigned fake_ids_writes(void) { return writes; }
void fake_ids_high_water(uint32_t value)
{
	sys_put_le32(value, record);
	have_record = true;
}
