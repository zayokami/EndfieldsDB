#ifndef ENDFIELDS_EF_INTERNAL_H
#define ENDFIELDS_EF_INTERNAL_H

#include "endfields.h"

#include "ef_port.h"

/* Internal helpers exposed to the carved-out sub-modules (ef_blob, ef_queue,
 * ef_execute, ef_grow). These are NOT part of the public ABI and must be
 * consumed only by library code; user-facing apps should not include this
 * header. */

/* Error accounting. */
void ef_set_error(struct ef_db *db, enum ef_err err);
enum ef_err ef_db_require_write(struct ef_db *db);

/* Superblock metadata commit (used by every write side path). */
void ef_db_mark_meta_dirty(struct ef_db *db);
enum ef_err ef_db_commit_meta(struct ef_db *db);

/* Per-slot header atomic accessors. */
uint64_t ef_slot_next_offset_load(const struct ef_slot *slot);
void ef_slot_next_offset_store(struct ef_slot *slot, uint64_t value);
void ef_slot_status_store(struct ef_slot *slot, uint32_t status);
int ef_slot_status_has_crc(uint32_t status);
int ef_slot_status_uses_link_crc(uint32_t status);

/* Slot header CRC. */
uint32_t ef_slot_header_crc_compute_full(uint64_t slot_id, const struct ef_slot *slot);
uint32_t ef_slot_header_crc_compute_link(uint64_t slot_id, const struct ef_slot *slot);
uint32_t ef_slot_header_crc_compute(uint64_t slot_id, const struct ef_slot *slot);
void ef_slot_header_crc_store(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot);
int ef_slot_header_crc_valid(struct ef_db *db, uint64_t slot_id, const struct ef_slot *slot);

/* Persistent free-list primitives. */
enum ef_err ef_free_list_pop_atomic(struct ef_db *db, uint64_t *slot_id_out, int clear_payload);
enum ef_err ef_free_list_push_atomic(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot);

/* Slot alloc with flag control. */
enum ef_err ef_alloc_slot_ex(struct ef_db *db, uint64_t *slot_id_out, unsigned flags);

/* Slot/free-list bookkeeping. */
enum ef_err ef_claim_slot(struct ef_db *db, uint64_t slot_id);
enum ef_err ef_unlink_free_slot(struct ef_db *db, uint64_t slot_id);

/* Slot-by-id accessor that does NOT verify the header CRC. */
struct ef_slot *ef_peek_slot(struct ef_db *db, uint64_t slot_id);

/* Resolve a raw file offset to a slot pointer. Returns NULL on bad offset. */
struct ef_slot *ef_slot_at_offset(struct ef_db *db, uint64_t offset, uint64_t *slot_id_out);

/* File layout helpers shared with ef_grow. */
size_t ef_expected_file_size(uint64_t max_slots, uint32_t hash_capacity);
void ef_db_to_io(const struct ef_db *db, struct ef_io *io);
void ef_db_bind_io(struct ef_db *db, const struct ef_io *io);
void ef_db_refresh_checksums(struct ef_db *db);

/* Superblock free_count primitives. */
uint32_t ef_sb_free_count_load(const struct ef_superblock *sb);
void ef_sb_free_count_store(struct ef_superblock *sb, uint32_t value);
void ef_sb_free_count_inc(struct ef_superblock *sb);
void ef_sb_free_count_dec(struct ef_superblock *sb);

/* Hash capacity validation. */
int ef_hash_capacity_valid(uint32_t hash_capacity);

/* Public DB APIs used internally by other modules. ef_grow is called from
 * ef_alloc_ex's auto-grow path in endfields.c; ef_upgrade runs from the
 * initial open path. */
enum ef_err ef_grow(struct ef_db *db, uint64_t new_max_slots);
enum ef_err ef_upgrade(struct ef_db *db);

/* Queue spin/yield helper (used by free list and queue internals). */
void ef_queue_yield(uint32_t spins);

/* Index hash API surface (used by ef_execute). */
enum ef_err ef_index_remove_by_slot(struct ef_db *db, uint64_t slot_id);

/* Return a slot to the free pool: drop its index entry (if any) and push it
 * onto the persistent free list. Used by ef_blob chain freeing and by the
 * top-level ef_free_slot path. */
enum ef_err ef_return_slot_to_pool(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot);

#endif
