#include "ef_sb_layout.h"
#include "ef_atomic_unaligned.h"
#include "ef_config.h"

#include <stddef.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sched.h>
#endif

static void ef_sb_index_yield(uint32_t spins)
{
    if (spins < 64U) {
        return;
    }
#ifdef _WIN32
    if (spins < 4096U) {
        YieldProcessor();
        return;
    }
    Sleep(0);
#else
    (void)sched_yield();
#endif
}

#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
static void ef_atomic_store_u8(volatile void *ptr, uint8_t value)
{
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t align_off = addr & (uintptr_t)3U;
    volatile void *word_ptr = (volatile void *)(addr - align_off);
    uint32_t exp_word;
    uint32_t des_word;

    exp_word = ef_atomic_load_u32((const void *)word_ptr);
    for (;;) {
        des_word = (exp_word & ~((uint32_t)0xFFU << (align_off * 8U))) |
                   ((uint32_t)value << (align_off * 8U));
        if (ef_atomic_cas_u32(word_ptr, &exp_word, des_word)) {
            return;
        }
    }
}

static int ef_atomic_cas_u8(volatile void *ptr, uint8_t *expected, uint8_t desired)
{
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t align_off = addr & (uintptr_t)3U;
    volatile void *word_ptr = (volatile void *)(addr - align_off);
    uint32_t exp_word;
    uint32_t des_word;

    exp_word = ef_atomic_load_u32((const void *)word_ptr);
    for (;;) {
        uint8_t cur_byte = (uint8_t)((exp_word >> (align_off * 8U)) & 0xFFU);

        if (cur_byte != *expected) {
            *expected = cur_byte;
            return 0;
        }
        des_word = (exp_word & ~((uint32_t)0xFFU << (align_off * 8U))) |
                   ((uint32_t)desired << (align_off * 8U));
        if (ef_atomic_cas_u32(word_ptr, &exp_word, des_word)) {
            return 1;
        }
    }
}
#else
static uint8_t ef_atomic_load_u8(const volatile void *ptr)
{
    uint8_t value;

    memcpy(&value, (const void *)ptr, sizeof(value));
    EF_ATOMIC_THREAD_FENCE();
    return value;
}

static void ef_atomic_store_u8(volatile void *ptr, uint8_t value)
{
    EF_ATOMIC_THREAD_FENCE();
    memcpy((void *)ptr, (const void *)&value, sizeof(value));
}

static int ef_atomic_cas_u8(volatile void *ptr, uint8_t *expected, uint8_t desired)
{
    uint8_t cur;

    for (;;) {
        memcpy(&cur, (const void *)ptr, sizeof(cur));
        if (cur != *expected) {
            *expected = cur;
            return 0;
        }
        memcpy((void *)ptr, (const void *)&desired, sizeof(desired));
        EF_ATOMIC_THREAD_FENCE();
        memcpy(&cur, (const void *)ptr, sizeof(cur));
        if (cur == desired) {
            return 1;
        }
        *expected = cur;
    }
}
#endif

static volatile uint8_t *ef_sb_queue_lock_byte(struct ef_superblock *sb)
{
    return (volatile uint8_t *)&sb->reserved[EF_SB_OFF_QUEUE_LOCK_V4];
}

static volatile uint8_t *ef_sb_index_write_lock_byte(struct ef_superblock *sb)
{
    return (volatile uint8_t *)&sb->reserved[EF_SB_OFF_INDEX_WRITE_LOCK];
}

static volatile uint32_t *ef_sb_index_seq_ptr(struct ef_superblock *sb)
{
    return (volatile uint32_t *)&sb->reserved[EF_SB_OFF_INDEX_SEQ];
}

static int ef_sb_uses_v4_index_layout(const struct ef_superblock *sb)
{
    return sb != NULL && sb->schema_version >= EF_SCHEMA_VERSION;
}

uint32_t ef_sb_hash_capacity_load(const struct ef_superblock *sb)
{
    if (sb == NULL) {
        return 0;
    }

    if (ef_sb_uses_v4_index_layout(sb)) {
        uint16_t cap16;

        memcpy(&cap16, &sb->reserved[EF_SB_OFF_HASH_CAP_V4], sizeof(cap16));
        return (uint32_t)cap16;
    }

    if (sb->schema_version >= EF_SCHEMA_VERSION_V3) {
        uint32_t cap32;

        memcpy(&cap32, &sb->reserved[EF_SB_OFF_HASH_CAP_V3], sizeof(cap32));
        return cap32;
    }

    return 0;
}

void ef_sb_hash_capacity_store(struct ef_superblock *sb, uint32_t hash_capacity)
{
    uint16_t cap16;

    if (sb == NULL) {
        return;
    }

    if (hash_capacity > 0xFFFFU) {
        hash_capacity = 0xFFFFU;
    }

    cap16 = (uint16_t)hash_capacity;
    memcpy(&sb->reserved[EF_SB_OFF_HASH_CAP_V4], &cap16, sizeof(cap16));
}

enum ef_err ef_sb_migrate_v3_index_layout(struct ef_superblock *sb)
{
    uint32_t hash32;
    uint32_t queue_lock32;
    uint8_t queue_lock8;

    if (sb == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (sb->schema_version >= EF_SCHEMA_VERSION) {
        return EF_OK;
    }
    if (sb->schema_version < EF_SCHEMA_VERSION_V3) {
        return EF_OK;
    }

    memcpy(&hash32, &sb->reserved[EF_SB_OFF_HASH_CAP_V3], sizeof(hash32));
    if (hash32 > 0xFFFFU) {
        return EF_ERR_BAD_VERSION;
    }

    memcpy(&queue_lock32, &sb->reserved[EF_SB_OFF_QUEUE_LOCK_V3], sizeof(queue_lock32));
    queue_lock8 = (uint8_t)(queue_lock32 != 0U ? 1U : 0U);

    memset(&sb->reserved[EF_SB_OFF_HASH_CAP_V4], 0, 8U);
    ef_sb_hash_capacity_store(sb, hash32);
    ef_atomic_store_u8(ef_sb_queue_lock_byte(sb), queue_lock8);
    ef_atomic_store_u8(ef_sb_index_write_lock_byte(sb), 0);
    ef_atomic_store_u32(ef_sb_index_seq_ptr(sb), 0);

    sb->schema_version = EF_SCHEMA_VERSION;
    return EF_OK;
}

enum ef_err ef_sb_queue_lock_acquire(struct ef_superblock *sb)
{
    volatile uint8_t *lock;
    uint8_t exp = 0;
    uint32_t spins = 0;

    if (sb == NULL) {
        return EF_ERR_NULL_ARG;
    }

    if (!ef_sb_uses_v4_index_layout(sb)) {
        volatile uint32_t *lock32 = (volatile uint32_t *)&sb->reserved[EF_SB_OFF_QUEUE_LOCK_V3];
        uint32_t exp32 = 0;

#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
        for (;;) {
            if (++spins > EF_SB_INDEX_SPIN_MAX) {
                return EF_ERR_QUEUE_BUSY;
            }
            ef_sb_index_yield(spins);
            exp32 = 0;
            if (ef_atomic_cas_u32((volatile void *)lock32, &exp32, 1U)) {
                return EF_OK;
            }
        }
#else
        if (*lock32 != 0U) {
            return EF_ERR_QUEUE_BUSY;
        }
        *lock32 = 1U;
        return EF_OK;
#endif
    }

    lock = ef_sb_queue_lock_byte(sb);
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
    for (;;) {
        if (++spins > EF_SB_INDEX_SPIN_MAX) {
            return EF_ERR_QUEUE_BUSY;
        }
        ef_sb_index_yield(spins);
        exp = 0;
        if (ef_atomic_cas_u8(lock, &exp, 1U)) {
            return EF_OK;
        }
    }
#else
    if (ef_atomic_load_u8(lock) != 0U) {
        return EF_ERR_QUEUE_BUSY;
    }
    ef_atomic_store_u8(lock, 1U);
    return EF_OK;
#endif
}

void ef_sb_queue_lock_release(struct ef_superblock *sb)
{
    if (sb == NULL) {
        return;
    }

    if (!ef_sb_uses_v4_index_layout(sb)) {
        ef_atomic_store_u32((volatile void *)&sb->reserved[EF_SB_OFF_QUEUE_LOCK_V3], 0U);
        return;
    }

    ef_atomic_store_u8(ef_sb_queue_lock_byte(sb), 0U);
}

enum ef_err ef_sb_index_write_lock_acquire(struct ef_superblock *sb)
{
    volatile uint8_t *lock;
    uint8_t exp = 0;
    uint32_t spins = 0;

    if (sb == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (!ef_sb_uses_v4_index_layout(sb)) {
        return EF_ERR_BAD_VERSION;
    }

    lock = ef_sb_index_write_lock_byte(sb);
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
    for (;;) {
        if (++spins > EF_SB_INDEX_SPIN_MAX) {
            return EF_ERR_INDEX_BUSY;
        }
        ef_sb_index_yield(spins);
        exp = 0;
        if (ef_atomic_cas_u8(lock, &exp, 1U)) {
            return EF_OK;
        }
    }
#else
    if (ef_atomic_load_u8(lock) != 0U) {
        return EF_ERR_INDEX_BUSY;
    }
    ef_atomic_store_u8(lock, 1U);
    return EF_OK;
#endif
}

void ef_sb_index_write_lock_release(struct ef_superblock *sb)
{
    if (sb == NULL || !ef_sb_uses_v4_index_layout(sb)) {
        return;
    }

    ef_atomic_store_u8(ef_sb_index_write_lock_byte(sb), 0U);
}

uint32_t ef_sb_index_seq_load(const struct ef_superblock *sb)
{
    if (sb == NULL || !ef_sb_uses_v4_index_layout(sb)) {
        return 0;
    }

    return ef_atomic_load_u32((const void *)&sb->reserved[EF_SB_OFF_INDEX_SEQ]);
}

void ef_sb_index_write_seq_begin(struct ef_superblock *sb)
{
    volatile uint32_t *seq_ptr;
    uint32_t seq;

    if (sb == NULL || !ef_sb_uses_v4_index_layout(sb)) {
        return;
    }

    seq_ptr = ef_sb_index_seq_ptr(sb);
    seq = ef_atomic_load_u32((const void *)seq_ptr);
    ef_atomic_store_u32(seq_ptr, seq + 1U);
    EF_ATOMIC_THREAD_FENCE();
}

void ef_sb_index_write_seq_end(struct ef_superblock *sb)
{
    volatile uint32_t *seq_ptr;
    uint32_t seq;

    if (sb == NULL || !ef_sb_uses_v4_index_layout(sb)) {
        return;
    }

    seq_ptr = ef_sb_index_seq_ptr(sb);
    EF_ATOMIC_THREAD_FENCE();
    seq = ef_atomic_load_u32((const void *)seq_ptr);
    ef_atomic_store_u32(seq_ptr, seq + 1U);
}

int ef_sb_index_seq_read_stable(const struct ef_superblock *sb, uint32_t seq_before)
{
    uint32_t seq_after;

    if (sb == NULL || !ef_sb_uses_v4_index_layout(sb)) {
        return 1;
    }

    EF_ATOMIC_THREAD_FENCE();
    seq_after = ef_sb_index_seq_load(sb);
    return (seq_before == seq_after) && ((seq_after & 1U) == 0U);
}
