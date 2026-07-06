#ifndef EF_CRC_INTERNAL_H
#define EF_CRC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

uint32_t ef_crc32_update_slice4(uint32_t crc, const void *data, size_t len);

int ef_crc32_pclmul_available(void);
uint32_t ef_crc32_update_pclmul(uint32_t crc, const void *data, size_t len);

#endif
