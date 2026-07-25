#include "endfields.h"
#include "ef_internal.h"
#include "ef_port.h"
#include "ef_atomic_unaligned.h"
#include "ef_sb_layout.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EF_ATOMIC_STORE_U32(p, v) ef_atomic_store_u32((volatile void *)(p), (v))
#define EF_ATOMIC_CAS_U32(p, expected, desired) \
    ef_atomic_cas_u32((volatile void *)(p), (expected), (desired))
#define EF_ATOMIC_LOAD_U64(p)  ef_atomic_load_u64((const volatile void *)(p))
#define EF_ATOMIC_STORE_U64(p, v) ef_atomic_store_u64((volatile void *)(p), (v))
#define EF_ATOMIC_CAS_U64(p, expected, desired) \
    ef_atomic_cas_u64((volatile void *)(p), (expected), (desired))
#define EF_ATOMIC_THREAD_FENCE() ef_atomic_thread_fence(__ATOMIC_SEQ_CST)

#define EF_QUEUE_SPIN_MAX 65536U

static uint64_t *ef_sb_queue_head_ptr(struct ef_superblock *sb)
{
    return (uint64_t *)&sb->reserved[EF_SB_OFF_QUEUE_HEAD];
}

static uint64_t *ef_sb_queue_tail_ptr(struct ef_superblock *sb)
{
    return (uint64_t *)&sb->reserved[EF_SB_OFF_QUEUE_TAIL];
}

static const uint64_t *ef_sb_queue_head_ptr_ro(const struct ef_superblock *sb)
{
    return (const uint64_t *)&sb->reserved[EF_SB_OFF_QUEUE_HEAD];
}

static enum ef_err ef_queue_dummy_offset(struct ef_db *db, uint64_t *dummy_offset_out)
{
    volatile uint64_t *head_ptr = (volatile uint64_t *)ef_sb_queue_head_ptr(db->sb);
    volatile uint64_t *tail_ptr = (volatile uint64_t *)ef_sb_queue_tail_ptr(db->sb);
    struct ef_slot *dummy;
    uint64_t head;
    uint64_t dummy_id;
    uint64_t dummy_offset;
    uint64_t exp;
    enum ef_err err;

    head = EF_ATOMIC_LOAD_U64(head_ptr);
    if (head != 0) {
        dummy = ef_slot_at_offset(db, head, NULL);
        if (dummy != NULL && dummy->status == EF_STATUS_QUEUE_DUMMY) {
            *dummy_offset_out = head;
            return EF_OK;
        }
    }

    err = ef_alloc_slot(db, &dummy_id);
    if (err != EF_OK) {
        return err;
    }

    dummy = ef_peek_slot(db, dummy_id);
    if (dummy == NULL) {
        return ef_last_error(db);
    }

    dummy_offset = ef_slot_to_offset(db, dummy_id);
    dummy->status = EF_STATUS_QUEUE_DUMMY;
    ef_slot_next_offset_store(dummy, 0);
    memset(ef_slot_payload_ptr(db, dummy), 0, ef_payload_capacity(db));
    ef_slot_header_crc_store(db, dummy_id, dummy);
    EF_ATOMIC_THREAD_FENCE();

    exp = 0;
    if (EF_ATOMIC_CAS_U64(head_ptr, &exp, dummy_offset)) {
        EF_ATOMIC_STORE_U64(tail_ptr, dummy_offset);
        *dummy_offset_out = dummy_offset;
        ef_db_mark_meta_dirty(db);
        return EF_OK;
    }

    dummy->status = EF_STATUS_USED;
    (void)ef_return_slot_to_pool(db, dummy_id, dummy);
    *dummy_offset_out = EF_ATOMIC_LOAD_U64(head_ptr);
    return EF_OK;
}

static enum ef_err ef_queue_lock_acquire(struct ef_db *db)
{
    enum ef_err err;

    err = ef_sb_queue_lock_acquire(db->sb);
    if (err != EF_OK) {
        ef_set_error(db, err);
    }
    return err;
}

static void ef_queue_lock_release(struct ef_db *db)
{
    ef_sb_queue_lock_release(db->sb);
}

static enum ef_err ef_queue_enqueue_mpmc(struct ef_db *db, uint64_t slot_offset, uint64_t slot_id)
{
    struct ef_slot *node;
    struct ef_slot *tail_slot;
    volatile uint64_t *tail_ptr;
    uint64_t dummy_offset = 0;
    uint64_t tail_off;
    uint64_t tail_id;
    enum ef_err err;

    err = ef_queue_dummy_offset(db, &dummy_offset);
    if (err != EF_OK) {
        return err;
    }

    tail_ptr = (volatile uint64_t *)ef_sb_queue_tail_ptr(db->sb);

    node = ef_slot_at_offset(db, slot_offset, NULL);
    if (node == NULL) {
        ef_set_error(db, EF_ERR_OFFSET);
        return EF_ERR_OFFSET;
    }

    ef_slot_next_offset_store(node, 0);
    node->status = EF_STATUS_QUEUED;
    ef_slot_header_crc_store(db, slot_id, node);
    EF_ATOMIC_THREAD_FENCE();

    err = ef_queue_lock_acquire(db);
    if (err != EF_OK) {
        return err;
    }

    tail_off = EF_ATOMIC_LOAD_U64(tail_ptr);
    tail_slot = ef_slot_at_offset(db, tail_off, &tail_id);
    if (tail_slot == NULL) {
        ef_queue_lock_release(db);
        ef_set_error(db, EF_ERR_OFFSET);
        return EF_ERR_OFFSET;
    }

    ef_slot_next_offset_store(tail_slot, slot_offset);
    ef_slot_header_crc_store(db, tail_id, tail_slot);
    EF_ATOMIC_STORE_U64(tail_ptr, slot_offset);
    ef_queue_lock_release(db);
    ef_db_mark_meta_dirty(db);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

static enum ef_err ef_queue_dequeue_mpmc(struct ef_db *db, void *buf, size_t buf_cap, size_t *out_len)
{
    volatile uint64_t *head_ptr;
    volatile uint64_t *tail_ptr;
    struct ef_slot *dummy;
    struct ef_slot *first;
    uint64_t dummy_offset;
    uint64_t dummy_id;
    uint64_t tail_off;
    uint64_t first_off;
    uint64_t first_id;
    uint64_t first_next;
    uint8_t stored_len;
    const uint8_t *payload;
    enum ef_err err;

    head_ptr = (volatile uint64_t *)ef_sb_queue_head_ptr(db->sb);
    tail_ptr = (volatile uint64_t *)ef_sb_queue_tail_ptr(db->sb);

    dummy_offset = EF_ATOMIC_LOAD_U64(head_ptr);
    if (dummy_offset == 0) {
        ef_set_error(db, EF_ERR_QUEUE_EMPTY);
        return EF_ERR_QUEUE_EMPTY;
    }

    err = ef_queue_lock_acquire(db);
    if (err != EF_OK) {
        return err;
    }

    dummy = ef_slot_at_offset(db, dummy_offset, &dummy_id);
    if (dummy == NULL || dummy->status != EF_STATUS_QUEUE_DUMMY) {
        ef_queue_lock_release(db);
        ef_set_error(db, EF_ERR_NOT_FOUND);
        return EF_ERR_NOT_FOUND;
    }

    first_off = ef_slot_next_offset_load(dummy);
    if (first_off == 0) {
        ef_queue_lock_release(db);
        ef_set_error(db, EF_ERR_QUEUE_EMPTY);
        return EF_ERR_QUEUE_EMPTY;
    }

    first = ef_slot_at_offset(db, first_off, &first_id);
    if (first == NULL || first->status != EF_STATUS_QUEUED) {
        ef_queue_lock_release(db);
        ef_set_error(db, EF_ERR_NOT_FOUND);
        return EF_ERR_NOT_FOUND;
    }

    tail_off = EF_ATOMIC_LOAD_U64(tail_ptr);
    first_next = ef_slot_next_offset_load(first);
    payload = (const uint8_t *)ef_slot_payload_ptr(db, first);
    stored_len = payload[0];
    if ((size_t)stored_len + 1 > buf_cap) {
        ef_queue_lock_release(db);
        ef_set_error(db, EF_ERR_PAYLOAD_LEN);
        return EF_ERR_PAYLOAD_LEN;
    }
    if (stored_len > 0) {
        memcpy(buf, payload + 1, stored_len);
    }
    *out_len = stored_len;

    ef_slot_next_offset_store(dummy, first_next);
    if (tail_off == first_off) {
        EF_ATOMIC_STORE_U64(tail_ptr, first_next != 0 ? first_next : dummy_offset);
    }
    ef_slot_header_crc_store(db, dummy_id, dummy);
    err = ef_return_slot_to_pool(db, first_id, first);
    ef_queue_lock_release(db);
    ef_db_mark_meta_dirty(db);

    ef_set_error(db, err);
    return err;
}

enum ef_err ef_queue_push(struct ef_db *db, const void *data, uint8_t len)
{
    uint64_t slot_id;
    enum ef_err err;
    void *payload;
    struct ef_slot *slot;
    size_t cap;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }
    if ((data == NULL && len != 0) || db == NULL || db->sb == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }
    cap = ef_payload_capacity(db);

    if ((size_t)len + 1 > cap) {
        ef_set_error(db, EF_ERR_PAYLOAD_LEN);
        return EF_ERR_PAYLOAD_LEN;
    }

    err = ef_alloc_ex(db, &slot_id, 0U);
    if (err != EF_OK) {
        return err;
    }

    slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        ef_set_error(db, EF_ERR_SLOT_ID);
        return EF_ERR_SLOT_ID;
    }

    payload = ef_slot_payload_ptr(db, slot);
    ((uint8_t *)payload)[0] = len;
    if (len > 0) {
        memcpy((uint8_t *)payload + 1, data, len);
    }

    err = ef_queue_enqueue_mpmc(db, ef_slot_to_offset(db, slot_id), slot_id);
    if (err != EF_OK) {
        (void)ef_return_slot_to_pool(db, slot_id, slot);
    }
    return err;
}

enum ef_err ef_queue_pop(struct ef_db *db, void *buf, size_t buf_cap, size_t *out_len)
{
    enum ef_err err;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }
    if (buf == NULL || out_len == NULL || db == NULL || db->sb == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }

    return ef_queue_dequeue_mpmc(db, buf, buf_cap, out_len);
}

int ef_queue_empty(const struct ef_db *db)
{
    const struct ef_superblock *sb;
    uint64_t dummy_offset;
    const struct ef_slot *dummy;

    if (db == NULL || db->sb == NULL) {
        return 1;
    }

    sb = db->sb;
    dummy_offset = EF_ATOMIC_LOAD_U64((volatile uint64_t *)ef_sb_queue_head_ptr_ro(sb));
    if (dummy_offset == 0) {
        return 1;
    }

    dummy = (const struct ef_slot *)ef_offset_to_ptr((struct ef_db *)db, dummy_offset);
    if (dummy == NULL || dummy->status != EF_STATUS_QUEUE_DUMMY) {
        return EF_ATOMIC_LOAD_U64((volatile uint64_t *)ef_sb_queue_head_ptr_ro(sb)) == 0;
    }

    return ef_slot_next_offset_load(dummy) == 0;
}

int ef_queue_drained(struct ef_db *db)
{
    volatile uint64_t *head_ptr;
    uint64_t dummy_offset;
    struct ef_slot *dummy;
    uint64_t dummy_id;
    enum ef_err err;
    int drained;

    if (db == NULL || db->sb == NULL) {
        return 1;
    }

    head_ptr = (volatile uint64_t *)ef_sb_queue_head_ptr(db->sb);
    dummy_offset = EF_ATOMIC_LOAD_U64(head_ptr);
    if (dummy_offset == 0) {
        return 1;
    }

    err = ef_queue_lock_acquire(db);
    if (err != EF_OK) {
        return 0;
    }

    dummy = ef_slot_at_offset(db, dummy_offset, &dummy_id);
    if (dummy == NULL || dummy->status != EF_STATUS_QUEUE_DUMMY) {
        ef_queue_lock_release(db);
        return 0;
    }

    drained = (ef_slot_next_offset_load(dummy) == 0);
    ef_queue_lock_release(db);
    return drained;
}
