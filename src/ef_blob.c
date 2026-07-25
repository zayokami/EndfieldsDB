#include "endfields.h"
#include "ef_internal.h"
#include "ef_atomic_unaligned.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Maximum bytes an overflow slot can hold. */
static size_t ef_blob_overflow_chunk_cap(const struct ef_db *db)
{
    return ef_payload_capacity(db);
}

size_t ef_blob_inline_capacity(const struct ef_db *db)
{
    size_t cap = ef_payload_capacity(db);
    if (cap <= EF_BLOB_HDR_SIZE) {
        return 0;
    }
    return cap - EF_BLOB_HDR_SIZE;
}

static int ef_blob_magic_valid(const void *payload)
{
    uint32_t magic = 0;
    if (payload == NULL) {
        return 0;
    }
    memcpy(&magic, payload, sizeof(magic));
    return magic == EF_BLOB_MAGIC;
}

static uint32_t ef_blob_read_len(const struct ef_db *db, const struct ef_slot *slot)
{
    const uint8_t *payload = (const uint8_t *)ef_slot_payload_ptr(db, (struct ef_slot *)slot);
    uint32_t total = 0;

    if (payload == NULL || !ef_blob_magic_valid(payload)) {
        return 0;
    }
    memcpy(&total, payload + EF_BLOB_LEN_SIZE, sizeof(total));
    return total;
}

int ef_slot_has_overflow_chain(struct ef_db *db, const struct ef_slot *head)
{
    uint64_t offset;
    uint64_t slot_id;

    if (head == NULL || head->next_offset == 0) {
        return 0;
    }

    offset = head->next_offset;
    if (ef_offset_to_slot_id(db, offset, &slot_id) != EF_OK) {
        return 0;
    }
    if (slot_id >= db->sb->max_slots) {
        return 0;
    }
    return db->slots[slot_id].status == EF_STATUS_OVERFLOW;
}

size_t ef_blob_size(const struct ef_db *db, uint64_t slot_id)
{
    const struct ef_slot *slot;

    if (db == NULL || db->sb == NULL || slot_id >= db->sb->max_slots) {
        return 0;
    }

    slot = db->slots + slot_id;
    if (slot->status != EF_STATUS_USED) {
        return 0;
    }

    return (size_t)ef_blob_read_len(db, slot);
}

enum ef_err ef_return_slot_to_pool(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot)
{
    enum ef_err err;

    err = ef_index_remove_by_slot(db, slot_id);
    if (err != EF_OK) {
        ef_set_error(db, err);
        return err;
    }

#if EF_HAS_HW_ATOMICS
    return ef_free_list_push_atomic(db, slot_id, slot);
#else
    {
        uint64_t slot_offset = ef_slot_to_offset(db, slot_id);
        slot->next_offset = db->sb->free_list_head;
        db->sb->free_list_head = slot_offset;
        slot->status = EF_STATUS_FREE;
        slot->header_crc = 0;
        memset(ef_slot_payload_ptr(db, slot), 0, ef_payload_capacity(db));
        ef_sb_free_count_inc(db->sb);
        ef_db_mark_meta_dirty(db);
        ef_set_error(db, EF_OK);
        return EF_OK;
    }
#endif
}

static enum ef_err ef_alloc_overflow_slot(struct ef_db *db, uint64_t *slot_id_out, struct ef_slot **slot_out)
{
    enum ef_err err;

    err = ef_alloc(db, slot_id_out);
    if (err != EF_OK) {
        return err;
    }

    if (slot_out != NULL) {
        *slot_out = ef_get_slot(db, *slot_id_out);
        if (*slot_out == NULL) {
            return ef_last_error(db);
        }
        (*slot_out)->status = EF_STATUS_OVERFLOW;
        ef_slot_header_crc_store(db, *slot_id_out, *slot_out);
    }

    return EF_OK;
}

enum ef_err ef_blob_release_chain(struct ef_db *db, uint64_t head_id, struct ef_slot *head)
{
    uint64_t offset;
    enum ef_err err;

    if (db == NULL || head == NULL) {
        return EF_ERR_NULL_ARG;
    }

    offset = head->next_offset;
    head->next_offset = 0;
    ef_slot_header_crc_store(db, head_id, head);

    while (offset != 0) {
        uint64_t slot_id;
        struct ef_slot *slot;
        uint64_t next;

        err = ef_offset_to_slot_id(db, offset, &slot_id);
        if (err != EF_OK) {
            return err;
        }

        slot = db->slots + slot_id;
        if (slot->status != EF_STATUS_OVERFLOW) {
            ef_set_error(db, EF_ERR_NOT_FOUND);
            return EF_ERR_NOT_FOUND;
        }

        next = slot->next_offset;
        slot->next_offset = 0;
        err = ef_return_slot_to_pool(db, slot_id, slot);
        if (err != EF_OK) {
            return err;
        }

        offset = next;
    }

    return EF_OK;
}

enum ef_err ef_write_blob(struct ef_db *db, uint64_t slot_id, const void *data, size_t len)
{
    struct ef_slot *head;
    struct ef_slot *tail;
    enum ef_err err;
    size_t inline_cap;
    size_t chunk_cap;
    size_t remaining;
    size_t copied;
    const uint8_t *src;
    uint32_t total_len;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    if (data == NULL && len > 0) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }
    if (len > (size_t)UINT32_MAX) {
        ef_set_error(db, EF_ERR_PAYLOAD_LEN);
        return EF_ERR_PAYLOAD_LEN;
    }

    inline_cap = ef_blob_inline_capacity(db);
    chunk_cap = ef_blob_overflow_chunk_cap(db);
    if (inline_cap == 0 || chunk_cap == 0) {
        ef_set_error(db, EF_ERR_PAYLOAD_LEN);
        return EF_ERR_PAYLOAD_LEN;
    }

    head = ef_get_slot(db, slot_id);
    if (head == NULL) {
        return ef_last_error(db);
    }
    if (head->status == EF_STATUS_FREE) {
        err = ef_claim_slot(db, slot_id);
        if (err != EF_OK) {
            return err;
        }
        head = ef_get_slot(db, slot_id);
        if (head == NULL) {
            return ef_last_error(db);
        }
    }
    if (head->status != EF_STATUS_USED) {
        ef_set_error(db, EF_ERR_SLOT_BUSY);
        return EF_ERR_SLOT_BUSY;
    }

    err = ef_blob_release_chain(db, slot_id, head);
    if (err != EF_OK) {
        return err;
    }
    head = ef_get_slot(db, slot_id);
    if (head == NULL) {
        return ef_last_error(db);
    }

    total_len = (uint32_t)len;
    {
        uint8_t *payload = (uint8_t *)ef_slot_payload_ptr(db, head);
        uint32_t magic = EF_BLOB_MAGIC;
        memcpy(payload, &magic, sizeof(magic));
        memcpy(payload + EF_BLOB_LEN_SIZE, &total_len, sizeof(total_len));
    }

    src = (const uint8_t *)data;
    copied = 0;
    if (len > 0) {
        size_t inline_bytes = len < inline_cap ? len : inline_cap;
        memcpy((uint8_t *)ef_slot_payload_ptr(db, head) + EF_BLOB_HDR_SIZE, src, inline_bytes);
        copied = inline_bytes;
    }

    remaining = len - copied;
    tail = head;
    {
        uint64_t tail_id = slot_id;

        while (remaining > 0) {
            uint64_t ov_id;
            struct ef_slot *ov;
            size_t chunk;

            err = ef_alloc_overflow_slot(db, &ov_id, &ov);
            if (err != EF_OK) {
                (void)ef_blob_release_chain(db, slot_id, head);
                total_len = 0;
                {
                    uint8_t *payload = (uint8_t *)ef_slot_payload_ptr(db, head);
                    uint32_t magic = 0;
                    memcpy(payload, &magic, sizeof(magic));
                    memcpy(payload + EF_BLOB_LEN_SIZE, &total_len, sizeof(total_len));
                }
                memset((uint8_t *)ef_slot_payload_ptr(db, head) + EF_BLOB_HDR_SIZE, 0, inline_cap);
                ef_slot_header_crc_store(db, slot_id, head);
                return err;
            }

            chunk = remaining < chunk_cap ? remaining : chunk_cap;
            memcpy(ef_slot_payload_ptr(db, ov), src + copied, chunk);
            if (chunk < chunk_cap) {
                memset((uint8_t *)ef_slot_payload_ptr(db, ov) + chunk, 0, chunk_cap - chunk);
            }

            tail->next_offset = ef_slot_to_offset(db, ov_id);
            ef_slot_header_crc_store(db, tail_id, tail);
            ef_slot_header_crc_store(db, ov_id, ov);
            tail = ov;
            tail_id = ov_id;
            copied += chunk;
            remaining -= chunk;
        }
    }

    head->status = EF_STATUS_USED;
    ef_slot_header_crc_store(db, slot_id, head);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

enum ef_err ef_read_blob(struct ef_db *db, uint64_t slot_id, void *buf, size_t buf_cap, size_t *out_len)
{
    struct ef_slot *head;
    uint32_t total_len;
    size_t inline_cap;
    size_t chunk_cap;
    size_t copied;
    size_t need;
    uint64_t offset;

    if (out_len != NULL) {
        *out_len = 0;
    }
    if (db == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }

    head = ef_get_slot(db, slot_id);
    if (head == NULL) {
        return ef_last_error(db);
    }
    if (head->status != EF_STATUS_USED) {
        ef_set_error(db, EF_ERR_SLOT_FREE);
        return EF_ERR_SLOT_FREE;
    }

    if (!ef_blob_magic_valid(ef_slot_payload_ptr(db, head))) {
        ef_set_error(db, EF_ERR_NOT_FOUND);
        return EF_ERR_NOT_FOUND;
    }

    total_len = ef_blob_read_len(db, head);
    need = (size_t)total_len;
    if (out_len != NULL) {
        *out_len = need;
    }
    if (buf == NULL || buf_cap == 0) {
        ef_set_error(db, EF_OK);
        return EF_OK;
    }

    inline_cap = ef_blob_inline_capacity(db);
    chunk_cap = ef_blob_overflow_chunk_cap(db);
    copied = 0;

    if (need > 0 && copied < need) {
        size_t n = need - copied;
        if (n > inline_cap) {
            n = inline_cap;
        }
        if (n > buf_cap) {
            n = buf_cap;
        }
        memcpy(buf, (const uint8_t *)ef_slot_payload_ptr(db, head) + EF_BLOB_HDR_SIZE, n);
        copied += n;
    }

    offset = head->next_offset;
    while (copied < need && copied < buf_cap && offset != 0) {
        uint64_t ov_id;
        struct ef_slot *ov;
        size_t n;

        if (ef_offset_to_slot_id(db, offset, &ov_id) != EF_OK) {
            ef_set_error(db, EF_ERR_OFFSET);
            return EF_ERR_OFFSET;
        }

        ov = ef_get_slot(db, ov_id);
        if (ov == NULL) {
            return ef_last_error(db);
        }
        if (ov->status != EF_STATUS_OVERFLOW) {
            ef_set_error(db, EF_ERR_NOT_FOUND);
            return EF_ERR_NOT_FOUND;
        }

        n = need - copied;
        if (n > chunk_cap) {
            n = chunk_cap;
        }
        if (n > buf_cap - copied) {
            n = buf_cap - copied;
        }
        memcpy((uint8_t *)buf + copied, ef_slot_payload_ptr(db, ov), n);
        copied += n;
        offset = ov->next_offset;
    }

    ef_set_error(db, EF_OK);
    return EF_OK;
}
