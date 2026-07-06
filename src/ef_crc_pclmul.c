/* IEEE CRC-32 (Ethernet) via PCLMULQDQ on x86-64.
 * Algorithm and constants from Intel soft-crc (BSD-3-Clause), crc_ether.c / crcr.h.
 * SSE4.2 _mm_crc32_* is CRC-32C and is not used here. */

#include "ef_crc_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)

#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif

#define EF_CRC_PCLMUL_MIN 64U

typedef struct ef_crc32_pclmul_ctx {
    uint64_t rk1;
    uint64_t rk2;
    uint64_t rk5;
    uint64_t rk6;
    uint64_t rk7;
    uint64_t rk8;
} ef_crc32_pclmul_ctx;

#if defined(_MSC_VER)
#define EF_CRC_ALIGNED16 __declspec(align(16))
#else
#define EF_CRC_ALIGNED16 __attribute__((aligned(16)))
#endif

static const ef_crc32_pclmul_ctx ef_crc32_ether_pclmul = {
    UINT64_C(0x00ccaa009e),
    UINT64_C(0x01751997d0),
    UINT64_C(0x00ccaa009e),
    UINT64_C(0x0163cd6124),
    UINT64_C(0x01f7011640),
    UINT64_C(0x01db710641)
};

static const EF_CRC_ALIGNED16 uint8_t ef_crc_xmm_shift_tab[48] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static int ef_crc_pclmul_checked;
static int ef_crc_pclmul_ok;

static __m128i ef_crc_xmm_shift_left(__m128i reg, unsigned int num)
{
    const __m128i *p = (const __m128i *)(ef_crc_xmm_shift_tab + 16U - num);

    return _mm_shuffle_epi8(reg, _mm_loadu_si128(p));
}

static __m128i ef_crc32_folding_round(__m128i data_block, __m128i precomp, __m128i fold)
{
    __m128i tmp0 = _mm_clmulepi64_si128(fold, precomp, 0x01);
    __m128i tmp1 = _mm_clmulepi64_si128(fold, precomp, 0x10);

    return _mm_xor_si128(tmp1, _mm_xor_si128(data_block, tmp0));
}

static __m128i ef_crc32_reduce_128_to_64(__m128i data128, __m128i precomp)
{
    __m128i tmp0;
    __m128i tmp1;
    __m128i tmp2;

    tmp0 = _mm_clmulepi64_si128(data128, precomp, 0x00);
    tmp1 = _mm_srli_si128(data128, 8);
    tmp0 = _mm_xor_si128(tmp0, tmp1);

    tmp2 = _mm_slli_si128(tmp0, 4);
    tmp1 = _mm_clmulepi64_si128(tmp2, precomp, 0x10);

    return _mm_xor_si128(tmp1, tmp0);
}

static uint32_t ef_crc32_reduce_64_to_32(__m128i data64, __m128i precomp)
{
    static const EF_CRC_ALIGNED16 uint32_t mask1[4] = {
        0xffffffffU, 0xffffffffU, 0x00000000U, 0x00000000U
    };
    static const EF_CRC_ALIGNED16 uint32_t mask2[4] = {
        0x00000000U, 0xffffffffU, 0xffffffffU, 0xffffffffU
    };
    __m128i tmp0;
    __m128i tmp1;
    __m128i tmp2;

    tmp0 = _mm_and_si128(data64, _mm_load_si128((const __m128i *)mask2));

    tmp1 = _mm_clmulepi64_si128(tmp0, precomp, 0x00);
    tmp1 = _mm_xor_si128(tmp1, tmp0);
    tmp1 = _mm_and_si128(tmp1, _mm_load_si128((const __m128i *)mask1));

    tmp2 = _mm_clmulepi64_si128(tmp1, precomp, 0x10);
    tmp2 = _mm_xor_si128(tmp2, tmp1);
    tmp2 = _mm_xor_si128(tmp2, tmp0);

    return (uint32_t)_mm_extract_epi32(tmp2, 2);
}

static uint32_t ef_crc32_pclmul_body(const uint8_t *data, size_t data_len, uint32_t crc,
                                     const ef_crc32_pclmul_ctx *params)
{
    __m128i temp;
    __m128i fold;
    __m128i k;
    size_t n;

    if (data == NULL || data_len == 0U || params == NULL) {
        return crc;
    }

    temp = _mm_insert_epi32(_mm_setzero_si128(), (int)crc, 0);

    if (data_len < 32U) {
        EF_CRC_ALIGNED16 uint8_t buffer[16];

        if (data_len == 16U) {
            fold = _mm_loadu_si128((const __m128i *)data);
            fold = _mm_xor_si128(fold, temp);
            goto reduction_128_64;
        }
        if (data_len < 16U) {
            memset(buffer, 0, sizeof(buffer));
            memcpy(buffer, data, data_len);
            fold = _mm_load_si128((__m128i *)buffer);
            fold = _mm_xor_si128(fold, temp);
            if (data_len < 4U) {
                fold = ef_crc_xmm_shift_left(fold, 8U - (unsigned int)data_len);
                goto barret_reduction;
            }
            fold = ef_crc_xmm_shift_left(fold, 16U - (unsigned int)data_len);
            goto reduction_128_64;
        }

        fold = _mm_loadu_si128((const __m128i *)data);
        fold = _mm_xor_si128(fold, temp);
        n = 16U;
        k = _mm_load_si128((__m128i *)(&params->rk1));
        goto partial_bytes;
    }

    fold = _mm_loadu_si128((const __m128i *)data);
    fold = _mm_xor_si128(fold, temp);

    k = _mm_load_si128((__m128i *)(&params->rk1));
    for (n = 16U; (n + 16U) <= data_len; n += 16U) {
        temp = _mm_loadu_si128((const __m128i *)&data[n]);
        fold = ef_crc32_folding_round(temp, k, fold);
    }

partial_bytes:
    if (n < data_len) {
        static const EF_CRC_ALIGNED16 uint32_t mask3[4] = {
            0x80808080U, 0x80808080U, 0x80808080U, 0x80808080U
        };
        static const EF_CRC_ALIGNED16 uint8_t shf_table[32] = {
            0x00, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
            0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };
        __m128i last16;
        __m128i a;
        __m128i b;

        last16 = _mm_loadu_si128((const __m128i *)&data[data_len - 16U]);

        temp = _mm_loadu_si128((const __m128i *)&shf_table[data_len & 15U]);
        a = _mm_shuffle_epi8(fold, temp);

        temp = _mm_xor_si128(temp, _mm_load_si128((const __m128i *)mask3));
        b = _mm_shuffle_epi8(fold, temp);
        b = _mm_blendv_epi8(b, last16, temp);

        temp = _mm_clmulepi64_si128(a, k, 0x01);
        fold = _mm_clmulepi64_si128(a, k, 0x10);

        fold = _mm_xor_si128(fold, temp);
        fold = _mm_xor_si128(fold, b);
    }

reduction_128_64:
    k = _mm_load_si128((__m128i *)(&params->rk5));
    fold = ef_crc32_reduce_128_to_64(fold, k);

barret_reduction:
    k = _mm_load_si128((__m128i *)(&params->rk7));
    return ef_crc32_reduce_64_to_32(fold, k);
}

int ef_crc32_pclmul_available(void)
{
    if (!ef_crc_pclmul_checked) {
#if defined(_MSC_VER)
        int info[4];

        __cpuid(info, 1);
        ef_crc_pclmul_ok = ((info[2] >> 1) & 1) && ((info[2] >> 20) & 1) && ((info[2] >> 9) & 1);
#else
        unsigned int eax;
        unsigned int ebx;
        unsigned int ecx;
        unsigned int edx;

        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
            ef_crc_pclmul_ok = ((ecx >> 1) & 1U) && ((ecx >> 20) & 1U) && ((ecx >> 9) & 1U);
        } else {
            ef_crc_pclmul_ok = 0;
        }
#endif
        ef_crc_pclmul_checked = 1;
    }

    return ef_crc_pclmul_ok;
}

uint32_t ef_crc32_update_pclmul(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0;
    size_t chunk;
    uint32_t out = crc;

    if (!ef_crc32_pclmul_available() || data == NULL || len == 0U) {
        return ef_crc32_update_slice4(crc, data, len);
    }

    while (len - offset >= EF_CRC_PCLMUL_MIN) {
        chunk = len - offset;
        if (chunk > 4096U) {
            chunk = 4096U;
        }
        out = ef_crc32_pclmul_body(bytes + offset, chunk, out, &ef_crc32_ether_pclmul);
        offset += chunk;
    }

    if (offset < len) {
        out = ef_crc32_update_slice4(out, bytes + offset, len - offset);
    }

    return out;
}

#else /* !x86_64 */

int ef_crc32_pclmul_available(void)
{
    return 0;
}

uint32_t ef_crc32_update_pclmul(uint32_t crc, const void *data, size_t len)
{
    return ef_crc32_update_slice4(crc, data, len);
}

#endif
