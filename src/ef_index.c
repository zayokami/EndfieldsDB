#include "ef_index.h"
#include "ef_port.h"
#include "ef_atomic_unaligned.h"
#include "ef_sb_layout.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sched.h>
#endif

#define EF_SB_SIZE 64U

static int ef_hash_entry_empty_atomic(const struct ef_hash_entry *entry)
{
    return ef_atomic_load_u64((const void *)&entry->slot_offset) == 0ULL;
}

static void ef_hash_entry_load_atomic(const struct ef_hash_entry *entry, struct ef_hash_entry *out)
{
    out->slot_offset = ef_atomic_load_u64((const void *)&entry->slot_offset);
    out->key_hash = ef_atomic_load_u64((const void *)&entry->key_hash);
}

static void ef_hash_entry_store_atomic(struct ef_hash_entry *entry, uint64_t key_hash,
                                       uint64_t slot_offset)
{
    ef_atomic_store_u64(&entry->key_hash, key_hash);
    ef_atomic_store_u64(&entry->slot_offset, slot_offset);
}

static void ef_hash_entry_clear_atomic(struct ef_hash_entry *entry)
{
    ef_atomic_store_u64(&entry->key_hash, 0ULL);
    ef_atomic_store_u64(&entry->slot_offset, 0ULL);
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

    db->hash_capacity = ef_sb_hash_capacity_load(db->sb);
    if (db->hash_capacity > 0) {
        db->hash_index = (struct ef_hash_entry *)((uint8_t *)db->mmap_addr + EF_SB_SIZE);
        db->slots_base = ef_index_slots_base(db->hash_capacity);
    } else {
        db->hash_index = NULL;
        db->slots_base = EF_SB_SIZE;
    }
    db->slots = (struct ef_slot *)((uint8_t *)db->mmap_addr + db->slots_base);
}

static int ef_index_has_seqlock(const struct ef_db *db)
{
    return db != NULL && db->sb != NULL && db->sb->schema_version >= EF_SCHEMA_VERSION &&
           db->hash_capacity > 0U;
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
    {
        uint64_t queue_head = ef_atomic_load_u64((const void *)ef_idx_queue_head_ptr(db->sb));
        uint64_t queue_tail = ef_atomic_load_u64((const void *)ef_idx_queue_tail_ptr(db->sb));

        ef_atomic_store_u64(ef_idx_queue_head_ptr(db->sb),
                            ef_index_fixup_offset(queue_head, old_base, new_base));
        ef_atomic_store_u64(ef_idx_queue_tail_ptr(db->sb),
                            ef_index_fixup_offset(queue_tail, old_base, new_base));
    }

    for (i = 0; i < db->sb->max_slots; ++i) {
        struct ef_slot *slot = db->slots + i;
        uint64_t next_off = ef_atomic_load_u64((const unsigned char *)slot +
                                               offsetof(struct ef_slot, next_offset));

        if (next_off >= old_base) {
            ef_atomic_store_u64((unsigned char *)slot + offsetof(struct ef_slot, next_offset),
                                next_off + (new_base - old_base));
        }
    }
}

static void ef_index_read_yield(uint32_t attempt)
{
    if (attempt < 32U) {
        return;
    }
#ifdef _WIN32
    if (attempt < 512U) {
        YieldProcessor();
        return;
    }
    Sleep(0);
#else
    (void)sched_yield();
#endif
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

static enum ef_err ef_index_write_begin(struct ef_db *db)
{
    enum ef_err err;

    if (!ef_index_has_seqlock(db)) {
        return EF_OK;
    }

    err = ef_sb_index_write_lock_acquire(db->sb);
    if (err != EF_OK) {
        return err;
    }

    ef_sb_index_write_seq_begin(db->sb);
    return EF_OK;
}

static void ef_index_write_end(struct ef_db *db)
{
    if (!ef_index_has_seqlock(db)) {
        return;
    }

    ef_sb_index_write_seq_end(db->sb);
    ef_sb_index_write_lock_release(db->sb);
}

static int ef_hash_entry_empty(const struct ef_hash_entry *entry)
{
    return ef_hash_entry_empty_atomic(entry);
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
        struct ef_hash_entry cur;

        ef_hash_entry_load_atomic(entry, &cur);

        if (ef_hash_entry_empty_atomic(entry)) {
            ef_hash_entry_store_atomic(entry, incoming.key_hash, incoming.slot_offset);
            return EF_OK;
        }

        if (cur.key_hash == key_hash) {
            ef_hash_entry_store_atomic(entry, key_hash, slot_offset);
            return EF_OK;
        }

        if (ef_hash_probe_dist(capacity, home, i % capacity) >
            ef_hash_probe_dist(capacity, ef_hash_home(capacity, cur.key_hash),
                               (uint32_t)(entry - db->hash_index))) {
            outgoing = cur;
            ef_hash_entry_store_atomic(entry, incoming.key_hash, incoming.slot_offset);
            incoming = outgoing;
            home = ef_hash_home(capacity, incoming.key_hash);
        }
    }

    return EF_ERR_INDEX_FULL;
}

static enum ef_err ef_index_find_entry_unlocked(const struct ef_db *db, uint64_t key_hash,
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
        struct ef_hash_entry cur;

        ef_hash_entry_load_atomic(entry, &cur);

        if (ef_hash_entry_empty_atomic(entry)) {
            return EF_ERR_NOT_FOUND;
        }
        if (cur.key_hash == key_hash) {
            *out = cur;
            return EF_OK;
        }
    }

    return EF_ERR_NOT_FOUND;
}

static enum ef_err ef_index_find_entry(const struct ef_db *db, uint64_t key_hash,
                                       struct ef_hash_entry *out)
{
    uint32_t attempt;

    if (!ef_index_has_seqlock(db)) {
        return ef_index_find_entry_unlocked(db, key_hash, out);
    }

    for (attempt = 0; attempt < EF_SB_INDEX_SEQ_READ_MAX; ++attempt) {
        uint32_t seq1;
        enum ef_err err;

        seq1 = ef_sb_index_seq_load(db->sb);
        if ((seq1 & 1U) != 0U) {
            ef_index_read_yield(attempt);
            continue;
        }

        err = ef_index_find_entry_unlocked(db, key_hash, out);
        if (err == EF_ERR_NOT_FOUND && ef_sb_index_seq_read_stable(db->sb, seq1)) {
            return EF_ERR_NOT_FOUND;
        }
        if (err == EF_OK && ef_sb_index_seq_read_stable(db->sb, seq1)) {
            return EF_OK;
        }

        ef_index_read_yield(attempt);
    }

    return EF_ERR_INDEX_BUSY;
}

enum ef_err ef_index_put(struct ef_db *db, const char *key, uint64_t slot_id)
{
    uint64_t key_hash;
    uint64_t slot_offset;
    enum ef_err err;

    if (db == NULL || key == NULL) {
        return EF_ERR_NULL_ARG;
    }

    err = ef_index_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    key_hash = ef_key_hash(key, strlen(key));
    slot_offset = ef_slot_to_offset(db, slot_id);
    if (slot_offset == 0) {
        return EF_ERR_SLOT_ID;
    }

    err = ef_index_write_begin(db);
    if (err != EF_OK) {
        return err;
    }

    err = ef_index_put_entry(db, key_hash, slot_offset);
    ef_index_write_end(db);
    if (err == EF_OK) {
        ef_db_mark_meta_dirty(db);
    }
    return err;
}

enum ef_err ef_index_get(struct ef_db *db, const char *key, uint64_t *slot_id_out)
{
    struct ef_hash_entry found;
    enum ef_err err;

    if (key == NULL || slot_id_out == NULL) {
        return EF_ERR_NULL_ARG;
    }

    err = ef_index_find_entry(db, ef_key_hash(key, strlen(key)), &found);
    if (err != EF_OK) {
        return err;
    }

    return ef_offset_to_slot_id(db, found.slot_offset, slot_id_out);
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
        struct ef_hash_entry cur;

        ef_hash_entry_load_atomic(entry, &cur);

        if (ef_hash_entry_empty_atomic(entry)) {
            return EF_ERR_NOT_FOUND;
        }
        if (cur.key_hash == key_hash) {
            pos = i % capacity;
            for (;;) {
                uint32_t next_pos = (pos + 1U) % capacity;
                struct ef_hash_entry *next = db->hash_index + next_pos;
                struct ef_hash_entry next_val;

                ef_hash_entry_load_atomic(next, &next_val);

                if (ef_hash_entry_empty_atomic(next)) {
                    ef_hash_entry_clear_atomic(entry);
                    return EF_OK;
                }

                if (ef_hash_probe_dist(capacity, ef_hash_home(capacity, next_val.key_hash),
                                       next_pos) == 0) {
                    ef_hash_entry_clear_atomic(entry);
                    return EF_OK;
                }

                ef_hash_entry_store_atomic(entry, next_val.key_hash, next_val.slot_offset);
                entry = next;
                pos = next_pos;
            }
        }
    }

    return EF_ERR_NOT_FOUND;
}

enum ef_err ef_index_remove(struct ef_db *db, const char *key)
{
    enum ef_err err;

    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0 || key == NULL) {
        return EF_ERR_NULL_ARG;
    }

    err = ef_index_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    err = ef_index_write_begin(db);
    if (err != EF_OK) {
        return err;
    }

    err = ef_index_remove_by_hash(db, ef_key_hash(key, strlen(key)));
    ef_index_write_end(db);
    if (err == EF_OK) {
        ef_db_mark_meta_dirty(db);
    }
    return err;
}

enum ef_err ef_index_remove_by_slot(struct ef_db *db, uint64_t slot_id)
{
    uint32_t i;
    uint64_t slot_offset;
    enum ef_err err;

    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return EF_OK;
    }

    err = ef_index_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    slot_offset = ef_slot_to_offset(db, slot_id);
    if (slot_offset == 0) {
        return EF_OK;
    }

    err = ef_index_write_begin(db);
    if (err != EF_OK) {
        return err;
    }

    for (i = 0; i < db->hash_capacity; ++i) {
        struct ef_hash_entry cur;

        ef_hash_entry_load_atomic(&db->hash_index[i], &cur);
        if (cur.slot_offset == slot_offset) {
            err = ef_index_remove_by_hash(db, cur.key_hash);
            if (err == EF_ERR_NOT_FOUND) {
                err = EF_OK;
            }
            break;
        }
    }

    ef_index_write_end(db);
    if (err == EF_OK) {
        ef_db_mark_meta_dirty(db);
    }
    return err;
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
    if (new_capacity > 0xFFFFU) {
        return EF_ERR_GROW;
    }

    err = ef_index_write_begin(db);
    if (err != EF_OK) {
        return err;
    }

    for (i = 0; i < old_capacity; ++i) {
        if (!ef_hash_entry_empty(db->hash_index + i)) {
            ++n_entries;
        }
    }

    if (n_entries > 0) {
        backup = (struct ef_hash_entry *)malloc((size_t)n_entries * sizeof(*backup));
        if (backup == NULL) {
            ef_index_write_end(db);
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
        ef_index_write_end(db);
        return EF_ERR_FILE_SIZE;
    }

#if EF_HAS_FILE_IO
    if (db->backend == EF_BACKEND_FILE) {
        ef_index_db_to_io(db, &io);
        err = ef_port_grow_file(&io, new_size);
        if (err != EF_OK) {
            free(backup);
            ef_index_write_end(db);
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

    ef_sb_hash_capacity_store(db->sb, new_capacity);
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
            ef_index_write_end(db);
            return err;
        }
    }

    free(backup);
    ef_db_mark_meta_dirty(db);
    ef_db_refresh_slot_crcs(db);
    ef_index_write_end(db);
    return EF_OK;
}

enum ef_err ef_index_clear(struct ef_db *db)
{
    enum ef_err err;

    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return EF_ERR_NULL_ARG;
    }

    err = ef_index_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    err = ef_index_write_begin(db);
    if (err != EF_OK) {
        return err;
    }

    memset(db->hash_index, 0, (size_t)db->hash_capacity * sizeof(struct ef_hash_entry));
    ef_db_mark_meta_dirty(db);
    ef_index_write_end(db);
    return EF_OK;
}
