#ifndef BS_TEST_BYTEORDER_H
#define BS_TEST_BYTEORDER_H
#include <stdint.h>
static inline uint32_t sys_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void sys_put_le32(uint32_t value, uint8_t *p)
{
	for (unsigned i = 0; i < 4; ++i) {
		p[i] = (uint8_t)(value >> (8 * i));
	}
}
#endif
