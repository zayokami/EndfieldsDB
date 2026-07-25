#include "endfields.h"
#include "ef_index.h"
#include "ef_port.h"
#include "ef_crc.h"
#include "ef_atomic_unaligned.h"
#include "ef_sb_layout.h"
#include "ef_blob.h"
#include "ef_internal.h"
#include "ef_undo.h"
#include "ef_txn.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#define EF_CHASE_VISIT_CACHE 64U
#define EF_SCHEMA_LEGACY 1U

#define EF_ATOMIC_STORE_U32(p, v) ef_atomic_store_u32((volatile void *)(p), (v))
#define EF_ATOMIC_CAS_U32(p, expected, desired) \
    ef_atomic_cas_u32((volatile void *)(p), (expected), (desired))
#define EF_ATOMIC_LOAD_U64(p)  ef_atomic_load_u64((const volatile void *)(p))
#define EF_ATOMIC_STORE_U64(p, v) ef_atomic_store_u64((volatile void *)(p), (v))
#define EF_ATOMIC_CAS_U64(p, expected, desired) \
    ef_atomic_cas_u64((volatile void *)(p), (expected), (desired))

uint64_t ef_slot_next_offset_load(const struct ef_slot *slot)
{
    return ef_atomic_load_u64((const unsigned char *)slot + offsetof(struct ef_slot, next_offset));
}

void ef_slot_next_offset_store(struct ef_slot *slot, uint64_t value)
{
    ef_atomic_store_u64((unsigned char *)slot + offsetof(struct ef_slot, next_offset), value);
}

#define EF_QUEUE_SPIN_MAX 65536U

#if EF_HAS_HW_ATOMICS
void ef_queue_yield(uint32_t spins)
{
    if (spins < 64U) {
        return;
    }
#ifdef _WIN32
    if (spins < 4096U) {
        YieldProcessor();
    } else {
        SwitchToThread();
    }
#else
    if (spins < 4096U) {
        (void)0;
    } else {
        sched_yield();
    }
#endif
}

#else
void ef_queue_yield(uint32_t spins)
{
    (void)spins;
}
#endif

void ef_set_error(struct ef_db *db, enum ef_err err)
{
    if (db != NULL) {
        ef_atomic_store_u32(&db->last_err, (uint32_t)err);
    }
}

void ef_slot_status_store(struct ef_slot *slot, uint32_t status)
{
    ef_atomic_store_u32(&slot->status, status);
}

uint32_t ef_sb_free_count_load(const struct ef_superblock *sb)
{
    if (sb == NULL) {
        return 0;
    }
    return ef_atomic_load_u32((const void *)&sb->free_count);
}

void ef_sb_free_count_store(struct ef_superblock *sb, uint32_t value)
{
    if (sb == NULL) {
        return;
    }
    ef_atomic_store_u32((void *)&sb->free_count, value);
}

void ef_sb_free_count_inc(struct ef_superblock *sb)
{
    if (sb == NULL) {
        return;
    }
    (void)ef_atomic_fetch_add_u32((void *)&sb->free_count, 1U);
}

void ef_sb_free_count_dec(struct ef_superblock *sb)
{
    if (sb == NULL) {
        return;
    }
    (void)ef_atomic_fetch_sub_u32((void *)&sb->free_count, 1U);
}

static uint32_t *ef_sb_checksum_ptr(struct ef_superblock *sb)
{
    return (uint32_t *)&sb->reserved[0];
}

static const uint32_t *ef_sb_checksum_ptr_ro(const struct ef_superblock *sb)
{
    return (const uint32_t *)&sb->reserved[0];
}

uint32_t ef_sb_checksum_compute(const struct ef_superblock *sb)
{
    uint32_t crc;
    uint32_t zero_crc = 0;

    crc = ef_crc32_update(0xFFFFFFFFU, sb, offsetof(struct ef_superblock, reserved));
    crc = ef_crc32_update(crc, &zero_crc, sizeof(zero_crc));
    crc = ef_crc32_update(crc, sb->reserved + sizeof(uint32_t),
                          sizeof(sb->reserved) - sizeof(uint32_t));
    return crc ^ 0xFFFFFFFFU;
}

void ef_sb_checksum_store(struct ef_superblock *sb)
{
    if (sb->flags & EF_FLAG_SB_CRC) {
        *ef_sb_checksum_ptr(sb) = ef_sb_checksum_compute(sb);
    } else {
        *ef_sb_checksum_ptr(sb) = 0;
    }
}

void ef_db_mark_meta_dirty(struct ef_db *db)
{
    if (db == NULL || db->sb == NULL || db->readonly) {
        return;
    }
    if (db->sb->flags & EF_FLAG_SB_CRC) {
        if (ef_atomic_load_u8(&db->sb_meta_dirty) == 0U) {
            ef_atomic_store_u8(&db->sb_meta_dirty, 1U);
        }
    }
}

enum ef_err ef_db_commit_meta(struct ef_db *db)
{
    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (db->sb == NULL || db->readonly) {
        ef_set_error(db, EF_OK);
        return EF_OK;
    }
    if (ef_atomic_load_u8(&db->sb_meta_dirty) != 0U) {
        ef_sb_checksum_store(db->sb);
        ef_atomic_store_u8(&db->sb_meta_dirty, 0U);
    }
    ef_set_error(db, EF_OK);
    return EF_OK;
}

static int ef_sb_checksum_valid(const struct ef_superblock *sb)
{
    uint32_t stored;

    if (!(sb->flags & EF_FLAG_SB_CRC)) {
        return 1;
    }

    stored = *ef_sb_checksum_ptr_ro(sb);
    if (stored == 0) {
        return 0;
    }

    return stored == ef_sb_checksum_compute(sb);
}

int ef_slot_status_uses_link_crc(uint32_t status)
{
    return status == EF_STATUS_QUEUED || status == EF_STATUS_QUEUE_DUMMY ||
           status == EF_STATUS_QUEUE_LINK || status == EF_STATUS_QUEUE_DEQ;
}

uint32_t ef_slot_header_crc_compute_full(uint64_t slot_id, const struct ef_slot *slot)
{
    uint32_t crc;

    crc = ef_crc32_update(0xFFFFFFFFU, &slot_id, sizeof(slot_id));
    crc = ef_crc32_update(crc, &slot->status, sizeof(slot->status));
    crc = ef_crc32_update(crc, slot->payload, sizeof(slot->payload));
    {
        uint64_t next_off = ef_slot_next_offset_load(slot);

        crc = ef_crc32_update(crc, &next_off, sizeof(next_off));
    }
    return crc ^ 0xFFFFFFFFU;
}

uint32_t ef_slot_header_crc_compute_link(uint64_t slot_id, const struct ef_slot *slot)
{
    uint32_t crc;
    uint64_t next_off = ef_slot_next_offset_load(slot);

    crc = ef_crc32_update(0xFFFFFFFFU, &slot_id, sizeof(slot_id));
    crc = ef_crc32_update(crc, &slot->status, sizeof(slot->status));
    crc = ef_crc32_update(crc, &next_off, sizeof(next_off));
    return crc ^ 0xFFFFFFFFU;
}

uint32_t ef_slot_header_crc_compute(uint64_t slot_id, const struct ef_slot *slot)
{
    if (ef_slot_status_uses_link_crc(slot->status)) {
        return ef_slot_header_crc_compute_link(slot_id, slot);
    }
    return ef_slot_header_crc_compute_full(slot_id, slot);
}

void ef_slot_header_crc_store(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot)
{
    if (!(db->sb->flags & EF_FLAG_SLOT_CRC)) {
        return;
    }
    slot->header_crc = ef_slot_header_crc_compute(slot_id, slot);
}

#if EF_HAS_HW_ATOMICS
enum ef_err ef_free_list_pop_atomic(struct ef_db *db, uint64_t *slot_id_out, int clear_payload)
{
    volatile uint64_t *head_ptr = (volatile uint64_t *)&db->sb->free_list_head;
    struct ef_slot *slot;
    uint64_t slot_id;
    uint64_t head;
    uint64_t next;
    uint64_t exp;
    uint32_t spins = 0;
    enum ef_err err;

    for (;;) {
        if (++spins > EF_QUEUE_SPIN_MAX) {
            ef_set_error(db, EF_ERR_QUEUE_BUSY);
            return EF_ERR_QUEUE_BUSY;
        }
        ef_queue_yield(spins);

        head = EF_ATOMIC_LOAD_U64(head_ptr);
        if (head == 0 || ef_sb_free_count_load(db->sb) == 0) {
            ef_set_error(db, EF_ERR_SLOT_FULL);
            return EF_ERR_SLOT_FULL;
        }

        slot = (struct ef_slot *)ef_offset_to_ptr(db, head);
        if (slot == NULL) {
            return ef_last_error(db);
        }

        err = ef_offset_to_slot_id(db, head, &slot_id);
        if (err != EF_OK) {
            ef_set_error(db, err);
            return err;
        }

        next = ef_slot_next_offset_load(slot);
        exp = head;
        if (EF_ATOMIC_CAS_U64(head_ptr, &exp, next)) {
            ef_slot_next_offset_store(slot, 0);
            ef_slot_status_store(slot, EF_STATUS_USED);
            if (clear_payload) {
                memset(ef_slot_payload_ptr(db, slot), 0, ef_payload_capacity(db));
            }
            ef_sb_free_count_dec(db->sb);
            ef_slot_header_crc_store(db, slot_id, slot);
            ef_db_mark_meta_dirty(db);
            *slot_id_out = slot_id;
            ef_set_error(db, EF_OK);
            return EF_OK;
        }
    }
}

enum ef_err ef_free_list_push_atomic(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot)
{
    volatile uint64_t *head_ptr = (volatile uint64_t *)&db->sb->free_list_head;
    uint64_t slot_offset;
    uint64_t head;
    uint64_t exp;
    uint32_t spins = 0;

    slot_offset = ef_slot_to_offset(db, slot_id);
    slot->header_crc = 0;
    memset(ef_slot_payload_ptr(db, slot), 0, ef_payload_capacity(db));

    for (;;) {
        if (++spins > EF_QUEUE_SPIN_MAX) {
            ef_set_error(db, EF_ERR_QUEUE_BUSY);
            return EF_ERR_QUEUE_BUSY;
        }
        ef_queue_yield(spins);

        head = EF_ATOMIC_LOAD_U64(head_ptr);
        ef_slot_next_offset_store(slot, head);
        ef_slot_status_store(slot, EF_STATUS_FREE);
        EF_ATOMIC_THREAD_FENCE();

        exp = head;
        if (EF_ATOMIC_CAS_U64(head_ptr, &exp, slot_offset)) {
            ef_sb_free_count_inc(db->sb);
            ef_db_mark_meta_dirty(db);
            ef_set_error(db, EF_OK);
            return EF_OK;
        }
    }
}
#endif

int ef_slot_status_has_crc(uint32_t status)
{
    return status == EF_STATUS_USED || status == EF_STATUS_OVERFLOW ||
           status == EF_STATUS_QUEUED || status == EF_STATUS_QUEUE_DUMMY ||
           status == EF_STATUS_QUEUE_LINK || status == EF_STATUS_QUEUE_DEQ;
}

void ef_db_refresh_slot_crcs(struct ef_db *db)
{
    uint64_t i;

    if (db == NULL || db->sb == NULL || db->slots == NULL) {
        return;
    }

    if (!(db->sb->flags & EF_FLAG_SLOT_CRC)) {
        return;
    }

    for (i = 0; i < db->sb->max_slots; ++i) {
        if (ef_slot_status_has_crc(db->slots[i].status)) {
            ef_slot_header_crc_store(db, i, db->slots + i);
        }
    }
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
static const uint64_t *ef_sb_queue_tail_ptr_ro(const struct ef_superblock *sb)
{
    return (const uint64_t *)&sb->reserved[EF_SB_OFF_QUEUE_TAIL];
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static uint32_t ef_hash_capacity_from_sb(const struct ef_superblock *sb)
{
    if (sb == NULL || sb->schema_version < EF_SCHEMA_VERSION_V3) {
        return 0;
    }
    return ef_sb_hash_capacity_load(sb);
}

int ef_hash_capacity_valid(uint32_t hash_capacity)
{
    if (hash_capacity == 0) {
        return 1;
    }
    if (hash_capacity > 0xFFFFU) {
        return 0;
    }
    return (hash_capacity & (hash_capacity - 1U)) == 0;
}

int ef_slot_header_crc_valid(struct ef_db *db, uint64_t slot_id, const struct ef_slot *slot)
{
    if (!(db->sb->flags & EF_FLAG_SLOT_CRC)) {
        return 1;
    }
    if (!ef_slot_status_has_crc(slot->status)) {
        return 1;
    }
    if (slot->header_crc == 0) {
        return 0;
    }
    if (slot->header_crc == ef_slot_header_crc_compute(slot_id, slot)) {
        return 1;
    }
    if (ef_slot_status_uses_link_crc(slot->status) &&
        slot->header_crc == ef_slot_header_crc_compute_full(slot_id, slot)) {
        return 1;
    }
    return 0;
}

enum ef_err ef_db_require_write(struct ef_db *db)
{
    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (db->readonly) {
        ef_set_error(db, EF_ERR_READONLY);
        return EF_ERR_READONLY;
    }
    return EF_OK;
}

void ef_db_refresh_checksums(struct ef_db *db)
{
    uint64_t i;

    if (db == NULL || db->sb == NULL || db->slots == NULL) {
        return;
    }

    if (db->sb->flags & EF_FLAG_SLOT_CRC) {
        for (i = 0; i < db->sb->max_slots; ++i) {
            if (ef_slot_status_has_crc(db->slots[i].status)) {
                ef_slot_header_crc_store(db, i, db->slots + i);
            }
        }
    }

    ef_sb_checksum_store(db->sb);
}

static int ef_magic_valid(const struct ef_superblock *sb)
{
    return sb->magic[0] == EF_MAGIC_0 &&
           sb->magic[1] == EF_MAGIC_1 &&
           sb->magic[2] == EF_MAGIC_2 &&
           sb->magic[3] == EF_MAGIC_3;
}

size_t ef_expected_file_size(uint64_t max_slots, uint32_t hash_capacity,
                             uint32_t undo_log_slots)
{
    return (size_t)(sizeof(struct ef_superblock) +
                    (uint64_t)hash_capacity * sizeof(struct ef_hash_entry) +
                    max_slots * sizeof(struct ef_slot) +
                    sizeof(struct ef_undo_header) +
                    (uint64_t)undo_log_slots * 32U);
}

static void ef_db_bind_slots_layout(struct ef_db *db)
{
    uint32_t hash_capacity;
    uint64_t slots_end;

    if (db == NULL || db->sb == NULL || db->mmap_addr == NULL) {
        return;
    }

    hash_capacity = ef_hash_capacity_from_sb(db->sb);
    db->hash_capacity = hash_capacity;
    if (hash_capacity > 0) {
        db->hash_index = (struct ef_hash_entry *)((uint8_t *)db->mmap_addr +
                                                  sizeof(struct ef_superblock));
        db->slots_base = (uint64_t)sizeof(struct ef_superblock) +
                         (uint64_t)hash_capacity * sizeof(struct ef_hash_entry);
    } else {
        db->hash_index = NULL;
        db->slots_base = (uint64_t)sizeof(struct ef_superblock);
    }
    db->slots = (struct ef_slot *)((uint8_t *)db->mmap_addr + db->slots_base);

    /* v5: undo log segment sits after the slot area. */
    slots_end = db->slots_base + (uint64_t)db->sb->max_slots * sizeof(struct ef_slot);
    db->undo_log_base = slots_end;
    db->undo_log_slots = EF_UNDO_LOG_DEFAULT_SLOTS;
    db->undo_log_mmap = (uint8_t *)db->mmap_addr + slots_end;
}

void ef_db_bind_io(struct ef_db *db, const struct ef_io *io)
{
    db->fd = io->fd;
    db->mmap_addr = io->map_addr;
    db->file_size = io->map_size;
    db->backend = io->backend;
#ifdef _WIN32
    db->map_handle = io->map_handle;
#endif
    db->sb = (struct ef_superblock *)db->mmap_addr;
    db->map_capacity = io->map_capacity;
    db->readonly = io->readonly;
    ef_db_bind_slots_layout(db);
}

void ef_db_to_io(const struct ef_db *db, struct ef_io *io)
{
    io->fd = db->fd;
    io->map_addr = db->mmap_addr;
    io->map_size = db->file_size;
    io->backend = db->backend;
    io->map_capacity = db->map_capacity;
    io->readonly = db->readonly;
#ifdef _WIN32
    io->map_handle = db->map_handle;
#endif
}

static enum ef_err ef_validate_superblock(const struct ef_superblock *sb, size_t file_size)
{
    size_t expected;
    uint32_t undo_log_slots;

    if (sb == NULL) {
        return EF_ERR_NULL_ARG;
    }

    if (!ef_magic_valid(sb)) {
        return EF_ERR_BAD_MAGIC;
    }

    if (sb->slot_size != EF_SLOT_SIZE) {
        return EF_ERR_BAD_SLOT_SIZE;
    }

    if (sb->schema_version != 0 &&
        sb->schema_version != EF_SCHEMA_LEGACY &&
        sb->schema_version != EF_SCHEMA_VERSION_V2 &&
        sb->schema_version != EF_SCHEMA_VERSION_V3 &&
        sb->schema_version != EF_SCHEMA_VERSION_V4 &&
        sb->schema_version != EF_SCHEMA_VERSION) {
        return EF_ERR_BAD_VERSION;
    }

    if (!ef_hash_capacity_valid(ef_hash_capacity_from_sb(sb))) {
        return EF_ERR_BAD_VERSION;
    }

    /* v5 files always include the default undo log segment; v3/v4 files do
     * not (they will be migrated to v5 by ef_db_init_mapped after validation).
     * Older files (v1/v2/legacy) may have already been pre-grown to the v5
     * layout by ef_open_ex_hash, so accept file_size >= expected for any
     * pre-v5 schema (the migration path will use the extra region as the
     * undo log). v5 files must match exactly. */
    undo_log_slots = (sb->schema_version == EF_SCHEMA_VERSION)
                         ? EF_UNDO_LOG_DEFAULT_SLOTS
                         : 0U;
    expected = ef_expected_file_size(sb->max_slots, ef_hash_capacity_from_sb(sb), undo_log_slots);
    if (sb->schema_version == EF_SCHEMA_VERSION) {
        if (file_size != expected) {
            return EF_ERR_FILE_SIZE;
        }
    } else {
        if (file_size < expected) {
            return EF_ERR_FILE_SIZE;
        }
    }

    if (!ef_sb_checksum_valid(sb)) {
        return EF_ERR_BAD_CHECKSUM;
    }

    return EF_OK;
}

static void ef_init_superblock(struct ef_superblock *sb, uint64_t max_slots, uint32_t hash_capacity)
{
    sb->magic[0] = EF_MAGIC_0;
    sb->magic[1] = EF_MAGIC_1;
    sb->magic[2] = EF_MAGIC_2;
    sb->magic[3] = EF_MAGIC_3;
    sb->slot_size = EF_SLOT_SIZE;
    sb->max_slots = max_slots;
    sb->free_list_head = 0;
    sb->schema_version = EF_SCHEMA_VERSION;
    sb->flags = EF_FLAG_SB_CRC | EF_FLAG_SLOT_CRC;
    ef_sb_free_count_store(sb, (uint32_t)max_slots);
    memset(sb->reserved, 0, sizeof(sb->reserved));
    ef_sb_hash_capacity_store(sb, hash_capacity);
    ef_sb_checksum_store(sb);
}

static void ef_init_hash_region(struct ef_db *db)
{
    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return;
    }

    memset(db->hash_index, 0, (size_t)db->hash_capacity * sizeof(struct ef_hash_entry));
}

static void ef_upgrade_superblock(struct ef_superblock *sb, uint64_t free_count)
{
    if (sb->schema_version == 0) {
        sb->schema_version = EF_SCHEMA_LEGACY;
        sb->flags = EF_FLAG_NONE;
    }
    ef_sb_free_count_store(sb, (uint32_t)free_count);
    ef_sb_checksum_store(sb);
}

static void ef_init_slots(struct ef_db *db)
{
    size_t slots_bytes;

    if (db == NULL || db->slots == NULL || db->sb == NULL) {
        return;
    }

    slots_bytes = (size_t)(db->sb->max_slots * sizeof(struct ef_slot));
    memset(db->slots, 0, slots_bytes);
}

enum ef_err ef_unlink_free_slot(struct ef_db *db, uint64_t slot_id);
struct ef_slot *ef_slot_at_offset(struct ef_db *db, uint64_t offset, uint64_t *slot_id_out);

static enum ef_err ef_build_free_list(struct ef_db *db)
{
    uint64_t i;
    uint64_t max_slots;
    struct ef_slot *slot;

    if (db == NULL || db->sb == NULL || db->slots == NULL) {
        return EF_ERR_NULL_ARG;
    }

    max_slots = db->sb->max_slots;
    db->sb->free_list_head = 0;

    if (max_slots == 0) {
        ef_sb_free_count_store(db->sb, 0);
        return EF_OK;
    }

    for (i = 0; i < max_slots; ++i) {
        slot = db->slots + i;
        slot->status = EF_STATUS_FREE;
        memset(slot->payload, 0, sizeof(slot->payload));
        slot->next_offset = (i + 1 < max_slots) ? ef_slot_to_offset(db, i + 1) : 0;
    }

    db->sb->free_list_head = ef_slot_to_offset(db, 0);
    ef_sb_free_count_store(db->sb, (uint32_t)max_slots);
    ef_db_refresh_checksums(db);
    return EF_OK;
}

static enum ef_err ef_rebuild_free_list(struct ef_db *db)
{
    int64_t i;
    struct ef_slot *slot;
    uint64_t free_count = 0;

    if (db == NULL || db->sb == NULL || db->slots == NULL) {
        return EF_ERR_NULL_ARG;
    }

    db->sb->free_list_head = 0;

    for (i = (int64_t)db->sb->max_slots - 1; i >= 0; --i) {
        slot = db->slots + (uint64_t)i;
        if (slot->status == EF_STATUS_FREE) {
            slot->next_offset = db->sb->free_list_head;
            db->sb->free_list_head = ef_slot_to_offset(db, (uint64_t)i);
            ++free_count;
        }
    }

    ef_upgrade_superblock(db->sb, free_count);
    ef_db_refresh_checksums(db);
    return EF_OK;
}

static int ef_chase_offset_seen(uint64_t offset, const uint64_t *seen, uint32_t seen_count)
{
    uint32_t i;

    for (i = 0; i < seen_count; ++i) {
        if (seen[i] == offset) {
            return 1;
        }
    }
    return 0;
}

static enum ef_err ef_db_init_mapped(struct ef_db *db, int is_new_file, uint64_t initial_slots,
                                     uint32_t hash_capacity)
{
    enum ef_err err;

    if (is_new_file) {
        if (!ef_hash_capacity_valid(hash_capacity)) {
            return EF_ERR_BAD_VERSION;
        }
        ef_init_superblock(db->sb, initial_slots, hash_capacity);
        ef_db_bind_slots_layout(db);
        ef_init_hash_region(db);
        ef_init_slots(db);
        err = ef_build_free_list(db);
        if (err != EF_OK) {
            return err;
        }
    } else {
        err = ef_validate_superblock(db->sb, db->file_size);
        if (err != EF_OK) {
            return err;
        }
        if (db->readonly) {
            /* Read-only mapping: use on-disk free list as-is; never write mmap. */
            return EF_OK;
        }
        /* Bind layout before any v3/v4→v5 migration so that undo_log_slots
         * is populated and post-migration zeroing hits the right region. */
        ef_db_bind_slots_layout(db);
        if (db->sb->schema_version == EF_SCHEMA_VERSION_V3 &&
            ef_sb_hash_capacity_load(db->sb) > 0U) {
            err = ef_sb_migrate_v3_index_layout(db->sb);
            if (err != EF_OK) {
                return err;
            }
            ef_db_bind_slots_layout(db);
            ef_db_mark_meta_dirty(db);
        }
        if (db->sb->schema_version == EF_SCHEMA_VERSION_V4) {
            err = ef_sb_migrate_v4_txn_layout(db->sb);
            if (err != EF_OK) {
                return err;
            }
            ef_db_mark_meta_dirty(db);
            /* Zero the undo log segment. v4 file didn't have it; the
             * mmap'd region after the slot area may contain random bytes
             * that we must clear before the first transaction. */
            {
                uint64_t slots_end = db->slots_base +
                                     (uint64_t)db->sb->max_slots * sizeof(struct ef_slot);
                size_t undo_bytes = (size_t)EF_UNDO_HEADER_SIZE +
                                    (size_t)db->undo_log_slots * EF_UNDO_RECORD_SIZE;
                if (db->file_size >= slots_end + undo_bytes) {
                    memset((uint8_t *)db->mmap_addr + slots_end, 0, undo_bytes);
                }
            }
        }
        /* v5: if a transaction was left active (stale), replay and clear. */
        if (db->sb->schema_version >= EF_SCHEMA_VERSION) {
            err = ef_txn_recover_from_stale_lock(db);
            if (err != EF_OK) {
                return err;
            }
        }
        err = ef_rebuild_free_list(db);
        if (err != EF_OK) {
            return err;
        }
        /* After migration, the file size should match the v5 layout
         * (including the undo log segment). The caller may have provided a
         * larger buffer for the memory backend; cap db->file_size at the
         * true v5 layout to keep offset checks consistent. */
        {
            size_t v5_size = ef_expected_file_size(db->sb->max_slots,
                                                   ef_hash_capacity_from_sb(db->sb),
                                                   db->undo_log_slots);
            if (db->file_size < v5_size) {
                if (db->backend == EF_BACKEND_FILE) {
                    /* The file should have been grown before init_mapped. */
                    return EF_ERR_FILE_SIZE;
                }
                /* Memory backend: caller must have given us a buffer that
                 * already covers the v5 size. The actual mmap is sized by
                 * the port layer; bump db->file_size so offset checks pass
                 * for offsets inside the undo log segment. */
                db->file_size = v5_size;
            }
        }
    }

    return EF_OK;
}

enum ef_err ef_unlink_free_slot(struct ef_db *db, uint64_t slot_id);

enum ef_err ef_claim_slot(struct ef_db *db, uint64_t slot_id)
{
    struct ef_slot *slot;
    enum ef_err err;

    slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        ef_set_error(db, EF_ERR_SLOT_ID);
        return EF_ERR_SLOT_ID;
    }

    if (slot->status == EF_STATUS_USED) {
        ef_set_error(db, EF_OK);
        return EF_OK;
    }

    err = ef_unlink_free_slot(db, slot_id);
    if (err != EF_OK) {
        ef_set_error(db, err);
        return err;
    }

    slot->status = EF_STATUS_USED;
    if (ef_sb_free_count_load(db->sb) > 0) {
        ef_sb_free_count_dec(db->sb);
    }
    ef_slot_header_crc_store(db, slot_id, slot);
    ef_db_mark_meta_dirty(db);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

const char *ef_strerror(enum ef_err err)
{
    switch (err) {
    case EF_OK: return "ok";
    case EF_ERR_NULL_ARG: return "null argument";
    case EF_ERR_IO: return "io error";
    case EF_ERR_MMAP: return "mmap failed";
    case EF_ERR_OOM: return "out of memory";
    case EF_ERR_BAD_MAGIC: return "invalid magic";
    case EF_ERR_BAD_VERSION: return "unsupported schema version";
    case EF_ERR_BAD_CHECKSUM: return "checksum mismatch";
    case EF_ERR_BAD_SLOT_SIZE: return "invalid slot size in superblock";
    case EF_ERR_FILE_SIZE: return "file size mismatch";
    case EF_ERR_SLOT_ID: return "invalid slot id";
    case EF_ERR_OFFSET: return "invalid file offset";
    case EF_ERR_PAYLOAD_LEN: return "invalid payload length";
    case EF_ERR_OPCODE: return "invalid opcode";
    case EF_ERR_SLOT_FREE: return "slot is free";
    case EF_ERR_SLOT_BUSY: return "slot is in use";
    case EF_ERR_SLOT_FULL: return "no free slots";
    case EF_ERR_NOT_FOUND: return "not found";
    case EF_ERR_CHASE_DEPTH: return "chase depth exceeded";
    case EF_ERR_CHASE_CYCLE: return "pointer cycle detected";
    case EF_ERR_READONLY: return "database opened read-only";
    case EF_ERR_GROW: return "invalid grow request";
    case EF_ERR_QUEUE_EMPTY: return "queue is empty";
    case EF_ERR_QUEUE_BUSY: return "queue contended too long";
    case EF_ERR_INDEX_FULL: return "hash index is full";
    case EF_ERR_INDEX_BUSY: return "hash index lock busy";
    case EF_ERR_USER_ABORT: return "user callback aborted";
    default: return "unknown error";
    }
}

enum ef_err ef_last_error(const struct ef_db *db)
{
    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    return (enum ef_err)ef_atomic_load_u32(&db->last_err);
}

#if EF_HAS_FILE_IO

enum ef_err ef_open_ex_hash(const char *filepath, uint64_t initial_slots, uint32_t hash_capacity,
                            struct ef_db **db_out)
{
    struct ef_db *db;
    struct ef_io io;
    size_t file_size;
    int is_new_file = 0;
    enum ef_err err;

    if (db_out == NULL || filepath == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (!ef_hash_capacity_valid(hash_capacity)) {
        return EF_ERR_BAD_VERSION;
    }

    *db_out = NULL;
    file_size = ef_expected_file_size(initial_slots, hash_capacity, EF_UNDO_LOG_DEFAULT_SLOTS);

    db = (struct ef_db *)calloc(1, sizeof(*db));
    if (db == NULL) {
        return EF_ERR_OOM;
    }

    err = ef_port_open_file(filepath, file_size, &io, &is_new_file);
    if (err != EF_OK) {
        free(db);
        return err;
    }

    ef_db_bind_io(db, &io);
    if (!is_new_file && db->sb != NULL &&
        db->sb->schema_version < EF_SCHEMA_VERSION) {
        /* Existing v3/v4 file: auto-grow the file to v5 layout before
         * migration runs. The v3/v4 on-disk file is missing the undo log
         * segment; without this grow the migration would write past the
         * mapped size. */
        size_t v5_size = ef_expected_file_size(db->sb->max_slots,
                                               ef_hash_capacity_from_sb(db->sb),
                                               EF_UNDO_LOG_DEFAULT_SLOTS);
        if (db->file_size < v5_size) {
            enum ef_err err_grow = ef_port_grow_file(&io, v5_size);
            if (err_grow != EF_OK) {
                ef_port_close(&io);
                free(db);
                return err_grow;
            }
            ef_db_bind_io(db, &io);
        }
    }
    err = ef_db_init_mapped(db, is_new_file, initial_slots, hash_capacity);
    if (err != EF_OK) {
        ef_port_close(&io);
        free(db);
        return err;
    }

    ef_set_error(db, EF_OK);
    *db_out = db;
    return EF_OK;
}

enum ef_err ef_open_ex(const char *filepath, uint64_t initial_slots, struct ef_db **db_out)
{
    return ef_open_ex_hash(filepath, initial_slots, 0, db_out);
}

struct ef_db *ef_open(const char *filepath, uint64_t initial_slots)
{
    struct ef_db *db = NULL;
    if (ef_open_ex(filepath, initial_slots, &db) != EF_OK) {
        return NULL;
    }
    return db;
}

enum ef_err ef_open_readonly_ex(const char *filepath, struct ef_db **db_out)
{
    struct ef_db *db;
    struct ef_io io;
    enum ef_err err;

    if (db_out == NULL || filepath == NULL) {
        return EF_ERR_NULL_ARG;
    }

    *db_out = NULL;
    db = (struct ef_db *)calloc(1, sizeof(*db));
    if (db == NULL) {
        return EF_ERR_OOM;
    }

    err = ef_port_open_file_existing(filepath, &io, 1);
    if (err != EF_OK) {
        free(db);
        return err;
    }

    ef_db_bind_io(db, &io);
    err = ef_db_init_mapped(db, 0, 0, 0);
    if (err != EF_OK) {
        ef_port_close(&io);
        free(db);
        return err;
    }

    db->readonly = 1;
    ef_set_error(db, EF_OK);
    *db_out = db;
    return EF_OK;
}

struct ef_db *ef_open_readonly(const char *filepath)
{
    struct ef_db *db = NULL;
    if (ef_open_readonly_ex(filepath, &db) != EF_OK) {
        return NULL;
    }
    return db;
}

#else /* !EF_HAS_FILE_IO */

enum ef_err ef_open_ex_hash(const char *filepath, uint64_t initial_slots, uint32_t hash_capacity,
                            struct ef_db **db_out)
{
    (void)filepath;
    (void)initial_slots;
    (void)hash_capacity;
    if (db_out != NULL) {
        *db_out = NULL;
    }
    return EF_ERR_IO;
}

enum ef_err ef_open_ex(const char *filepath, uint64_t initial_slots, struct ef_db **db_out)
{
    (void)filepath;
    (void)initial_slots;
    if (db_out != NULL) {
        *db_out = NULL;
    }
    return EF_ERR_IO;
}

struct ef_db *ef_open(const char *filepath, uint64_t initial_slots)
{
    (void)filepath;
    (void)initial_slots;
    return NULL;
}

enum ef_err ef_open_readonly_ex(const char *filepath, struct ef_db **db_out)
{
    (void)filepath;
    if (db_out != NULL) {
        *db_out = NULL;
    }
    return EF_ERR_IO;
}

struct ef_db *ef_open_readonly(const char *filepath)
{
    (void)filepath;
    return NULL;
}

#endif /* EF_HAS_FILE_IO */

enum ef_err ef_open_memory_hash(void *buffer, size_t buffer_size, uint64_t max_slots,
                                uint32_t hash_capacity, int init_new, struct ef_db **db_out)
{
    struct ef_db *db;
    struct ef_io io;
    size_t need;
    enum ef_err err;

    if (db_out == NULL || buffer == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (!ef_hash_capacity_valid(hash_capacity)) {
        return EF_ERR_BAD_VERSION;
    }

    *db_out = NULL;
    need = ef_expected_file_size(max_slots, hash_capacity, EF_UNDO_LOG_DEFAULT_SLOTS);
    if (!init_new) {
        const struct ef_superblock *sb_probe = (const struct ef_superblock *)buffer;
        size_t on_disk_need;

        if (ef_magic_valid(sb_probe) && sb_probe->slot_size == EF_SLOT_SIZE) {
            /* Existing files: v5 already includes the undo log; older
             * versions (v2/v3/v4) will be migrated by ef_db_init_mapped,
             * so probe size matches the pre-migration layout. */
            uint32_t probe_undo = (sb_probe->schema_version == EF_SCHEMA_VERSION)
                                      ? EF_UNDO_LOG_DEFAULT_SLOTS
                                      : 0U;
            on_disk_need = ef_expected_file_size(sb_probe->max_slots,
                                                 ef_hash_capacity_from_sb(sb_probe), probe_undo);
            if (on_disk_need > buffer_size) {
                return EF_ERR_FILE_SIZE;
            }
            need = on_disk_need;
            max_slots = sb_probe->max_slots;
        }
    }
    if (buffer_size < need) {
        return EF_ERR_FILE_SIZE;
    }

    db = (struct ef_db *)calloc(1, sizeof(*db));
    if (db == NULL) {
        return EF_ERR_OOM;
    }

    err = ef_port_open_memory(buffer, need, &io);
    if (err != EF_OK) {
        free(db);
        return err;
    }

    if (init_new) {
        memset(buffer, 0, need);
    }

    ef_db_bind_io(db, &io);
    db->file_size = need;
    db->map_capacity = buffer_size;
    err = ef_db_init_mapped(db, init_new, max_slots, hash_capacity);
    if (err != EF_OK) {
        ef_port_close(&io);
        free(db);
        return err;
    }

    ef_set_error(db, EF_OK);
    *db_out = db;
    return EF_OK;
}

enum ef_err ef_open_memory(void *buffer, size_t buffer_size, uint64_t max_slots, int init_new,
                           struct ef_db **db_out)
{
    return ef_open_memory_hash(buffer, buffer_size, max_slots, 0, init_new, db_out);
}

void ef_close(struct ef_db *db)
{
    struct ef_io io;

    if (db == NULL) {
        return;
    }

    if (!db->readonly) {
        (void)ef_db_commit_meta(db);
    }

    ef_db_to_io(db, &io);
    ef_port_close(&io);
    free(db);
}

enum ef_err ef_sync_ex(struct ef_db *db, enum ef_sync_mode mode)
{
    struct ef_io io;
    enum ef_err err;

    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }

    err = ef_db_commit_meta(db);
    if (err != EF_OK) {
        return err;
    }

    ef_db_to_io(db, &io);
    err = ef_port_sync(&io, mode);
    ef_set_error(db, err);
    return err;
}

enum ef_err ef_sync(struct ef_db *db)
{
    return ef_sync_ex(db, EF_SYNC_FULL);
}

int ef_is_readonly(const struct ef_db *db)
{
    if (db == NULL) {
        return 0;
    }
    return db->readonly != 0;
}

size_t ef_payload_capacity(const struct ef_db *db)
{
    if (db == NULL || db->sb == NULL) {
        return EF_PAYLOAD_SIZE;
    }
    if (db->sb->schema_version == 0 || db->sb->schema_version == EF_SCHEMA_LEGACY) {
        return EF_PAYLOAD_SIZE_LEGACY;
    }
    return EF_PAYLOAD_SIZE;
}

void *ef_slot_payload_ptr(const struct ef_db *db, struct ef_slot *slot)
{
    if (db == NULL || slot == NULL) {
        return NULL;
    }
    if (db->sb->schema_version == 0 || db->sb->schema_version == EF_SCHEMA_LEGACY) {
        return (uint8_t *)slot + offsetof(struct ef_slot, status) + sizeof(uint32_t);
    }
    return slot->payload;
}

struct ef_slot *ef_slot_at_offset(struct ef_db *db, uint64_t offset, uint64_t *slot_id_out)
{
    uint64_t rel;
    uint64_t slot_id;

    if (db == NULL || db->sb == NULL || db->slots == NULL) {
        return NULL;
    }
    if (EF_UNLIKELY(offset < db->slots_base || offset >= db->file_size)) {
        return NULL;
    }

    rel = offset - db->slots_base;
    if (EF_UNLIKELY((rel & EF_SLOT_MASK) != 0)) {
        return NULL;
    }

    slot_id = rel >> EF_SLOT_SHIFT;
    if (EF_UNLIKELY(slot_id >= db->sb->max_slots)) {
        return NULL;
    }

    if (slot_id_out != NULL) {
        *slot_id_out = slot_id;
    }
    return db->slots + slot_id;
}

uint64_t ef_slot_to_offset(const struct ef_db *db, uint64_t slot_id)
{
    if (db == NULL || db->sb == NULL) {
        return 0;
    }
    if (EF_UNLIKELY(slot_id >= db->sb->max_slots)) {
        return 0;
    }
    return db->slots_base + (slot_id << EF_SLOT_SHIFT);
}

enum ef_err ef_offset_to_slot_id(const struct ef_db *db, uint64_t offset, uint64_t *slot_id_out)
{
    uint64_t rel;
    uint64_t slot_id;

    if (db == NULL || db->sb == NULL || slot_id_out == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (EF_UNLIKELY(offset < db->slots_base)) {
        return EF_ERR_OFFSET;
    }

    rel = offset - db->slots_base;
    if (EF_UNLIKELY((rel & EF_SLOT_MASK) != 0)) {
        return EF_ERR_OFFSET;
    }

    slot_id = rel >> EF_SLOT_SHIFT;
    if (EF_UNLIKELY(slot_id >= db->sb->max_slots)) {
        return EF_ERR_SLOT_ID;
    }

    *slot_id_out = slot_id;
    return EF_OK;
}

static enum ef_err ef_slot_verify_used(struct ef_db *db, uint64_t slot_id, const struct ef_slot *slot)
{
    if (!ef_slot_header_crc_valid(db, slot_id, slot)) {
        ef_set_error(db, EF_ERR_BAD_CHECKSUM);
        return EF_ERR_BAD_CHECKSUM;
    }
    return EF_OK;
}

struct ef_slot *ef_peek_slot(struct ef_db *db, uint64_t slot_id)
{
    if (EF_UNLIKELY(db == NULL || db->sb == NULL || db->slots == NULL)) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return NULL;
    }
    if (EF_UNLIKELY(slot_id >= db->sb->max_slots)) {
        ef_set_error(db, EF_ERR_SLOT_ID);
        return NULL;
    }

    ef_set_error(db, EF_OK);
    return db->slots + slot_id;
}

struct ef_slot *ef_get_slot(struct ef_db *db, uint64_t slot_id)
{
    struct ef_slot *slot;

    slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        return NULL;
    }

    if (ef_slot_status_has_crc(slot->status)) {
        if (ef_slot_verify_used(db, slot_id, slot) != EF_OK) {
            return NULL;
        }
    }

    return slot;
}

void *ef_offset_to_ptr(struct ef_db *db, uint64_t offset)
{
    if (EF_UNLIKELY(db == NULL || db->mmap_addr == NULL)) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return NULL;
    }
    if (EF_UNLIKELY(offset == 0 || offset >= db->file_size)) {
        ef_set_error(db, EF_ERR_OFFSET);
        return NULL;
    }

    ef_set_error(db, EF_OK);
    return (uint8_t *)db->mmap_addr + offset;
}

struct ef_slot *ef_chase(struct ef_db *db, struct ef_slot *current_slot)
{
    uint64_t next_offset;
    struct ef_slot *next_slot;

    if (EF_UNLIKELY(db == NULL || current_slot == NULL)) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return NULL;
    }

    next_offset = ef_slot_next_offset_load(current_slot);
    if (EF_UNLIKELY(next_offset == 0)) {
        ef_set_error(db, EF_ERR_OFFSET);
        return NULL;
    }
    if (EF_UNLIKELY(next_offset < db->slots_base || next_offset >= db->file_size)) {
        ef_set_error(db, EF_ERR_OFFSET);
        return NULL;
    }
    if (EF_UNLIKELY((next_offset - db->slots_base) & EF_SLOT_MASK)) {
        ef_set_error(db, EF_ERR_OFFSET);
        return NULL;
    }

    next_slot = (struct ef_slot *)((uint8_t *)db->mmap_addr + next_offset);
    if (EF_UNLIKELY(next_slot->status != EF_STATUS_USED)) {
        ef_set_error(db, EF_ERR_SLOT_FREE);
        return NULL;
    }

    EF_PREFETCH_R(next_slot);
    ef_set_error(db, EF_OK);
    return next_slot;
}

struct ef_slot *ef_chase_n(struct ef_db *db, uint64_t start_offset, uint32_t hops, uint32_t *hops_done_out)
{
    struct ef_slot *slot;
    uint64_t offset;
    uint64_t seen[EF_CHASE_VISIT_CACHE];
    uint32_t seen_count = 0;
    uint32_t i;
    uint32_t limit;

    if (db == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return NULL;
    }

    if (hops == 0) {
        if (hops_done_out != NULL) {
            *hops_done_out = 0;
        }
        if (ef_slot_at_offset(db, start_offset, NULL) == NULL) {
            ef_set_error(db, EF_ERR_OFFSET);
            return NULL;
        }
        ef_set_error(db, EF_OK);
        return (struct ef_slot *)ef_offset_to_ptr(db, start_offset);
    }

    limit = hops;
    if (limit > EF_CHASE_MAX_DEPTH) {
        limit = EF_CHASE_MAX_DEPTH;
    }

    if (EF_UNLIKELY(start_offset < db->slots_base || start_offset >= db->file_size)) {
        ef_set_error(db, EF_ERR_OFFSET);
        return NULL;
    }
    if (EF_UNLIKELY((start_offset - db->slots_base) & EF_SLOT_MASK)) {
        ef_set_error(db, EF_ERR_OFFSET);
        return NULL;
    }

    offset = start_offset;
    slot = NULL;

    for (i = 0; i < limit; ++i) {
        if (ef_chase_offset_seen(offset, seen, seen_count)) {
            ef_set_error(db, EF_ERR_CHASE_CYCLE);
            return NULL;
        }
        if (seen_count < EF_CHASE_VISIT_CACHE) {
            seen[seen_count++] = offset;
        }

        slot = (struct ef_slot *)((uint8_t *)db->mmap_addr + offset);
        if (EF_UNLIKELY(slot->status != EF_STATUS_USED)) {
            ef_set_error(db, EF_ERR_SLOT_FREE);
            return NULL;
        }

        if (ef_slot_next_offset_load(slot) == 0) {
            if (hops_done_out != NULL) {
                *hops_done_out = i + 1;
            }
            ef_set_error(db, EF_OK);
            return slot;
        }

        EF_PREFETCH_R((uint8_t *)db->mmap_addr + ef_slot_next_offset_load(slot));
        offset = ef_slot_next_offset_load(slot);
    }

    if (hops > EF_CHASE_MAX_DEPTH) {
        ef_set_error(db, EF_ERR_CHASE_DEPTH);
        return NULL;
    }

    slot = (struct ef_slot *)((uint8_t *)db->mmap_addr + offset);
    if (hops_done_out != NULL) {
        *hops_done_out = limit;
    }
    ef_set_error(db, EF_OK);
    return slot;
}

void *ef_get_field_ptr(struct ef_slot *slot, uint8_t field_offset)
{
    if (slot == NULL || field_offset >= sizeof(struct ef_slot)) {
        return NULL;
    }
    return (uint8_t *)slot + field_offset;
}

enum ef_err ef_unlink_free_slot(struct ef_db *db, uint64_t slot_id)
{
    struct ef_slot *slot;
    struct ef_slot *cursor;
    uint64_t slot_offset;
    uint64_t guard;
    uint64_t max_guard;

    slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        return EF_ERR_SLOT_ID;
    }
    if (slot->status != EF_STATUS_FREE) {
        return EF_OK;
    }

    slot_offset = ef_slot_to_offset(db, slot_id);
    if (db->sb->free_list_head == slot_offset) {
        db->sb->free_list_head = slot->next_offset;
        slot->next_offset = 0;
        return EF_OK;
    }

    cursor = ef_slot_at_offset(db, db->sb->free_list_head, NULL);
    max_guard = db->sb->max_slots + 1U;
    guard = 0;
    while (cursor != NULL && guard < max_guard) {
        ++guard;
        if (cursor->next_offset == slot_offset) {
            cursor->next_offset = slot->next_offset;
            slot->next_offset = 0;
            return EF_OK;
        }
        cursor = ef_slot_at_offset(db, cursor->next_offset, NULL);
    }

    return EF_ERR_NOT_FOUND;
}

enum ef_err ef_set_status(struct ef_db *db, uint64_t slot_id, uint32_t status)
{
    struct ef_slot *slot;
    enum ef_err err;
    uint32_t prior_status;
    uint32_t prior_crc;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        ef_set_error(db, EF_ERR_SLOT_ID);
        return EF_ERR_SLOT_ID;
    }
    prior_status = slot->status;
    prior_crc = slot->header_crc;

    if (status == EF_STATUS_FREE) {
        if (slot->status == EF_STATUS_FREE) {
            ef_set_error(db, EF_ERR_SLOT_FREE);
            return EF_ERR_SLOT_FREE;
        }
        return ef_free_slot(db, slot_id);
    }

    if (status == EF_STATUS_USED) {
        if (slot->status == EF_STATUS_FREE) {
            return ef_claim_slot(db, slot_id);
        }
        if (slot->status == EF_STATUS_USED) {
            ef_set_error(db, EF_OK);
            return EF_OK;
        }
        if (ef_txn_active(db)) {
            (void)ef_txn_record_slot_status(db, slot_id, prior_status, prior_crc);
        }
        slot->status = EF_STATUS_USED;
        ef_slot_header_crc_store(db, slot_id, slot);
        ef_set_error(db, EF_OK);
        return EF_OK;
    }

    if (ef_txn_active(db)) {
        (void)ef_txn_record_slot_status(db, slot_id, prior_status, prior_crc);
    }
    slot->status = status;
    if (ef_slot_status_has_crc(status)) {
        ef_slot_header_crc_store(db, slot_id, slot);
    } else {
        slot->header_crc = 0;
    }
    ef_set_error(db, EF_OK);
    return EF_OK;
}

enum ef_err ef_set_next_offset(struct ef_db *db, uint64_t slot_id, uint64_t next_offset)
{
    struct ef_slot *slot;
    enum ef_err err;
    uint64_t ignored;
    uint64_t prior_next;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    slot = ef_get_slot(db, slot_id);
    if (slot == NULL) {
        return ef_last_error(db);
    }
    if (slot->status != EF_STATUS_USED) {
        ef_set_error(db, EF_ERR_SLOT_FREE);
        return EF_ERR_SLOT_FREE;
    }
    if (ef_slot_has_overflow_chain(db, slot)) {
        ef_set_error(db, EF_ERR_SLOT_BUSY);
        return EF_ERR_SLOT_BUSY;
    }

    if (next_offset != 0) {
        err = ef_offset_to_slot_id(db, next_offset, &ignored);
        if (err != EF_OK) {
            ef_set_error(db, err);
            return err;
        }
    }

    prior_next = ef_slot_next_offset_load(slot);
    if (ef_txn_active(db) && prior_next != next_offset) {
        (void)ef_txn_record_slot_next(db, slot_id, prior_next);
    }
    ef_slot_next_offset_store(slot, next_offset);
    ef_slot_header_crc_store(db, slot_id, slot);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

enum ef_err ef_write_payload(struct ef_db *db, uint64_t slot_id, const void *data, uint8_t len)
{
    struct ef_slot *slot;
    enum ef_err err;
    size_t cap;
    void *payload;
    uint32_t prior_status;
    uint32_t prior_crc;
    uint8_t prior_payload_prefix[8];

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    if (data == NULL && len > 0) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }
    if (len > EF_PAYLOAD_SIZE) {
        ef_set_error(db, EF_ERR_PAYLOAD_LEN);
        return EF_ERR_PAYLOAD_LEN;
    }

    cap = ef_payload_capacity(db);
    if (len > cap) {
        ef_set_error(db, EF_ERR_PAYLOAD_LEN);
        return EF_ERR_PAYLOAD_LEN;
    }

    slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        ef_set_error(db, EF_ERR_SLOT_ID);
        return EF_ERR_SLOT_ID;
    }

    prior_status = slot->status;
    prior_crc = slot->header_crc;
    if (prior_status != EF_STATUS_FREE && ef_slot_payload_ptr(db, slot) != NULL) {
        memcpy(prior_payload_prefix, ef_slot_payload_ptr(db, slot), 8);
    } else {
        memset(prior_payload_prefix, 0, 8);
    }

    if (slot->status == EF_STATUS_FREE) {
        err = ef_claim_slot(db, slot_id);
        if (err != EF_OK) {
            return err;
        }
        slot = ef_peek_slot(db, slot_id);
    } else if (ef_slot_status_has_crc(slot->status)) {
        if (!ef_slot_header_crc_valid(db, slot_id, slot)) {
            ef_set_error(db, EF_ERR_BAD_CHECKSUM);
            return EF_ERR_BAD_CHECKSUM;
        }
    }

    payload = ef_slot_payload_ptr(db, slot);
    if (payload == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }

    if (ef_txn_active(db)) {
        if (prior_status != EF_STATUS_FREE) {
            (void)ef_txn_record_slot_payload(db, slot_id, prior_payload_prefix);
        } else {
            (void)ef_txn_record_slot_status(db, slot_id, prior_status, prior_crc);
        }
    }
    if (len > 0) {
        memcpy(payload, data, len);
    }
    if (len < cap) {
        memset((uint8_t *)payload + len, 0, cap - len);
    }
    slot->status = EF_STATUS_USED;
    ef_slot_header_crc_store(db, slot_id, slot);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

enum ef_err ef_write_field(struct ef_db *db, uint64_t slot_id, uint8_t field_offset, uint8_t value)
{
    struct ef_slot *slot;
    uint8_t *field;
    enum ef_err err;
    uint8_t prior_value;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    slot = ef_get_slot(db, slot_id);
    if (slot == NULL) {
        return ef_last_error(db);
    }

    field = (uint8_t *)ef_get_field_ptr(slot, field_offset);
    if (field == NULL) {
        ef_set_error(db, EF_ERR_OFFSET);
        return EF_ERR_OFFSET;
    }
    prior_value = *field;
    if (ef_txn_active(db) && field_offset == 0) {
        /* Only field 0 (status low byte) is a meaningful 8-byte snapshot
         * target; the rest of the slot write paths already record undo. */
        (void)ef_txn_record_slot_payload(db, slot_id, &prior_value);
    }
    *field = value;
    ef_slot_header_crc_store(db, slot_id, slot);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

enum ef_err ef_alloc_slot_ex(struct ef_db *db, uint64_t *slot_id_out, unsigned flags);

enum ef_err ef_alloc_slot(struct ef_db *db, uint64_t *slot_id_out)
{
    return ef_alloc_slot_ex(db, slot_id_out, EF_ALLOC_ZERO_PAYLOAD);
}

enum ef_err ef_alloc_slot_ex(struct ef_db *db, uint64_t *slot_id_out, unsigned flags)
{
    enum ef_err err;
    const int clear_payload = (flags & EF_ALLOC_ZERO_PAYLOAD) != 0U;
    const int has_seqlock = (db != NULL && db->sb != NULL &&
                            db->sb->schema_version >= EF_SCHEMA_VERSION);

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    if (slot_id_out == NULL || db == NULL || db->sb == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }

    /* If the index uses a seqlock, grab it so that concurrent
     * ef_index_rehash_to cannot memmove slots while we modify one.
     * ef_index_grow_for_insert holds this same lock for the duration of
     * the rehash. We determine whether the seqlock is needed from
     * schema_version only (schema_version itself is stable after open);
     * the index layout may have hash_capacity == 0 in which case rehash
     * never runs and the lock is a cheap no-op. */
    if (has_seqlock) {
        err = ef_sb_index_write_lock_acquire(db->sb);
        if (err != EF_OK) {
            ef_set_error(db, err);
            return err;
        }
    }
#if EF_HAS_HW_ATOMICS
    err = ef_free_list_pop_atomic(db, slot_id_out, clear_payload);
    if (err == EF_OK && ef_txn_active(db)) {
        struct ef_slot *slot = ef_peek_slot(db, *slot_id_out);
        if (slot != NULL) {
            uint32_t prior_status = EF_STATUS_FREE;
            uint32_t prior_crc = slot->header_crc;
            uint64_t prior_next = ef_slot_next_offset_load(slot);
            (void)prior_next;
            (void)ef_txn_record_slot_alloc(db, *slot_id_out, prior_status, prior_crc);
            (void)ef_txn_record_free_count(db, -1);
        }
    }
    if (has_seqlock) {
        ef_sb_index_write_lock_release(db->sb);
    }
    return err;
#else
    {
        struct ef_slot *slot;
        uint64_t slot_id;
        uint32_t prior_status = EF_STATUS_FREE;
        uint32_t prior_crc = 0;

        if (db->sb->free_list_head == 0 || ef_sb_free_count_load(db->sb) == 0) {
            ef_set_error(db, EF_ERR_SLOT_FULL);
            if (has_seqlock) {
                ef_sb_index_write_lock_release(db->sb);
            }
            return EF_ERR_SLOT_FULL;
        }

        slot = (struct ef_slot *)ef_offset_to_ptr(db, db->sb->free_list_head);
        if (slot == NULL) {
            if (has_seqlock) {
                ef_sb_index_write_lock_release(db->sb);
            }
            return ef_last_error(db);
        }

        err = ef_offset_to_slot_id(db, db->sb->free_list_head, &slot_id);
        if (err != EF_OK) {
            ef_set_error(db, err);
            if (has_seqlock) {
                ef_sb_index_write_lock_release(db->sb);
            }
            return err;
        }

        prior_crc = slot->header_crc;
        db->sb->free_list_head = slot->next_offset;
        slot->next_offset = 0;
        slot->status = EF_STATUS_USED;
        if (clear_payload) {
            memset(ef_slot_payload_ptr(db, slot), 0, ef_payload_capacity(db));
        }
        ef_sb_free_count_dec(db->sb);
        ef_slot_header_crc_store(db, slot_id, slot);
        ef_db_mark_meta_dirty(db);

        if (ef_txn_active(db)) {
            (void)ef_txn_record_slot_alloc(db, slot_id, prior_status, prior_crc);
            (void)ef_txn_record_free_count(db, -1);
        }

        *slot_id_out = slot_id;
        ef_set_error(db, EF_OK);
        if (has_seqlock) {
            ef_sb_index_write_lock_release(db->sb);
        }
        return EF_OK;
    }
#endif
}

struct ef_slot *ef_alloc_slot_ptr(struct ef_db *db, uint64_t *slot_id_out)
{
    if (ef_alloc_slot(db, slot_id_out) != EF_OK) {
        return NULL;
    }
    return ef_peek_slot(db, *slot_id_out);
}

enum ef_err ef_free_slot(struct ef_db *db, uint64_t slot_id)
{
    struct ef_slot *slot;
    enum ef_err err;
    uint32_t prior_status;
    uint64_t prior_next;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    slot = ef_peek_slot(db, slot_id);
    if (slot == NULL) {
        return ef_last_error(db);
    }
    prior_status = slot->status;
    prior_next = ef_slot_next_offset_load(slot);
    if (slot->status == EF_STATUS_FREE) {
        ef_set_error(db, EF_ERR_SLOT_FREE);
        return EF_ERR_SLOT_FREE;
    }
    if (slot->status == EF_STATUS_QUEUED) {
        ef_set_error(db, EF_ERR_SLOT_BUSY);
        return EF_ERR_SLOT_BUSY;
    }
    if (slot->status == EF_STATUS_QUEUE_DUMMY || slot->status == EF_STATUS_QUEUE_LINK ||
        slot->status == EF_STATUS_QUEUE_DEQ) {
        ef_set_error(db, EF_ERR_SLOT_BUSY);
        return EF_ERR_SLOT_BUSY;
    }

    if (slot->status == EF_STATUS_USED || slot->status == EF_STATUS_OVERFLOW) {
        if (!ef_slot_header_crc_valid(db, slot_id, slot)) {
            ef_set_error(db, EF_ERR_BAD_CHECKSUM);
            return EF_ERR_BAD_CHECKSUM;
        }
    }

    if (slot->status == EF_STATUS_USED) {
        err = ef_blob_release_chain(db, slot_id, slot);
        if (err != EF_OK) {
            ef_set_error(db, err);
            return err;
        }
    }

    if (ef_txn_active(db)) {
        (void)ef_txn_record_slot_free(db, slot_id, prior_next, prior_status);
        (void)ef_txn_record_free_count(db, 1);
    }
    return ef_return_slot_to_pool(db, slot_id, slot);
}

uint64_t ef_count_free_slots(const struct ef_db *db)
{
    if (db == NULL || db->sb == NULL) {
        return 0;
    }
    return ef_sb_free_count_load(db->sb);
}

enum ef_err ef_alloc_ex(struct ef_db *db, uint64_t *slot_id_out, unsigned flags)
{
    enum ef_err err;

    err = ef_alloc_slot_ex(db, slot_id_out, flags);
    if (err != EF_ERR_SLOT_FULL) {
        return err;
    }

    if (db == NULL || db->sb == NULL) {
        return EF_ERR_NULL_ARG;
    }

    err = ef_grow(db, db->sb->max_slots + 1);
    if (err != EF_OK) {
        return err;
    }

    return ef_alloc_slot_ex(db, slot_id_out, flags);
}

enum ef_err ef_alloc(struct ef_db *db, uint64_t *slot_id_out)
{
    return ef_alloc_ex(db, slot_id_out, EF_ALLOC_ZERO_PAYLOAD);
}
