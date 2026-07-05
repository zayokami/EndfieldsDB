#ifndef EF_CRC_H
#define EF_CRC_H

#include <stddef.h>
#include <stdint.h>

uint32_t ef_crc32(const void *data, size_t len);
/* Continue CRC from internal state (before final XOR). */
uint32_t ef_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t ef_crc32_combine(uint32_t crc, const void *data, size_t len);

#endif
