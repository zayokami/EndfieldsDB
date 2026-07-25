#include "ef_txn.h"
#include "ef_undo.h"
#include "ef_internal.h"
#include "ef_sb_layout.h"
#include "ef_atomic_unaligned.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
static uint32_t ef_txn_get_pid(void) { return (uint32_t)GetCurrentProcessId(); }
#else
#include <unistd.h>
static uint32_t ef_txn_get_pid(void) { return (uint32_t)getpid(); }
#endif

uint32_t ef_txn_current_writer_pid(void) { return ef_txn_get_pid(); }

int ef_txn_active(const struct ef_db *db)
{
    if (db == NULL) {
        return 0;
    }
    return db->txn_active != 0;
}

/* ===== Begin / Commit / Abort ===== */

enum ef_err ef_txn_begin(struct ef_db *db)
{
    enum ef_err err;
    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (db->readonly) {
        ef_set_error(db, EF_ERR_READONLY);
        return EF_ERR_READONLY;
    }
    if (db->sb == NULL || db->sb->schema_version < EF_SCHEMA_VERSION) {
        ef_set_error(db, EF_ERR_BAD_VERSION);
        return EF_ERR_BAD_VERSION;
    }
    if (db->txn_active) {
        ef_set_error(db, EF_ERR_TXN_BUSY);
        return EF_ERR_TXN_BUSY;
    }
    err = ef_sb_txn_lock_try_acquire(db->sb);
    if (err != EF_OK) {
        ef_set_error(db, err);
        return err;
    }
    /* Set state to ACTIVE before resetting the log so that ef_undo_record
     * accepts appends. */
    ef_sb_txn_state_store(db->sb, EF_TXN_STATE_ACTIVE);
    db->txn_state = EF_TXN_STATE_ACTIVE;
    db->txn_active = 1;
    db->txn_writer_pid = ef_txn_get_pid();
    ef_undo_reset(db);
    ef_db_mark_meta_dirty(db);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

enum ef_err ef_txn_commit(struct ef_db *db)
{
    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (!db->txn_active) {
        /* No-op: committing outside a transaction is allowed. */
        ef_set_error(db, EF_OK);
        return EF_OK;
    }
    db->txn_state = EF_TXN_STATE_NONE;
    db->txn_active = 0;
    db->txn_writer_pid = EF_TXN_WRITER_NONE;
    ef_undo_reset(db);
    ef_sb_txn_state_store(db->sb, EF_TXN_STATE_NONE);
    ef_sb_txn_lock_release(db->sb);
    ef_db_mark_meta_dirty(db);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

enum ef_err ef_txn_abort(struct ef_db *db)
{
    enum ef_err err;
    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (!db->txn_active) {
        ef_set_error(db, EF_OK);
        return EF_OK;
    }
    db->txn_state = EF_TXN_STATE_ABORTING;
    ef_sb_txn_state_store(db->sb, EF_TXN_STATE_ABORTING);
    err = ef_undo_replay_reverse(db);
    db->txn_state = EF_TXN_STATE_NONE;
    db->txn_active = 0;
    db->txn_writer_pid = EF_TXN_WRITER_NONE;
    ef_undo_reset(db);
    ef_sb_txn_state_store(db->sb, EF_TXN_STATE_NONE);
    ef_sb_txn_lock_release(db->sb);
    ef_db_mark_meta_dirty(db);
    ef_set_error(db, err);
    return err;
}

enum ef_err ef_txn_recover_from_stale_lock(struct ef_db *db)
{
    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (db->sb == NULL || db->sb->schema_version < EF_SCHEMA_VERSION) {
        return EF_OK;
    }
    if (ef_sb_txn_state_load(db->sb) != EF_TXN_STATE_ACTIVE) {
        return EF_OK;
    }
    /* Active transaction detected. Replay then clear. */
    db->txn_state = EF_TXN_STATE_ABORTING;
    ef_set_error(db, EF_OK);
    {
        enum ef_err err = ef_undo_replay_reverse(db);
        ef_undo_reset(db);
        ef_sb_txn_state_store(db->sb, EF_TXN_STATE_NONE);
        ef_sb_txn_lock_release(db->sb);
        db->txn_state = EF_TXN_STATE_NONE;
        ef_db_mark_meta_dirty(db);
        return err;
    }
}

/* ===== Per-kind undo record helpers ===== */

enum ef_err ef_txn_record_slot_status(struct ef_db *db, uint64_t slot_id,
                                      uint32_t prior_status, uint32_t prior_crc)
{
    uint8_t before[12];
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    memset(before, 0, sizeof(before));
    memcpy(before, &prior_status, sizeof(prior_status));
    memcpy(before + 4, &prior_crc, sizeof(prior_crc));
    return ef_undo_record_append(db, EF_UNDO_KIND_SLOT_STATUS, ef_slot_to_offset(db, slot_id), 0,
                                 before);
}

enum ef_err ef_txn_record_slot_next(struct ef_db *db, uint64_t slot_id, uint64_t prior_next)
{
    uint8_t before[12];
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    memset(before, 0, sizeof(before));
    memcpy(before, &prior_next, sizeof(prior_next));
    return ef_undo_record_append(db, EF_UNDO_KIND_SLOT_NEXT, ef_slot_to_offset(db, slot_id), 0,
                                 before);
}

enum ef_err ef_txn_record_slot_payload(struct ef_db *db, uint64_t slot_id,
                                       const void *prior_payload_first8)
{
    uint8_t before[12];
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    memset(before, 0, sizeof(before));
    if (prior_payload_first8 != NULL) {
        memcpy(before, prior_payload_first8, 8);
    }
    return ef_undo_record_append(db, EF_UNDO_KIND_SLOT_PAYLOAD, ef_slot_to_offset(db, slot_id), 0,
                                 before);
}

enum ef_err ef_txn_record_slot_alloc(struct ef_db *db, uint64_t slot_id,
                                     uint32_t prior_status, uint32_t prior_crc)
{
    uint8_t before[12];
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    memset(before, 0, sizeof(before));
    memcpy(before, &prior_status, sizeof(prior_status));
    memcpy(before + 4, &prior_crc, sizeof(prior_crc));
    return ef_undo_record_append(db, EF_UNDO_KIND_SLOT_ALLOC, ef_slot_to_offset(db, slot_id), 0,
                                 before);
}

enum ef_err ef_txn_record_slot_free(struct ef_db *db, uint64_t slot_id,
                                    uint64_t prior_next, uint32_t prior_status)
{
    uint8_t before[12];
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    memset(before, 0, sizeof(before));
    memcpy(before, &prior_next, sizeof(prior_next));
    /* aux carries the prior status, since before is only 12 bytes and we
     * need both next (8B) and status (4B). */
    return ef_undo_record_append(db, EF_UNDO_KIND_SLOT_FREE, ef_slot_to_offset(db, slot_id),
                                 (uint64_t)prior_status, before);
}

enum ef_err ef_txn_record_index_put(struct ef_db *db, uint64_t key_hash, uint64_t slot_offset)
{
    uint8_t before[12];
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    memset(before, 0, sizeof(before));
    memcpy(before, &key_hash, sizeof(key_hash));
    return ef_undo_record_append(db, EF_UNDO_KIND_INDEX_PUT, slot_offset, 0, before);
}

enum ef_err ef_txn_record_index_remove(struct ef_db *db, uint64_t key_hash,
                                       uint64_t slot_offset)
{
    uint8_t before[16];
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    memset(before, 0, sizeof(before));
    memcpy(before, &key_hash, sizeof(key_hash));
    memcpy(before + 8, &slot_offset, sizeof(slot_offset));
    return ef_undo_record_append(db, EF_UNDO_KIND_INDEX_REMOVE, slot_offset, 0, before);
}

enum ef_err ef_txn_record_queue_push(struct ef_db *db, uint64_t new_node_offset,
                                    uint64_t saved_tail_next)
{
    uint8_t before[16];
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    memset(before, 0, sizeof(before));
    memcpy(before, &saved_tail_next, sizeof(saved_tail_next));
    return ef_undo_record_append(db, EF_UNDO_KIND_QUEUE_PUSH, new_node_offset, 0, before);
}

enum ef_err ef_txn_record_queue_pop(struct ef_db *db, uint64_t popped_node_offset,
                                    uint64_t saved_dummy_next, uint64_t saved_tail,
                                    uint64_t saved_node_next)
{
    uint8_t before[16];
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    memset(before, 0, sizeof(before));
    memcpy(before, &saved_dummy_next, sizeof(saved_dummy_next));
    memcpy(before + 8, &saved_tail, sizeof(saved_tail));
    return ef_undo_record_append(db, EF_UNDO_KIND_QUEUE_POP, popped_node_offset,
                                 saved_node_next, before);
}

enum ef_err ef_txn_record_free_count(struct ef_db *db, int64_t delta)
{
    if (!ef_txn_active(db)) {
        return EF_OK;
    }
    return ef_undo_record_append(db, EF_UNDO_KIND_FREE_COUNT, 0, (uint64_t)delta, NULL);
}
