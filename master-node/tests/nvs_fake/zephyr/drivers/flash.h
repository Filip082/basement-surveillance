/* Host-only declarations for the flash-backed ID allocator tests. */
#ifndef BS_TEST_FLASH_H
#define BS_TEST_FLASH_H
#include <stddef.h>
#include <sys/types.h>
/** @brief Host-only device identity token; does not model a real Zephyr device. */
struct device {
	int unused; /**< Placeholder retaining a valid C struct; has no runtime meaning. */
};

/** @brief Minimal flash-page geometry consumed by the ID allocator's host tests. */
struct flash_pages_info {
	size_t size; /**< Erase-page size in bytes, supplied by the fake flash implementation. */
};
int flash_get_page_info_by_offs(const struct device *dev, off_t offset,
			       struct flash_pages_info *page);
#endif
