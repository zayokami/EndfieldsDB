#include "ef_crc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ef_crc_internal.h"

/* IEEE CRC-32 (reflected 0xEDB88320). SSE4.2 _mm_crc32_* is CRC-32C and must NOT be used here. */

static uint32_t ef_crc32_table[256];
static uint32_t ef_crc32_slice0[256];
static uint32_t ef_crc32_slice1[256];
static uint32_t ef_crc32_slice2[256];
static uint32_t ef_crc32_slice3[256];
static int ef_crc32_table_init = 0;

static void ef_crc32_init_tables(void)
{
    uint32_t i;
    uint32_t j;
    uint32_t crc;

    for (i = 0; i < 256; ++i) {
        crc = i;
        for (j = 0; j < 8; ++j) {
            if (crc & 1U) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
        ef_crc32_table[i] = crc;
    }

    for (i = 0; i < 256; ++i) {
        crc = ef_crc32_table[i];
        ef_crc32_slice0[i] = crc;
        crc = (crc >> 8) ^ ef_crc32_table[crc & 0xFFU];
        ef_crc32_slice1[i] = crc;
        crc = (crc >> 8) ^ ef_crc32_table[crc & 0xFFU];
        ef_crc32_slice2[i] = crc;
        crc = (crc >> 8) ^ ef_crc32_table[crc & 0xFFU];
        ef_crc32_slice3[i] = crc;
    }

    ef_crc32_table_init = 1;
}

static void ef_crc32_ensure_init(void)
{
    if (!ef_crc32_table_init) {
        ef_crc32_init_tables();
    }
}

uint32_t ef_crc32_update_slice4(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t word;

    ef_crc32_ensure_init();

    while (len >= 4U) {
        memcpy(&word, bytes, sizeof(word));
        crc ^= word;
        crc = ef_crc32_slice0[(crc >> 24) & 0xFFU] ^
              ef_crc32_slice1[(crc >> 16) & 0xFFU] ^
              ef_crc32_slice2[(crc >> 8) & 0xFFU] ^
              ef_crc32_slice3[crc & 0xFFU];
        bytes += 4;
        len -= 4U;
    }

    while (len > 0U) {
        crc = (crc >> 8) ^ ef_crc32_table[(crc ^ bytes[0]) & 0xFFU];
        bytes += 1;
        len -= 1U;
    }

    return crc;
}

uint32_t ef_crc32_update(uint32_t crc, const void *data, size_t len)
{
#if defined(__x86_64__) || defined(_M_X64)
    if (len >= 64U && ef_crc32_pclmul_available()) {
        return ef_crc32_update_pclmul(crc, data, len);
    }
#endif
    return ef_crc32_update_slice4(crc, data, len);
}

uint32_t ef_crc32(const void *data, size_t len)
{
    return ef_crc32_update(0xFFFFFFFFU, data, len) ^ 0xFFFFFFFFU;
}

uint32_t ef_crc32_combine(uint32_t crc, const void *data, size_t len)
{
    return ef_crc32_update(crc, data, len);
}
