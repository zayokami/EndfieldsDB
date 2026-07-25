#ifndef EF_SB_LAYOUT_H
#define EF_SB_LAYOUT_H

#include "endfields.h"

/* Superblock reserved[28] layout (schema v5):
 *   [0..3]   sb_checksum
 *   [4..11]  queue_head (u64)
 *   [12..19] queue_tail (u64)
 *   [20..21] hash_capacity (u16)
 *   [22]     queue_lock (u8)
 *   [23]     index_write_lock (u8)
 *   [24]     txn_lock (u8)             -- v5: per-DB transaction lock
 *   [25]     txn_state (u8)            -- v5: 0=NONE, 1=ACTIVE, 2=ABORTING
 *   [26..27] padding                   -- v5 (was v4 index_seq; relocated to undo log header)
 *
 * Schema v3 used u32 hash_capacity at [20..23] and u32 queue_lock at [24..27].
 * Schema v4 used [20..21] hash_capacity u16, [22] queue_lock u8, [23] index_write_lock u8,
 * and [24..27] index_seq u32. The v4 index_seq field is moved to the undo log header in v5
 * for compactness; v4 seqlock semantics are preserved by ef_sb_index_seq_* migration helpers.
 */

#define EF_SB_OFF_QUEUE_HEAD       4U
#define EF_SB_OFF_QUEUE_TAIL       12U
#define EF_SB_OFF_HASH_CAP_V4      20U
#define EF_SB_OFF_QUEUE_LOCK_V4    22U
#define EF_SB_OFF_INDEX_WRITE_LOCK 23U
#define EF_SB_OFF_TXN_LOCK         24U
#define EF_SB_OFF_TXN_STATE        25U
#define EF_SB_OFF_HASH_CAP_V3      20U
#define EF_SB_OFF_QUEUE_LOCK_V3    24U

#define EF_SB_INDEX_SPIN_MAX 65536U
#define EF_SB_INDEX_SEQ_READ_MAX 4096U

uint32_t ef_sb_hash_capacity_load(const struct ef_superblock *sb);
void ef_sb_hash_capacity_store(struct ef_superblock *sb, uint32_t hash_capacity);

enum ef_err ef_sb_migrate_v3_index_layout(struct ef_superblock *sb);
/* v3/v4 -> v5 reserved layout migration. v3 path calls ef_sb_migrate_v3_index_layout
 * first. Idempotent: no-op if already v5. */
enum ef_err ef_sb_migrate_v4_txn_layout(struct ef_superblock *sb);

enum ef_err ef_sb_queue_lock_acquire(struct ef_superblock *sb);
void ef_sb_queue_lock_release(struct ef_superblock *sb);

enum ef_err ef_sb_index_write_lock_acquire(struct ef_superblock *sb);
void ef_sb_index_write_lock_release(struct ef_superblock *sb);

void ef_sb_index_write_seq_begin(struct ef_db *db);
void ef_sb_index_write_seq_end(struct ef_db *db);

uint32_t ef_sb_index_seq_load(const struct ef_db *db);

/* Returns 1 if a consistent read window was observed, 0 to retry. */
int ef_sb_index_seq_read_stable(const struct ef_db *db, uint32_t seq_before);

/* v5 transaction lock: independent u8 spinlock. Use ef_txn_try_acquire/release
 * for high-level state-machine access; raw accessors are exposed for
 * crash-recovery probing at open time. */
enum ef_err ef_sb_txn_lock_try_acquire(struct ef_superblock *sb);
void ef_sb_txn_lock_release(struct ef_superblock *sb);
uint8_t ef_sb_txn_state_load(const struct ef_superblock *sb);
void ef_sb_txn_state_store(struct ef_superblock *sb, uint8_t state);

#endif
