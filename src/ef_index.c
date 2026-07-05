#include "ef_index.h"
#include "ef_port.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define EF_SB_SIZE 64U
#define EF_SB_OFF_QUEUE_HEAD 4U
#define EF_SB_OFF_QUEUE_TAIL 12U
#define EF_SB_OFF_HASH_CAP  20U

static uint32_t *ef_idx_hash_cap_ptr(struct ef_superblock *sb)
{
    return (uint32_t *)&sb->reserved[EF_SB_OFF_HASH_CAP];
}

static uint64_t *ef_idx_queue_head_ptr(struct ef_superblock *sb)
{
    return (uint64_t *)&sb->reserved[EF_SB_OFF_QUEUE_HEAD];
}

static uint64_t *ef_idx_queue_tail_ptr(struct ef_superblock *sb)
{
    return (uint64_t *)&sb->reserved[EF_SB_OFF_QUEUE_TAIL];
}

static size_t ef_index_file_size(uint64_t max_slots, uint32_t hash_capacity)
{
    return (size_t)(EF_SB_SIZE + (uint64_t)hash_capacity * EF_HASH_ENTRY_SIZE +
                    max_slots * sizeof(struct ef_slot));
}

static uint64_t ef_index_slots_base(uint32_t hash_capacity)
{
    return (uint64_t)EF_SB_SIZE + (uint64_t)hash_capacity * EF_HASH_ENTRY_SIZE;
}

static void ef_index_bind_layout(struct ef_db *db)
{
    if (db == NULL || db->sb == NULL || db->mmap_addr == NULL) {
        return;
    }

    db->hash_capacity = *ef_idx_hash_cap_ptr(db->sb);
    if (db->hash_capacity > 0) {
        db->hash_index = (struct ef_hash_entry *)((uint8_t *)db->mmap_addr + EF_SB_SIZE);
        db->slots_base = ef_index_slots_base(db->hash_capacity);
    } else {
        db->hash_index = NULL;
        db->slots_base = EF_SB_SIZE;
    }
    db->slots = (struct ef_slot *)((uint8_t *)db->mmap_addr + db->slots_base);
}

static uint64_t ef_index_fixup_offset(uint64_t offset, uint64_t old_base, uint64_t new_base)
{
    if (offset == 0 || offset < old_base) {
        return offset;
    }
    return offset + (new_base - old_base);
}

static void ef_index_fixup_all_offsets(struct ef_db *db, uint64_t old_base, uint64_t new_base)
{
    uint64_t i;

    db->sb->free_list_head =
        ef_index_fixup_offset(db->sb->free_list_head, old_base, new_base);
    *ef_idx_queue_head_ptr(db->sb) =
        ef_index_fixup_offset(*ef_idx_queue_head_ptr(db->sb), old_base, new_base);
    *ef_idx_queue_tail_ptr(db->sb) =
        ef_index_fixup_offset(*ef_idx_queue_tail_ptr(db->sb), old_base, new_base);

    for (i = 0; i < db->sb->max_slots; ++i) {
        struct ef_slot *slot = db->slots + i;
        if (slot->next_offset >= old_base) {
            slot->next_offset += (new_base - old_base);
        }
    }
}

static int ef_hash_entry_empty(const struct ef_hash_entry *entry)
{
    return entry->slot_offset == 0;
}

static uint32_t ef_hash_home(uint32_t capacity, uint64_t key_hash)
{
    return (uint32_t)(key_hash & (uint64_t)(capacity - 1U));
}

static uint32_t ef_hash_probe_dist(uint32_t capacity, uint32_t home, uint32_t pos)
{
    return (pos + capacity - home) % capacity;
}

uint64_t ef_key_hash(const char *key, size_t key_len)
{
    uint64_t hash = 14695981039346656037ULL;
    size_t i;

    if (key == NULL) {
        return 0;
    }

    for (i = 0; i < key_len; ++i) {
        hash ^= (uint64_t)(unsigned char)key[i];
        hash *= 1099511628211ULL;
    }

    return hash == 0 ? 1ULL : hash;
}

static enum ef_err ef_index_put_entry(struct ef_db *db, uint64_t key_hash, uint64_t slot_offset)
{
    uint32_t capacity;
    uint32_t home;
    uint32_t i;
    struct ef_hash_entry incoming;
    struct ef_hash_entry outgoing;

    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return EF_ERR_NULL_ARG;
    }

    capacity = db->hash_capacity;
    if ((capacity & (capacity - 1U)) != 0) {
        return EF_ERR_BAD_VERSION;
    }

    incoming.key_hash = key_hash;
    incoming.slot_offset = slot_offset;
    home = ef_hash_home(capacity, key_hash);

    for (i = home; i < home + capacity; ++i) {
        struct ef_hash_entry *entry = db->hash_index + (i % capacity);

        if (ef_hash_entry_empty(entry)) {
            *entry = incoming;
            return EF_OK;
        }

        if (entry->key_hash == key_hash) {
            entry->slot_offset = slot_offset;
            return EF_OK;
        }

        if (ef_hash_probe_dist(capacity, home, i % capacity) >
            ef_hash_probe_dist(capacity, ef_hash_home(capacity, entry->key_hash),
                               (uint32_t)(entry - db->hash_index))) {
            outgoing = *entry;
            *entry = incoming;
            incoming = outgoing;
            home = ef_hash_home(capacity, incoming.key_hash);
        }
    }

    return EF_ERR_INDEX_FULL;
}

static enum ef_err ef_index_find_entry(const struct ef_db *db, uint64_t key_hash,
                                       struct ef_hash_entry *out)
{
    uint32_t capacity;
    uint32_t home;
    uint32_t i;

    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0 || out == NULL) {
        return EF_ERR_NULL_ARG;
    }

    capacity = db->hash_capacity;
    home = ef_hash_home(capacity, key_hash);

    for (i = home; i < home + capacity; ++i) {
        const struct ef_hash_entry *entry = db->hash_index + (i % capacity);

        if (ef_hash_entry_empty(entry)) {
            return EF_ERR_NOT_FOUND;
        }
        if (entry->key_hash == key_hash) {
            *out = *entry;
            return EF_OK;
        }
    }

    return EF_ERR_NOT_FOUND;
}

enum ef_err ef_index_put(struct ef_db *db, const char *key, uint64_t slot_id)
{
    uint64_t key_hash;
    uint64_t slot_offset;

    if (key == NULL) {
        return EF_ERR_NULL_ARG;
    }

    key_hash = ef_key_hash(key, strlen(key));
    slot_offset = ef_slot_to_offset(db, slot_id);
    if (slot_offset == 0) {
        return EF_ERR_SLOT_ID;
    }

    return ef_index_put_entry(db, key_hash, slot_offset);
}

enum ef_err ef_index_get(struct ef_db *db, const char *key, uint64_t *slot_id_out)
{
    struct ef_hash_entry found;
    enum ef_err err;

    if (slot_id_out == NULL) {
        return EF_ERR_NULL_ARG;
    }

    err = ef_index_find_entry(db, ef_key_hash(key, strlen(key)), &found);
    if (err != EF_OK) {
        return err;
    }

    return ef_offset_to_slot_id(db, found.slot_offset, slot_id_out);
}

static enum ef_err ef_index_require_write(struct ef_db *db)
{
    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (db->readonly) {
        return EF_ERR_READONLY;
    }
    return EF_OK;
}

#if EF_HAS_FILE_IO
static void ef_index_db_to_io(const struct ef_db *db, struct ef_io *io)
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
#endif

static enum ef_err ef_index_remove_by_hash(struct ef_db *db, uint64_t key_hash)
{
    uint32_t capacity;
    uint32_t home;
    uint32_t i;
    uint32_t pos;

    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return EF_ERR_NULL_ARG;
    }

    capacity = db->hash_capacity;
    home = ef_hash_home(capacity, key_hash);

    for (i = home; i < home + capacity; ++i) {
        struct ef_hash_entry *entry = db->hash_index + (i % capacity);

        if (ef_hash_entry_empty(entry)) {
            return EF_ERR_NOT_FOUND;
        }
        if (entry->key_hash == key_hash) {
            pos = i % capacity;
            for (;;) {
                uint32_t next_pos = (pos + 1U) % capacity;
                struct ef_hash_entry *next = db->hash_index + next_pos;

                if (ef_hash_entry_empty(next)) {
                    memset(entry, 0, sizeof(*entry));
                    return EF_OK;
                }

                if (ef_hash_probe_dist(capacity, ef_hash_home(capacity, next->key_hash),
                                       next_pos) == 0) {
                    memset(entry, 0, sizeof(*entry));
                    return EF_OK;
                }

                *entry = *next;
                entry = next;
                pos = next_pos;
            }
        }
    }

    return EF_ERR_NOT_FOUND;
}

enum ef_err ef_index_remove(struct ef_db *db, const char *key)
{
    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0 || key == NULL) {
        return EF_ERR_NULL_ARG;
    }

    return ef_index_remove_by_hash(db, ef_key_hash(key, strlen(key)));
}

enum ef_err ef_index_remove_by_slot(struct ef_db *db, uint64_t slot_id)
{
    uint32_t i;
    uint64_t slot_offset;

    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return EF_OK;
    }

    slot_offset = ef_slot_to_offset(db, slot_id);
    if (slot_offset == 0) {
        return EF_OK;
    }

    for (i = 0; i < db->hash_capacity; ++i) {
        if (db->hash_index[i].slot_offset == slot_offset) {
            enum ef_err err = ef_index_remove_by_hash(db, db->hash_index[i].key_hash);
            if (err == EF_ERR_NOT_FOUND) {
                return EF_OK;
            }
            return err;
        }
    }

    return EF_OK;
}

enum ef_err ef_index_rehash(struct ef_db *db, uint32_t new_capacity)
{
    struct ef_hash_entry *backup = NULL;
    uint32_t old_capacity;
    uint32_t n_entries = 0;
    uint32_t i;
    uint64_t old_slots_base;
    uint64_t new_slots_base;
    size_t new_size;
    size_t slot_bytes;
    enum ef_err err;
#if EF_HAS_FILE_IO
    struct ef_io io;
#endif

    if (db == NULL || db->sb == NULL) {
        return EF_ERR_NULL_ARG;
    }

    err = ef_index_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    old_capacity = db->hash_capacity;
    if (old_capacity == 0 || db->hash_index == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (new_capacity <= old_capacity || (new_capacity & (new_capacity - 1U)) != 0) {
        return EF_ERR_GROW;
    }

    for (i = 0; i < old_capacity; ++i) {
        if (!ef_hash_entry_empty(db->hash_index + i)) {
            ++n_entries;
        }
    }

    if (n_entries > 0) {
        backup = (struct ef_hash_entry *)malloc((size_t)n_entries * sizeof(*backup));
        if (backup == NULL) {
            return EF_ERR_OOM;
        }

        n_entries = 0;
        for (i = 0; i < old_capacity; ++i) {
            if (!ef_hash_entry_empty(db->hash_index + i)) {
                backup[n_entries++] = db->hash_index[i];
            }
        }
    }

    old_slots_base = db->slots_base;
    new_slots_base = ef_index_slots_base(new_capacity);
    new_size = ef_index_file_size(db->sb->max_slots, new_capacity);

    if (db->backend == EF_BACKEND_MEMORY && new_size > db->map_capacity) {
        free(backup);
        return EF_ERR_FILE_SIZE;
    }

#if EF_HAS_FILE_IO
    if (db->backend == EF_BACKEND_FILE) {
        ef_index_db_to_io(db, &io);
        err = ef_port_grow_file(&io, new_size);
        if (err != EF_OK) {
            free(backup);
            return err;
        }
        db->mmap_addr = io.map_addr;
        db->file_size = io.map_size;
        db->map_capacity = io.map_capacity;
        db->fd = io.fd;
#ifdef _WIN32
        db->map_handle = io.map_handle;
#endif
        db->sb = (struct ef_superblock *)db->mmap_addr;
    } else
#endif
    {
        db->file_size = new_size;
    }

    slot_bytes = (size_t)(db->sb->max_slots * sizeof(struct ef_slot));
    memmove((uint8_t *)db->mmap_addr + new_slots_base,
            (uint8_t *)db->mmap_addr + old_slots_base,
            slot_bytes);

    *ef_idx_hash_cap_ptr(db->sb) = new_capacity;
    ef_index_bind_layout(db);
    ef_index_fixup_all_offsets(db, old_slots_base, new_slots_base);

    for (i = 0; i < n_entries; ++i) {
        backup[i].slot_offset =
            ef_index_fixup_offset(backup[i].slot_offset, old_slots_base, new_slots_base);
    }

    memset(db->hash_index, 0, (size_t)new_capacity * sizeof(struct ef_hash_entry));

    for (i = 0; i < n_entries; ++i) {
        err = ef_index_put_entry(db, backup[i].key_hash, backup[i].slot_offset);
        if (err != EF_OK) {
            free(backup);
            return err;
        }
    }

    free(backup);
    ef_db_mark_meta_dirty(db);
    ef_db_refresh_slot_crcs(db);
    return EF_OK;
}

enum ef_err ef_index_clear(struct ef_db *db)
{
    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return EF_ERR_NULL_ARG;
    }

    memset(db->hash_index, 0, (size_t)db->hash_capacity * sizeof(struct ef_hash_entry));
    return EF_OK;
}
