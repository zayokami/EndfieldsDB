#include "endfields.h"
#include "ef_index.h"
#include "ef_port.h"
#include "ef_crc.h"
#include "ef_atomic_unaligned.h"
#include "ef_sb_layout.h"

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

static uint64_t ef_slot_next_offset_load(const struct ef_slot *slot)
{
    return ef_atomic_load_u64((const unsigned char *)slot + offsetof(struct ef_slot, next_offset));
}

static void ef_slot_next_offset_store(struct ef_slot *slot, uint64_t value)
{
    ef_atomic_store_u64((unsigned char *)slot + offsetof(struct ef_slot, next_offset), value);
}

#define EF_QUEUE_SPIN_MAX 65536U

#if EF_HAS_HW_ATOMICS
static void ef_queue_yield(uint32_t spins)
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
static void ef_queue_yield(uint32_t spins)
{
    (void)spins;
}
#endif

static void ef_set_error(struct ef_db *db, enum ef_err err)
{
    if (db != NULL) {
        db->last_err = err;
    }
}

static uint32_t *ef_sb_checksum_ptr(struct ef_superblock *sb)
{
    return (uint32_t *)&sb->reserved[0];
}

static const uint32_t *ef_sb_checksum_ptr_ro(const struct ef_superblock *sb)
{
    return (const uint32_t *)&sb->reserved[0];
}

static uint32_t ef_sb_checksum_compute(const struct ef_superblock *sb)
{
    uint32_t crc;
    uint32_t zero_crc = 0;

    crc = ef_crc32_update(0xFFFFFFFFU, sb, offsetof(struct ef_superblock, reserved));
    crc = ef_crc32_update(crc, &zero_crc, sizeof(zero_crc));
    crc = ef_crc32_update(crc, sb->reserved + sizeof(uint32_t),
                          sizeof(sb->reserved) - sizeof(uint32_t));
    return crc ^ 0xFFFFFFFFU;
}

static void ef_sb_checksum_store(struct ef_superblock *sb)
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
        db->sb_meta_dirty = 1;
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
    if (db->sb_meta_dirty) {
        ef_sb_checksum_store(db->sb);
        db->sb_meta_dirty = 0;
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

static uint32_t ef_slot_header_crc_compute(uint64_t slot_id, const struct ef_slot *slot)
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

static struct ef_slot *ef_slot_by_id(struct ef_db *db, uint64_t slot_id)
{
    if (db == NULL || db->sb == NULL || db->slots == NULL) {
        return NULL;
    }
    if (EF_UNLIKELY(slot_id >= db->sb->max_slots)) {
        return NULL;
    }
    return db->slots + slot_id;
}

static void ef_slot_header_crc_store(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot)
{
    if (!(db->sb->flags & EF_FLAG_SLOT_CRC)) {
        return;
    }
    slot->header_crc = ef_slot_header_crc_compute(slot_id, slot);
}

#if EF_HAS_HW_ATOMICS
static enum ef_err ef_free_list_pop_atomic(struct ef_db *db, uint64_t *slot_id_out)
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
        if (head == 0 || db->sb->free_count == 0) {
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
            slot->status = EF_STATUS_USED;
            memset(ef_slot_payload_ptr(db, slot), 0, ef_payload_capacity(db));
            if (db->sb->free_count > 0) {
                --db->sb->free_count;
            }
            ef_slot_header_crc_store(db, slot_id, slot);
            ef_db_mark_meta_dirty(db);
            *slot_id_out = slot_id;
            ef_set_error(db, EF_OK);
            return EF_OK;
        }
    }
}

static enum ef_err ef_free_list_push_atomic(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot)
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
        EF_ATOMIC_THREAD_FENCE();

        exp = head;
        if (EF_ATOMIC_CAS_U64(head_ptr, &exp, slot_offset)) {
            slot->status = EF_STATUS_FREE;
            ++db->sb->free_count;
            ef_db_mark_meta_dirty(db);
            ef_set_error(db, EF_OK);
            return EF_OK;
        }
    }
}
#endif

static int ef_slot_status_has_crc(uint32_t status)
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

static int ef_hash_capacity_valid(uint32_t hash_capacity)
{
    if (hash_capacity == 0) {
        return 1;
    }
    if (hash_capacity > 0xFFFFU) {
        return 0;
    }
    return (hash_capacity & (hash_capacity - 1U)) == 0;
}

static int ef_slot_header_crc_valid(struct ef_db *db, uint64_t slot_id, const struct ef_slot *slot)
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
    return slot->header_crc == ef_slot_header_crc_compute(slot_id, slot);
}

static enum ef_err ef_db_require_write(struct ef_db *db)
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

static void ef_db_refresh_checksums(struct ef_db *db)
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

static size_t ef_expected_file_size(uint64_t max_slots, uint32_t hash_capacity)
{
    return (size_t)(sizeof(struct ef_superblock) +
                    (uint64_t)hash_capacity * sizeof(struct ef_hash_entry) +
                    max_slots * sizeof(struct ef_slot));
}

static void ef_db_bind_slots_layout(struct ef_db *db)
{
    uint32_t hash_capacity;

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
}

static void ef_db_bind_io(struct ef_db *db, const struct ef_io *io)
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

static void ef_db_to_io(const struct ef_db *db, struct ef_io *io)
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
        sb->schema_version != EF_SCHEMA_VERSION) {
        return EF_ERR_BAD_VERSION;
    }

    if (!ef_hash_capacity_valid(ef_hash_capacity_from_sb(sb))) {
        return EF_ERR_BAD_VERSION;
    }

    expected = ef_expected_file_size(sb->max_slots, ef_hash_capacity_from_sb(sb));
    if (file_size != expected) {
        return EF_ERR_FILE_SIZE;
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
    sb->free_count = (uint32_t)max_slots;
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
    sb->free_count = (uint32_t)free_count;
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

static enum ef_err ef_unlink_free_slot(struct ef_db *db, uint64_t slot_id);
static struct ef_slot *ef_slot_at_offset(struct ef_db *db, uint64_t offset, uint64_t *slot_id_out);

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
        db->sb->free_count = 0;
        return EF_OK;
    }

    for (i = 0; i < max_slots; ++i) {
        slot = db->slots + i;
        slot->status = EF_STATUS_FREE;
        memset(slot->payload, 0, sizeof(slot->payload));
        slot->next_offset = (i + 1 < max_slots) ? ef_slot_to_offset(db, i + 1) : 0;
    }

    db->sb->free_list_head = ef_slot_to_offset(db, 0);
    db->sb->free_count = (uint32_t)max_slots;
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
        if (db->sb->schema_version == EF_SCHEMA_VERSION_V3 &&
            ef_sb_hash_capacity_load(db->sb) > 0U) {
            err = ef_sb_migrate_v3_index_layout(db->sb);
            if (err != EF_OK) {
                return err;
            }
            ef_db_bind_slots_layout(db);
            ef_db_mark_meta_dirty(db);
        }
        err = ef_rebuild_free_list(db);
        if (err != EF_OK) {
            return err;
        }
    }

    return EF_OK;
}

static enum ef_err ef_unlink_free_slot(struct ef_db *db, uint64_t slot_id);

static enum ef_err ef_claim_slot(struct ef_db *db, uint64_t slot_id)
{
    struct ef_slot *slot;
    enum ef_err err;

    slot = ef_slot_by_id(db, slot_id);
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
    if (db->sb->free_count > 0) {
        --db->sb->free_count;
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
    default: return "unknown error";
    }
}

enum ef_err ef_last_error(const struct ef_db *db)
{
    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    return db->last_err;
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
    file_size = ef_expected_file_size(initial_slots, hash_capacity);

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
    err = ef_db_init_mapped(db, is_new_file, initial_slots, hash_capacity);
    if (err != EF_OK) {
        ef_port_close(&io);
        free(db);
        return err;
    }

    db->last_err = EF_OK;
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
    db->last_err = EF_OK;
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
    need = ef_expected_file_size(max_slots, hash_capacity);
    if (!init_new) {
        const struct ef_superblock *sb_probe = (const struct ef_superblock *)buffer;
        size_t on_disk_need;

        if (ef_magic_valid(sb_probe) && sb_probe->slot_size == EF_SLOT_SIZE) {
            on_disk_need = ef_expected_file_size(sb_probe->max_slots,
                                                 ef_hash_capacity_from_sb(sb_probe));
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

    db->last_err = EF_OK;
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

static int ef_slot_has_overflow_chain(struct ef_db *db, const struct ef_slot *head)
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

static enum ef_err ef_return_slot_to_pool(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot)
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
    uint64_t slot_offset = ef_slot_to_offset(db, slot_id);
    slot->next_offset = db->sb->free_list_head;
    db->sb->free_list_head = slot_offset;
    slot->status = EF_STATUS_FREE;
    slot->header_crc = 0;
    memset(ef_slot_payload_ptr(db, slot), 0, ef_payload_capacity(db));
    ++db->sb->free_count;
    ef_db_mark_meta_dirty(db);
    ef_set_error(db, EF_OK);
    return EF_OK;
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

static enum ef_err ef_blob_free_overflow_chain(struct ef_db *db, uint64_t head_id, struct ef_slot *head)
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

    err = ef_blob_free_overflow_chain(db, slot_id, head);
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
                (void)ef_blob_free_overflow_chain(db, slot_id, head);
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

enum ef_err ef_foreach_used(struct ef_db *db, ef_slot_visit_fn fn, void *ctx)
{
    uint64_t i;

    if (db == NULL || db->sb == NULL || db->slots == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }
    if (fn == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }

    for (i = 0; i < db->sb->max_slots; ++i) {
        struct ef_slot *slot = db->slots + i;

        if (slot->status != EF_STATUS_USED) {
            continue;
        }
        if (ef_slot_header_crc_valid(db, i, slot) == 0) {
            ef_set_error(db, EF_ERR_BAD_CHECKSUM);
            return EF_ERR_BAD_CHECKSUM;
        }
        if (!fn(db, i, slot, ctx)) {
            break;
        }
    }

    ef_set_error(db, EF_OK);
    return EF_OK;
}

void ef_slot_iter_init(struct ef_db *db, struct ef_slot_iter *it)
{
    if (it == NULL) {
        return;
    }
    it->db = db;
    it->index = 0;
}

int ef_slot_iter_next(struct ef_slot_iter *it, uint64_t *slot_id_out, struct ef_slot **slot_out)
{
    if (it == NULL || it->db == NULL || it->db->sb == NULL || it->db->slots == NULL) {
        return 0;
    }

    while (it->index < it->db->sb->max_slots) {
        uint64_t i = it->index++;
        struct ef_slot *slot = it->db->slots + i;

        if (slot->status != EF_STATUS_USED) {
            continue;
        }
        if (ef_slot_header_crc_valid(it->db, i, slot) == 0) {
            ef_set_error(it->db, EF_ERR_BAD_CHECKSUM);
            return -1;
        }
        if (slot_id_out != NULL) {
            *slot_id_out = i;
        }
        if (slot_out != NULL) {
            *slot_out = slot;
        }
        ef_set_error(it->db, EF_OK);
        return 1;
    }

    ef_set_error(it->db, EF_OK);
    return 0;
}

static struct ef_slot *ef_slot_at_offset(struct ef_db *db, uint64_t offset, uint64_t *slot_id_out)
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

static enum ef_err ef_upgrade_slots_v1_to_v2(struct ef_db *db)
{
    uint64_t i;
    uint8_t legacy[EF_PAYLOAD_SIZE_LEGACY];

    for (i = 0; i < db->sb->max_slots; ++i) {
        struct ef_slot *slot = db->slots + i;

        if (slot->status == EF_STATUS_USED) {
            memcpy(legacy, &slot->header_crc, EF_PAYLOAD_SIZE_LEGACY);
            memset(slot->payload, 0, sizeof(slot->payload));
            memcpy(slot->payload, legacy, EF_PAYLOAD_SIZE);
            slot->header_crc = 0;
            ef_slot_header_crc_store(db, i, slot);
        } else if (slot->status == EF_STATUS_FREE) {
            slot->header_crc = 0;
        }
    }

    return EF_OK;
}

int ef_needs_upgrade(const struct ef_db *db)
{
    if (db == NULL || db->sb == NULL) {
        return 0;
    }
    if (db->sb->schema_version == EF_SCHEMA_VERSION ||
        db->sb->schema_version == EF_SCHEMA_VERSION_V3 ||
        db->sb->schema_version == EF_SCHEMA_VERSION_V2) {
        return (db->sb->flags & (EF_FLAG_SB_CRC | EF_FLAG_SLOT_CRC)) !=
               (EF_FLAG_SB_CRC | EF_FLAG_SLOT_CRC);
    }
    return db->sb->schema_version == 0 || db->sb->schema_version == EF_SCHEMA_LEGACY;
}

enum ef_err ef_upgrade(struct ef_db *db)
{
    enum ef_err err;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }
    if (db->sb == NULL || db->slots == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }

    if (!ef_needs_upgrade(db)) {
        ef_set_error(db, EF_OK);
        return EF_OK;
    }

    if (db->sb->schema_version != EF_SCHEMA_VERSION &&
        db->sb->schema_version != EF_SCHEMA_VERSION_V3 &&
        db->sb->schema_version != EF_SCHEMA_VERSION_V2 &&
        db->sb->schema_version != EF_SCHEMA_LEGACY &&
        db->sb->schema_version != 0) {
        ef_set_error(db, EF_ERR_BAD_VERSION);
        return EF_ERR_BAD_VERSION;
    }

    err = ef_upgrade_slots_v1_to_v2(db);
    if (err != EF_OK) {
        ef_set_error(db, err);
        return err;
    }

    db->sb->schema_version = EF_SCHEMA_VERSION;
    db->sb->flags = EF_FLAG_SB_CRC | EF_FLAG_SLOT_CRC;
    ef_db_refresh_checksums(db);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

static enum ef_err ef_grow_append_slots(struct ef_db *db, uint64_t old_max, uint64_t new_max)
{
    uint64_t i;
    struct ef_slot *slot;

    db->sb->max_slots = new_max;

    for (i = old_max; i < new_max; ++i) {
        slot = db->slots + i;
        memset(slot, 0, sizeof(*slot));
        slot->status = EF_STATUS_FREE;
        slot->next_offset = db->sb->free_list_head;
        db->sb->free_list_head = ef_slot_to_offset(db, i);
        ++db->sb->free_count;
    }

    ef_db_mark_meta_dirty(db);
    return EF_OK;
}

enum ef_err ef_grow(struct ef_db *db, uint64_t new_max_slots)
{
    uint64_t old_max;
    size_t new_size;
    enum ef_err err;
#if EF_HAS_FILE_IO
    struct ef_io io;
#endif

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    if (db->sb == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }

    old_max = db->sb->max_slots;
    if (new_max_slots <= old_max) {
        ef_set_error(db, EF_ERR_GROW);
        return EF_ERR_GROW;
    }

    new_size = ef_expected_file_size(new_max_slots, db->hash_capacity);
    if (db->backend == EF_BACKEND_MEMORY && new_size > db->map_capacity) {
        ef_set_error(db, EF_ERR_FILE_SIZE);
        return EF_ERR_FILE_SIZE;
    }

#if EF_HAS_FILE_IO
    if (db->backend == EF_BACKEND_FILE) {
        ef_db_to_io(db, &io);
        err = ef_port_grow_file(&io, new_size);
        if (err != EF_OK) {
            ef_set_error(db, err);
            return err;
        }
        ef_db_bind_io(db, &io);
    } else
#endif
    {
        db->file_size = new_size;
    }

    memset((uint8_t *)db->mmap_addr + ef_expected_file_size(old_max, db->hash_capacity), 0,
           new_size - ef_expected_file_size(old_max, db->hash_capacity));

    err = ef_grow_append_slots(db, old_max, new_max_slots);
    ef_set_error(db, err);
    return err;
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

struct ef_slot *ef_get_slot(struct ef_db *db, uint64_t slot_id)
{
    if (EF_UNLIKELY(db == NULL || db->sb == NULL || db->slots == NULL)) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return NULL;
    }
    if (EF_UNLIKELY(slot_id >= db->sb->max_slots)) {
        ef_set_error(db, EF_ERR_SLOT_ID);
        return NULL;
    }

    if (ef_slot_status_has_crc(db->slots[slot_id].status)) {
        if (ef_slot_verify_used(db, slot_id, db->slots + slot_id) != EF_OK) {
            return NULL;
        }
    }

    ef_set_error(db, EF_OK);
    return db->slots + slot_id;
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

static enum ef_err ef_unlink_free_slot(struct ef_db *db, uint64_t slot_id)
{
    struct ef_slot *slot;
    struct ef_slot *cursor;
    uint64_t slot_offset;
    uint64_t guard;
    uint64_t max_guard;

    slot = ef_slot_by_id(db, slot_id);
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

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    slot = ef_slot_by_id(db, slot_id);
    if (slot == NULL) {
        ef_set_error(db, EF_ERR_SLOT_ID);
        return EF_ERR_SLOT_ID;
    }

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
        slot->status = EF_STATUS_USED;
        ef_slot_header_crc_store(db, slot_id, slot);
        ef_set_error(db, EF_OK);
        return EF_OK;
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

    slot = ef_slot_by_id(db, slot_id);
    if (slot == NULL) {
        ef_set_error(db, EF_ERR_SLOT_ID);
        return EF_ERR_SLOT_ID;
    }

    if (slot->status == EF_STATUS_FREE) {
        err = ef_claim_slot(db, slot_id);
        if (err != EF_OK) {
            return err;
        }
        slot = ef_slot_by_id(db, slot_id);
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

    *field = value;
    ef_slot_header_crc_store(db, slot_id, slot);
    ef_set_error(db, EF_OK);
    return EF_OK;
}

enum ef_err ef_alloc_slot(struct ef_db *db, uint64_t *slot_id_out)
{
    enum ef_err err;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    if (slot_id_out == NULL || db == NULL || db->sb == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return EF_ERR_NULL_ARG;
    }
#if EF_HAS_HW_ATOMICS
    return ef_free_list_pop_atomic(db, slot_id_out);
#else
    struct ef_slot *slot;
    uint64_t slot_id;

    if (db->sb->free_list_head == 0 || db->sb->free_count == 0) {
        ef_set_error(db, EF_ERR_SLOT_FULL);
        return EF_ERR_SLOT_FULL;
    }

    slot = (struct ef_slot *)ef_offset_to_ptr(db, db->sb->free_list_head);
    if (slot == NULL) {
        return ef_last_error(db);
    }

    err = ef_offset_to_slot_id(db, db->sb->free_list_head, &slot_id);
    if (err != EF_OK) {
        ef_set_error(db, err);
        return err;
    }

    db->sb->free_list_head = slot->next_offset;
    slot->next_offset = 0;
    slot->status = EF_STATUS_USED;
    memset(ef_slot_payload_ptr(db, slot), 0, ef_payload_capacity(db));
    --db->sb->free_count;
    ef_slot_header_crc_store(db, slot_id, slot);
    ef_db_mark_meta_dirty(db);

    *slot_id_out = slot_id;
    ef_set_error(db, EF_OK);
    return EF_OK;
#endif
}

struct ef_slot *ef_alloc_slot_ptr(struct ef_db *db, uint64_t *slot_id_out)
{
    if (ef_alloc_slot(db, slot_id_out) != EF_OK) {
        return NULL;
    }
    return ef_get_slot(db, *slot_id_out);
}

enum ef_err ef_free_slot(struct ef_db *db, uint64_t slot_id)
{
    struct ef_slot *slot;
    enum ef_err err;

    err = ef_db_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    slot = ef_get_slot(db, slot_id);
    if (slot == NULL) {
        return ef_last_error(db);
    }
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

    if (slot->status == EF_STATUS_USED) {
        err = ef_blob_free_overflow_chain(db, slot_id, slot);
        if (err != EF_OK) {
            ef_set_error(db, err);
            return err;
        }
        slot = ef_get_slot(db, slot_id);
        if (slot == NULL) {
            return ef_last_error(db);
        }
    }

    return ef_return_slot_to_pool(db, slot_id, slot);
}

uint64_t ef_count_free_slots(const struct ef_db *db)
{
    if (db == NULL || db->sb == NULL) {
        return 0;
    }
    return db->sb->free_count;
}

enum ef_err ef_alloc(struct ef_db *db, uint64_t *slot_id_out)
{
    enum ef_err err;

    err = ef_alloc_slot(db, slot_id_out);
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

    return ef_alloc_slot(db, slot_id_out);
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

    dummy = ef_slot_by_id(db, dummy_id);
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
    uint8_t local[EF_PAYLOAD_SIZE];
    uint8_t stored_len;
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
    memcpy(local, ef_slot_payload_ptr(db, first), ef_payload_capacity(db));
    stored_len = local[0];
    if ((size_t)stored_len + 1 > buf_cap) {
        ef_queue_lock_release(db);
        ef_set_error(db, EF_ERR_PAYLOAD_LEN);
        return EF_ERR_PAYLOAD_LEN;
    }

    ef_slot_next_offset_store(dummy, first_next);
    if (tail_off == first_off) {
        EF_ATOMIC_STORE_U64(tail_ptr, first_next != 0 ? first_next : dummy_offset);
    }
    ef_slot_header_crc_store(db, dummy_id, dummy);
    err = ef_return_slot_to_pool(db, first_id, first);
    ef_queue_lock_release(db);
    ef_db_mark_meta_dirty(db);

    if (stored_len > 0) {
        memcpy(buf, local + 1, stored_len);
    }
    *out_len = stored_len;
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

    err = ef_alloc(db, &slot_id);
    if (err != EF_OK) {
        return err;
    }

    slot = ef_slot_by_id(db, slot_id);
    if (slot == NULL) {
        ef_set_error(db, EF_ERR_SLOT_ID);
        return EF_ERR_SLOT_ID;
    }

    payload = ef_slot_payload_ptr(db, slot);
    if ((size_t)len + 1 > cap) {
        ef_free_slot(db, slot_id);
        ef_set_error(db, EF_ERR_PAYLOAD_LEN);
        return EF_ERR_PAYLOAD_LEN;
    }
    ((uint8_t *)payload)[0] = len;
    if (len > 0) {
        memcpy((uint8_t *)payload + 1, data, len);
    }
    if ((size_t)len + 1 < cap) {
        memset((uint8_t *)payload + len + 1, 0, cap - (size_t)len - 1);
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

void *ef_execute(struct ef_db *db, struct ef_cmd *cmd, const void *aux)
{
    struct ef_slot *slot;
    const uint64_t *next_ptr;
    const uint32_t *status_ptr;
    const uint32_t *hops_ptr;
    const uint8_t *byte_ptr;
    enum ef_err err;

    if (db == NULL || cmd == NULL) {
        ef_set_error(db, EF_ERR_NULL_ARG);
        return NULL;
    }

    switch (cmd->opcode) {
    case EF_OP_GET_SLOT:
        return ef_get_slot(db, cmd->param);
    case EF_OP_CHASE:
        return ef_chase(db, (struct ef_slot *)ef_offset_to_ptr(db, cmd->param));
    case EF_OP_GET_FIELD:
        slot = ef_get_slot(db, cmd->param);
        return (slot == NULL) ? NULL : ef_get_field_ptr(slot, cmd->field_offset);
    case EF_OP_WRITE_PAYLOAD:
        err = ef_write_payload(db, cmd->param, aux, cmd->field_offset);
        return (err == EF_OK) ? ef_get_slot(db, cmd->param) : NULL;
    case EF_OP_SET_NEXT:
        if (aux == NULL) {
            ef_set_error(db, EF_ERR_NULL_ARG);
            return NULL;
        }
        next_ptr = (const uint64_t *)aux;
        err = ef_set_next_offset(db, cmd->param, *next_ptr);
        return (err == EF_OK) ? ef_get_slot(db, cmd->param) : NULL;
    case EF_OP_SET_STATUS:
        if (aux == NULL) {
            ef_set_error(db, EF_ERR_NULL_ARG);
            return NULL;
        }
        status_ptr = (const uint32_t *)aux;
        err = ef_set_status(db, cmd->param, *status_ptr);
        return (err == EF_OK) ? ef_get_slot(db, cmd->param) : NULL;
    case EF_OP_WRITE_FIELD:
        if (aux == NULL) {
            ef_set_error(db, EF_ERR_NULL_ARG);
            return NULL;
        }
        byte_ptr = (const uint8_t *)aux;
        err = ef_write_field(db, cmd->param, cmd->field_offset, *byte_ptr);
        return (err == EF_OK) ? ef_get_field_ptr(ef_get_slot(db, cmd->param), cmd->field_offset) : NULL;
    case EF_OP_ALLOC:
        if (aux == NULL) {
            ef_set_error(db, EF_ERR_NULL_ARG);
            return NULL;
        }
        err = ef_alloc_slot(db, (uint64_t *)aux);
        return (err == EF_OK) ? ef_get_slot(db, *(const uint64_t *)aux) : NULL;
    case EF_OP_FREE:
        err = ef_free_slot(db, cmd->param);
        return (err == EF_OK) ? NULL : NULL;
    case EF_OP_CHASE_N:
        if (cmd->field_offset == 0) {
            if (aux == NULL) {
                ef_set_error(db, EF_ERR_NULL_ARG);
                return NULL;
            }
            hops_ptr = (const uint32_t *)aux;
            return ef_chase_n(db, cmd->param, *hops_ptr, NULL);
        }
        return ef_chase_n(db, cmd->param, cmd->field_offset, NULL);
    default:
        ef_set_error(db, EF_ERR_OPCODE);
        return NULL;
    }
}
