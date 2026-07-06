#ifndef EF_CRC_H
#define EF_CRC_H

#include <stddef.h>
#include <stdint.h>

/* IEEE CRC-32 (Ethernet / zlib polynomial). Slicing-by-4 everywhere; x86-64 also uses
 * PCLMULQDQ folding for buffers >= 64 bytes (see ef_crc_pclmul.c). */

uint32_t ef_crc32(const void *data, size_t len);
/* Continue CRC from internal state (before final XOR). */
uint32_t ef_crc32_update(uint32_t crc, const void *data, size_t len);
uint32_t ef_crc32_combine(uint32_t crc, const void *data, size_t len);

#endif
