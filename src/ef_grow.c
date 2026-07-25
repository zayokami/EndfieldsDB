#include "endfields.h"
#include "ef_index.h"
#include "ef_internal.h"
#include "ef_port.h"
#include "ef_sb_layout.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Forward decls for endfields.c-only helpers used by grow/upgrade. These are
 * exposed via the implementation file pattern: they're file-static in
 * endfields.c and called by name. We mirror the prototypes here. */
size_t ef_expected_file_size(uint64_t max_slots, uint32_t hash_capacity);
void ef_db_to_io(const struct ef_db *db, struct ef_io *io);
void ef_db_bind_io(struct ef_db *db, const struct ef_io *io);
void ef_db_refresh_checksums(struct ef_db *db);

#define EF_SCHEMA_LEGACY 1U

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
        ef_sb_free_count_inc(db->sb);
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
