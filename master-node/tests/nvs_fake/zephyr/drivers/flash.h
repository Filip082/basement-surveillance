/* Host-only declarations for the flash-backed ID allocator tests. */
#ifndef BS_TEST_FLASH_H
#define BS_TEST_FLASH_H
#include <stddef.h>
#include <sys/types.h>
struct device { int unused; };
struct flash_pages_info { size_t size; };
int flash_get_page_info_by_offs(const struct device *dev, off_t offset,
			       struct flash_pages_info *page);
#endif
