#ifndef EF_CRC_H
#define EF_CRC_H

#include <stddef.h>
#include <stdint.h>

/* IEEE CRC-32 (Ethernet / zlib polynomial). Slicing-by-4 acceleration; SSE4.2 _mm_crc32_* is
 * CRC-32C and is not used for persisted superblock / slot header checksums. */

uint32_t ef_crc32(const void *data, size_t len);
/* Continue CRC from internal state (before final XOR). */
uint32_t ef_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t ef_crc32_combine(uint32_t crc, const void *data, size_t len);

#endif
