#ifndef EF_TXN_H
#define EF_TXN_H

#include "endfields.h"

/* See endfields.h for the public API. This header is the internal entry point
 * for the index/queue/slot subsystems to call into the transaction machinery
 * for undo recording and grow/rehash gating. */

struct ef_undo_kind_tag;

/* Append a slot-status change record (e.g. USED -> OVERFLOW). */
enum ef_err ef_txn_record_slot_status(struct ef_db *db, uint64_t slot_id,
                                      uint32_t prior_status, uint32_t prior_crc);
/* Append a slot-next-offset change record. */
enum ef_err ef_txn_record_slot_next(struct ef_db *db, uint64_t slot_id,
                                    uint64_t prior_next);
/* Append a slot payload write record. only the first 8 bytes of the prior
 * payload are recorded. */
enum ef_err ef_txn_record_slot_payload(struct ef_db *db, uint64_t slot_id,
                                       const void *prior_payload_first8);
/* Append a slot-alloc record. The slot's prior status/crc are read here. */
enum ef_err ef_txn_record_slot_alloc(struct ef_db *db, uint64_t slot_id,
                                     uint32_t prior_status, uint32_t prior_crc);
/* Append a slot-free record. The slot's prior next_offset and status are
 * read here. */
enum ef_err ef_txn_record_slot_free(struct ef_db *db, uint64_t slot_id,
                                    uint64_t prior_next, uint32_t prior_status);
/* Append an index-put record. */
enum ef_err ef_txn_record_index_put(struct ef_db *db, uint64_t key_hash,
                                    uint64_t slot_offset);
/* Append an index-remove record. */
enum ef_err ef_txn_record_index_remove(struct ef_db *db, uint64_t key_hash,
                                       uint64_t prior_slot_offset);
/* Append a queue-push record. */
enum ef_err ef_txn_record_queue_push(struct ef_db *db, uint64_t new_node_offset,
                                     uint64_t saved_tail_next);
/* Append a queue-pop record. */
enum ef_err ef_txn_record_queue_pop(struct ef_db *db, uint64_t popped_node_offset,
                                    uint64_t saved_dummy_next, uint64_t saved_tail,
                                    uint64_t saved_node_next);
/* Append a free-count change record. */
enum ef_err ef_txn_record_free_count(struct ef_db *db, int64_t delta);

/* Recover from a stale ACTIVE state observed at open. Replays the undo log
 * and clears the transaction lock. Returns the result of the replay. */
enum ef_err ef_txn_recover_from_stale_lock(struct ef_db *db);

/* Internal: get the writer pid field for the current process. */
uint32_t ef_txn_current_writer_pid(void);

#endif
