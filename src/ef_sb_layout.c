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

static volatile uint8_t *ef_sb_queue_lock_byte(struct ef_superblock *sb)
{
    return (volatile uint8_t *)&sb->reserved[EF_SB_OFF_QUEUE_LOCK_V4];
}

static volatile uint8_t *ef_sb_index_write_lock_byte(struct ef_superblock *sb)
{
    return (volatile uint8_t *)&sb->reserved[EF_SB_OFF_INDEX_WRITE_LOCK];
}

static volatile uint8_t *ef_sb_txn_lock_byte(struct ef_superblock *sb)
{
    return (volatile uint8_t *)&sb->reserved[EF_SB_OFF_TXN_LOCK];
}

static volatile uint8_t *ef_sb_txn_state_byte(struct ef_superblock *sb)
{
    return (volatile uint8_t *)&sb->reserved[EF_SB_OFF_TXN_STATE];
}

static const volatile uint8_t *ef_sb_txn_state_byte_ro(const struct ef_superblock *sb)
{
    return (const volatile uint8_t *)&sb->reserved[EF_SB_OFF_TXN_STATE];
}

static volatile uint32_t *ef_sb_index_seq_ptr(struct ef_superblock *sb)
{
    (void)sb;
    /* v5: the seqlock is in-memory; this helper is no longer used. */
    return NULL;
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
        /* Atomic 16-bit load to avoid racing with the lock-acquire CAS loop,
         * which reads the 4-byte word that contains hash_capacity. */
        uint16_t cap16 = ef_atomic_load_u16(
            (const volatile void *)&sb->reserved[EF_SB_OFF_HASH_CAP_V4]);
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

    /* Use atomic 32-bit read-modify-write on the aligned word that contains
     * hash_capacity. The index write lock byte and queue lock byte live in
     * the same 4-byte word (sb->reserved[20..23]), and the lock-acquire
     * CAS loop reads that word atomically. A plain 16-bit memcpy here would
     * race with the CAS loop's atomic load in TSAN, and on 32-bit platforms
     * the read-modify-write has to be atomic itself to avoid tearing the
     * lock bytes. The caller is expected to hold the index write lock so
     * the lock bytes remain stable for the duration of the RMW. */
    if (ef_atomic_ptr_is_aligned(&sb->reserved[EF_SB_OFF_HASH_CAP_V4],
                                 sizeof(uint32_t))) {
        volatile uint32_t *word_ptr =
            (volatile uint32_t *)&sb->reserved[EF_SB_OFF_HASH_CAP_V4];
        uint32_t cur;
        uint32_t next;

        do {
            cur = ef_atomic_load_u32((const volatile void *)word_ptr);
            next = (cur & 0xFFFF0000U) | (uint32_t)cap16;
        } while (!ef_atomic_cas_u32((volatile void *)word_ptr, &cur, next));
    } else {
        memcpy(&sb->reserved[EF_SB_OFF_HASH_CAP_V4], &cap16, sizeof(cap16));
    }
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
    /* v3 -> v4: index_seq at [24..27] was set to 0 by the memset above. */
    sb->schema_version = EF_SCHEMA_VERSION_V4;
    return EF_OK;
}

enum ef_err ef_sb_migrate_v4_txn_layout(struct ef_superblock *sb)
{
    if (sb == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (sb->schema_version >= EF_SCHEMA_VERSION) {
        return EF_OK;
    }
    if (sb->schema_version != EF_SCHEMA_VERSION_V4) {
        return EF_OK;
    }

    /* v4 -> v5: relocate the v4 index_seq [24..27] into a 4-byte zero region;
     * the actual seqlock now lives in db->index_seq (in-memory only). Zero out
     * the txn_lock and txn_state fields, then advance the schema version. */
    ef_atomic_store_u8(ef_sb_txn_lock_byte(sb), 0U);
    ef_atomic_store_u8(ef_sb_txn_state_byte(sb), 0U);
    /* Defensive: ensure [24..27] is fully zeroed (we just wrote 0/0 to 24/25
     * and the 16-bit padding at [26..27] should remain 0 from v4 init). */
    sb->reserved[26] = 0U;
    sb->reserved[27] = 0U;

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

uint32_t ef_sb_index_seq_load(const struct ef_db *db)
{
    if (db == NULL || db->sb == NULL || !ef_sb_uses_v4_index_layout(db->sb)) {
        return 0;
    }
    return ef_atomic_load_u32((const void *)&db->index_seq);
}

void ef_sb_index_write_seq_begin(struct ef_db *db)
{
    volatile uint32_t *seq_ptr;
    uint32_t seq;

    if (db == NULL || db->sb == NULL || !ef_sb_uses_v4_index_layout(db->sb)) {
        return;
    }

    seq_ptr = (volatile uint32_t *)&db->index_seq;
    seq = ef_atomic_load_u32((const void *)seq_ptr);
    ef_atomic_store_u32((void *)seq_ptr, seq + 1U);
    EF_ATOMIC_THREAD_FENCE();
}

void ef_sb_index_write_seq_end(struct ef_db *db)
{
    volatile uint32_t *seq_ptr;
    uint32_t seq;

    if (db == NULL || db->sb == NULL || !ef_sb_uses_v4_index_layout(db->sb)) {
        return;
    }

    seq_ptr = (volatile uint32_t *)&db->index_seq;
    EF_ATOMIC_THREAD_FENCE();
    seq = ef_atomic_load_u32((const void *)seq_ptr);
    ef_atomic_store_u32((void *)seq_ptr, seq + 1U);
}

int ef_sb_index_seq_read_stable(const struct ef_db *db, uint32_t seq_before)
{
    uint32_t seq_after;

    if (db == NULL || db->sb == NULL || !ef_sb_uses_v4_index_layout(db->sb)) {
        return 1;
    }

    EF_ATOMIC_THREAD_FENCE();
    seq_after = ef_sb_index_seq_load(db);
    return (seq_before == seq_after) && ((seq_after & 1U) == 0U);
}

enum ef_err ef_sb_txn_lock_try_acquire(struct ef_superblock *sb)
{
    volatile uint8_t *lock;
    uint8_t exp = 0;
    uint32_t spins = 0;

    if (sb == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (sb->schema_version < EF_SCHEMA_VERSION) {
        return EF_ERR_BAD_VERSION;
    }

    lock = ef_sb_txn_lock_byte(sb);
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
    for (;;) {
        if (++spins > EF_SB_INDEX_SPIN_MAX) {
            return EF_ERR_TXN_BUSY;
        }
        ef_sb_index_yield(spins);
        exp = 0;
        if (ef_atomic_cas_u8(lock, &exp, 1U)) {
            return EF_OK;
        }
    }
#else
    if (ef_atomic_load_u8(lock) != 0U) {
        return EF_ERR_TXN_BUSY;
    }
    ef_atomic_store_u8(lock, 1U);
    return EF_OK;
#endif
}

void ef_sb_txn_lock_release(struct ef_superblock *sb)
{
    if (sb == NULL || sb->schema_version < EF_SCHEMA_VERSION) {
        return;
    }
    ef_atomic_store_u8(ef_sb_txn_lock_byte(sb), 0U);
}

uint8_t ef_sb_txn_state_load(const struct ef_superblock *sb)
{
    if (sb == NULL || sb->schema_version < EF_SCHEMA_VERSION) {
        return 0;
    }
    return ef_atomic_load_u8((const void *)ef_sb_txn_state_byte_ro(sb));
}

void ef_sb_txn_state_store(struct ef_superblock *sb, uint8_t state)
{
    if (sb == NULL || sb->schema_version < EF_SCHEMA_VERSION) {
        return;
    }
    ef_atomic_store_u8((void *)ef_sb_txn_state_byte(sb), state);
}
