#include "ef_crc.h"

static uint32_t ef_crc32_table[256];
static int ef_crc32_table_init = 0;

static void ef_crc32_init_table(void)
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
    ef_crc32_table_init = 1;
}

static void ef_crc32_ensure_init(void)
{
    if (!ef_crc32_table_init) {
        ef_crc32_init_table();
    }
}

uint32_t ef_crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i;

    ef_crc32_ensure_init();

    for (i = 0; i < len; ++i) {
        crc = (crc >> 8) ^ ef_crc32_table[(crc ^ bytes[i]) & 0xFFU];
    }

    return crc;
}

uint32_t ef_crc32(const void *data, size_t len)
{
    return ef_crc32_update(0xFFFFFFFFU, data, len) ^ 0xFFFFFFFFU;
}

uint32_t ef_crc32_combine(uint32_t crc, const void *data, size_t len)
{
    return ef_crc32_update(crc, data, len);
}
