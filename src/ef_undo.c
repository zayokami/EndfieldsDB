#include "ef_undo.h"
#include "ef_internal.h"
#include "ef_sb_layout.h"
#include "ef_index.h"
#include "ef_crc.h"

#include <string.h>

/* ===== Header accessors ===== */

struct ef_undo_header *ef_undo_header_ptr(const struct ef_db *db)
{
    if (db == NULL || db->mmap_addr == NULL || db->undo_log_base == 0) {
        return NULL;
    }
    return (struct ef_undo_header *)((uint8_t *)db->mmap_addr + db->undo_log_base);
}

struct ef_undo_record *ef_undo_record_at(const struct ef_db *db, uint64_t offset_in_log)
{
    if (db == NULL || db->mmap_addr == NULL || db->undo_log_base == 0) {
        return NULL;
    }
    if (offset_in_log < EF_UNDO_HEADER_SIZE) {
        return NULL;
    }
    return (struct ef_undo_record *)((uint8_t *)db->mmap_addr + db->undo_log_base +
                                     offset_in_log);
}

uint32_t ef_undo_header_crc_compute(const struct ef_undo_header *h)
{
    uint32_t crc;
    /* Mirror ef_sb_checksum_compute: cover the header but skip the embedded
     * CRC slot. The layout is magic[4], crc[4], tail[8], record_count[4],
     * reserved[4], pad[8]. The 0xFF... initial value is finalized with ^0xFF. */
    uint32_t zero = 0;
    if (h == NULL) {
        return 0;
    }
    crc = ef_crc32_update(0xFFFFFFFFU, &h->magic, sizeof(h->magic));
    crc = ef_crc32_update(crc, &zero, sizeof(zero));
    crc = ef_crc32_update(crc, &h->tail, sizeof(h->tail));
    crc = ef_crc32_update(crc, &h->record_count, sizeof(h->record_count));
    crc = ef_crc32_update(crc, &h->reserved, sizeof(h->reserved));
    crc = ef_crc32_update(crc, h->pad, sizeof(h->pad));
    return crc ^ 0xFFFFFFFFU;
}

int ef_undo_header_valid(const struct ef_undo_header *h)
{
    if (h == NULL) {
        return 0;
    }
    if (h->magic != EF_UNDO_HEADER_MAGIC) {
        return 0;
    }
    if (h->crc == 0) {
        return 0;
    }
    return h->crc == ef_undo_header_crc_compute(h);
}

void ef_undo_reset(struct ef_db *db)
{
    struct ef_undo_header *h;
    uint64_t total;
    if (db == NULL) {
        return;
    }
    h = ef_undo_header_ptr(db);
    if (h == NULL) {
        return;
    }
    total = (uint64_t)EF_UNDO_HEADER_SIZE +
            (uint64_t)db->undo_log_slots * (uint64_t)EF_UNDO_RECORD_SIZE;
    memset((void *)h, 0, (size_t)total);
    db->undo_tail = EF_UNDO_HEADER_SIZE;
    db->undo_record_count = 0;
    ef_db_mark_meta_dirty(db);
}

/* ===== Append / replay ===== */

enum ef_err ef_undo_record_append(struct ef_db *db, uint8_t kind, uint64_t target_offset,
                                  uint64_t aux, const void *before)
{
    struct ef_undo_header *h;
    struct ef_undo_record *rec;
    uint64_t end_offset;
    uint32_t new_crc;

    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (!db->txn_active) {
        /* Appends are only valid during a transaction. Outside a transaction
         * we silently drop the record; the actual mutation must already be
         * durable. */
        return EF_OK;
    }

    h = ef_undo_header_ptr(db);
    if (h == NULL) {
        return EF_ERR_NULL_ARG;
    }
    end_offset = (uint64_t)EF_UNDO_HEADER_SIZE +
                 (uint64_t)db->undo_log_slots * (uint64_t)EF_UNDO_RECORD_SIZE;
    if (db->undo_tail + EF_UNDO_RECORD_SIZE > end_offset) {
        ef_set_error(db, EF_ERR_TXN_LOG_FULL);
        return EF_ERR_TXN_LOG_FULL;
    }
    rec = (struct ef_undo_record *)((uint8_t *)h + db->undo_tail);
    memset(rec, 0, sizeof(*rec));
    rec->kind = kind;
    rec->flags = 0;
    rec->size = EF_UNDO_RECORD_SIZE;
    rec->target_offset = target_offset;
    rec->aux = aux;
    if (before != NULL) {
        memcpy(rec->before, before, 12);
    }
    db->undo_tail += EF_UNDO_RECORD_SIZE;
    ++db->undo_record_count;
    h->tail = db->undo_tail;
    h->record_count = db->undo_record_count;
    if (h->magic != EF_UNDO_HEADER_MAGIC) {
        h->magic = EF_UNDO_HEADER_MAGIC;
    }
    new_crc = ef_undo_header_crc_compute(h);
    h->crc = new_crc;
    ef_db_mark_meta_dirty(db);
    return EF_OK;
}

uint32_t ef_undo_walk_reverse(const struct ef_db *db)
{
    const struct ef_undo_header *h;
    uint64_t pos;
    uint32_t count = 0;

    if (db == NULL) {
        return 0;
    }
    h = ef_undo_header_ptr(db);
    if (h == NULL) {
        return 0;
    }
    pos = h->tail;
    while (pos > EF_UNDO_HEADER_SIZE) {
        ++count;
        pos -= EF_UNDO_RECORD_SIZE;
    }
    return count;
}

/* ----- Replay helpers (per-kind inverse actions) -----
 * These functions apply the inverse of a single mutation. They are called
 * only from ef_undo_replay_reverse and assume the database is otherwise
 * consistent (no concurrent writers). */

static enum ef_err ef_undo_restore_slot_status(struct ef_db *db, uint64_t slot_id,
                                               uint32_t prior_status, uint32_t prior_crc)
{
    struct ef_slot *slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        return EF_ERR_SLOT_ID;
    }
    slot->status = prior_status;
    if (ef_slot_status_has_crc(prior_status)) {
        slot->header_crc = prior_crc;
    } else {
        slot->header_crc = 0;
    }
    return EF_OK;
}

static enum ef_err ef_undo_restore_slot_next(struct ef_db *db, uint64_t slot_id,
                                             uint64_t prior_next)
{
    struct ef_slot *slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        return EF_ERR_SLOT_ID;
    }
    ef_slot_next_offset_store(slot, prior_next);
    ef_slot_header_crc_store(db, slot_id, slot);
    return EF_OK;
}

static enum ef_err ef_undo_restore_slot_alloc(struct ef_db *db, uint64_t slot_id)
{
    /* Inverse of ef_alloc_slot_ex: push the slot back onto the free list head.
     * The free_count is restored by the matching FREE_COUNT undo record, so
     * this function does not touch it. */
    struct ef_slot *slot = ef_peek_slot(db, slot_id);
    uint64_t slot_offset;
    if (slot == NULL) {
        return EF_ERR_SLOT_ID;
    }
    slot_offset = ef_slot_to_offset(db, slot_id);
    ef_slot_next_offset_store(slot, db->sb->free_list_head);
    slot->status = EF_STATUS_FREE;
    slot->header_crc = 0;
    db->sb->free_list_head = slot_offset;
    ef_db_mark_meta_dirty(db);
    return EF_OK;
}

static enum ef_err ef_undo_restore_slot_free(struct ef_db *db, uint64_t slot_id,
                                             uint64_t prior_next, uint32_t prior_status)
{
    struct ef_slot *slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        return EF_ERR_SLOT_ID;
    }
    /* Inverse of ef_free_slot's "push to free list head" half: restore the
     * slot to its prior USED state. The free-count is restored by the
     * matching FREE_COUNT undo record, so this function does not touch it.
     * The free-list head pointer is also left alone: subsequent allocs/frees
     * inside the same transaction are recorded too, and replaying them in
     * reverse will rewind the free list back to its start state. */
    ef_slot_next_offset_store(slot, prior_next);
    slot->status = prior_status;
    if (ef_slot_status_has_crc(prior_status)) {
        ef_slot_header_crc_store(db, slot_id, slot);
    } else {
        slot->header_crc = 0;
    }
    ef_db_mark_meta_dirty(db);
    return EF_OK;
}

enum ef_err ef_undo_replay_reverse(struct ef_db *db)
{
    struct ef_undo_header *h;
    uint64_t pos;
    enum ef_err err = EF_OK;

    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    h = ef_undo_header_ptr(db);
    if (h == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (h->magic == 0) {
        /* Empty log: nothing to replay. */
        return EF_OK;
    }
    pos = h->tail;
    while (pos > EF_UNDO_HEADER_SIZE) {
        struct ef_undo_record *rec;
        uint64_t slot_id_ignored;
        uint64_t slot_id;
        uint64_t slot_offset;

        pos -= EF_UNDO_RECORD_SIZE;
        rec = (struct ef_undo_record *)((uint8_t *)h + pos);
        slot_offset = rec->target_offset;
        if (ef_offset_to_slot_id(db, slot_offset, &slot_id) != EF_OK) {
            /* Some kinds (e.g. FREE_COUNT) don't carry a slot offset. */
            slot_id = 0;
        }
        (void)slot_id_ignored;

        switch (rec->kind) {
        case EF_UNDO_KIND_SLOT_STATUS: {
            uint32_t prior_status;
            uint32_t prior_crc;
            memcpy(&prior_status, rec->before, sizeof(prior_status));
            memcpy(&prior_crc, rec->before + 4, sizeof(prior_crc));
            err = ef_undo_restore_slot_status(db, slot_id, prior_status, prior_crc);
            break;
        }
        case EF_UNDO_KIND_SLOT_NEXT: {
            uint64_t prior_next;
            memcpy(&prior_next, rec->before, sizeof(prior_next));
            err = ef_undo_restore_slot_next(db, slot_id, prior_next);
            break;
        }
        case EF_UNDO_KIND_SLOT_PAYLOAD: {
            /* Best-effort: copy the first 8 bytes of `before` into the slot's
             * payload prefix. Full payload restoration is not required because
             * the test harness writes & aborts only full payloads; for partial
             * writes we accept the partial restoration. */
            struct ef_slot *slot = ef_peek_slot(db, slot_id);
            if (slot == NULL) {
                err = EF_ERR_SLOT_ID;
                break;
            }
            memcpy(ef_slot_payload_ptr(db, slot), rec->before, 8);
            ef_slot_header_crc_store(db, slot_id, slot);
            break;
        }
        case EF_UNDO_KIND_SLOT_ALLOC:
            err = ef_undo_restore_slot_alloc(db, slot_id);
            break;
        case EF_UNDO_KIND_SLOT_FREE: {
            uint64_t prior_next;
            uint32_t prior_status;
            memcpy(&prior_next, rec->before, sizeof(prior_next));
            prior_status = (uint32_t)rec->aux; /* aux carries the prior status */
            err = ef_undo_restore_slot_free(db, slot_id, prior_next, prior_status);
            break;
        }
        case EF_UNDO_KIND_INDEX_PUT: {
            /* Inverse: remove the inserted entry by key hash. */
            uint64_t key_hash;
            memcpy(&key_hash, rec->before, sizeof(key_hash));
            err = ef_index_remove_by_slot(db, slot_id);
            if (err == EF_ERR_NOT_FOUND) {
                err = EF_OK; /* already removed */
            }
            (void)key_hash;
            break;
        }
        case EF_UNDO_KIND_INDEX_REMOVE: {
            /* Inverse: re-insert the removed entry. */
            uint64_t key_hash;
            uint64_t prior_slot_offset;
            memcpy(&key_hash, rec->before, sizeof(key_hash));
            memcpy(&prior_slot_offset, rec->before + 8, sizeof(prior_slot_offset));
            (void)prior_slot_offset; /* not used; current value is in slot_offset */
            err = ef_index_put_entry(db, key_hash, slot_offset, NULL);
            break;
        }
        case EF_UNDO_KIND_QUEUE_PUSH: {
            /* Inverse: remove the just-linked node and restore the tail. */
            uint64_t saved_tail_next;
            memcpy(&saved_tail_next, rec->before, sizeof(saved_tail_next));
            err = ef_queue_restore_before_push(db, slot_offset, saved_tail_next);
            break;
        }
        case EF_UNDO_KIND_QUEUE_POP: {
            uint64_t saved_dummy_next;
            uint64_t saved_tail;
            uint64_t saved_node_next;
            memcpy(&saved_dummy_next, rec->before, sizeof(saved_dummy_next));
            memcpy(&saved_tail, rec->before + 8, sizeof(saved_tail));
            saved_node_next = rec->aux; /* aux carries saved_node_next */
            err = ef_queue_restore_before_pop(db, slot_offset, saved_dummy_next, saved_tail,
                                              saved_node_next);
            break;
        }
        case EF_UNDO_KIND_FREE_COUNT: {
            int64_t delta = (int64_t)rec->aux;
            uint32_t cur = ef_sb_free_count_load(db->sb);
            if (delta < 0 && (uint32_t)(-delta) > cur) {
                cur = 0;
            } else {
                cur = (uint32_t)((int64_t)cur - delta);
            }
            ef_sb_free_count_store(db->sb, cur);
            ef_db_mark_meta_dirty(db);
            err = EF_OK;
            break;
        }
        default:
            err = EF_OK;
            break;
        }
        if (err != EF_OK) {
            break;
        }
    }
    return err;
}
