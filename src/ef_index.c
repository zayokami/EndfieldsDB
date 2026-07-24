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

static int ef_hash_entry_empty(const struct ef_hash_entry *entry);

static int ef_index_should_shrink_snap(const struct ef_db *db, uint32_t entries,
                                       uint32_t capacity);

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
    uint32_t i;

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

    /* Re-derive live entry count from the on-disk table. */
    db->hash_entry_count = 0U;
    if (db->hash_index != NULL) {
        for (i = 0; i < db->hash_capacity; ++i) {
            if (!ef_hash_entry_empty(db->hash_index + i)) {
                ++db->hash_entry_count;
            }
        }
    }
}

static int ef_index_has_seqlock(const struct ef_db *db)
{
    /* The seqlock is purely a function of the on-disk schema, so this is
     * safe to call without holding any lock. We deliberately do NOT look
     * at db->hash_capacity here: that field is rewritten by
     * ef_index_bind_layout during rehash, and reading it from the
     * lock-acquire fast path would race with bind_layout. */
    return db != NULL && db->sb != NULL && db->sb->schema_version >= EF_SCHEMA_VERSION;
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

/* Insert or update key_hash -> slot_offset. On success, *added_out is set to 1 when a
 * brand-new key was inserted, 0 when an existing key was overwritten. */
static enum ef_err ef_index_put_entry(struct ef_db *db, uint64_t key_hash, uint64_t slot_offset,
                                      int *added_out)
{
    uint32_t capacity;
    uint32_t home;
    uint32_t i;
    struct ef_hash_entry incoming;
    struct ef_hash_entry outgoing;
    uint32_t full_count = 0;

    if (added_out != NULL) {
        *added_out = 0;
    }

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
            if (added_out != NULL) {
                *added_out = 1;
            }
            return EF_OK;
        }
        ++full_count;

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

    (void)full_count;
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

/* Returns 1 if inserting one more entry would meet or exceed the rehash threshold. */
static int ef_index_needs_grow(uint32_t count, uint32_t capacity)
{
    return (uint64_t)(count + 1U) * EF_INDEX_REHASH_LOAD_FACTOR_DEN >
           (uint64_t)capacity * EF_INDEX_REHASH_LOAD_FACTOR_NUM;
}

static enum ef_err ef_index_grow_for_insert(struct ef_db *db);

enum ef_err ef_index_put(struct ef_db *db, const char *key, uint64_t slot_id)
{
    uint64_t key_hash;
    uint64_t slot_offset;
    enum ef_err err;
    int added = 0;

    if (db == NULL || key == NULL) {
        return EF_ERR_NULL_ARG;
    }

    err = ef_index_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    key_hash = ef_key_hash(key, strlen(key));

    /* Acquire the index write lock for the whole grow + insert critical
     * section so that concurrent writers cannot race on db->hash_capacity,
     * db->hash_index, db->file_size, db->slots_base, or db->hash_entry_count.
     * ef_index_grow re-acquires the lock when ef_index_write_begin is held
     * by us, so we must read slot_offset (which reads db->slots_base) under
     * the lock to avoid racing with ef_index_bind_layout's update. */
    err = ef_index_write_begin(db);
    if (err != EF_OK) {
        return err;
    }

    slot_offset = ef_slot_to_offset(db, slot_id);
    if (slot_offset == 0) {
        ef_index_write_end(db);
        return EF_ERR_SLOT_ID;
    }

    /* Grow proactively before the table can fill up. A full Robin Hood table would
     * otherwise force a destructive displacement in ef_index_put_entry, so we never
     * let the load factor reach 100%. Only grow when the key is not already present
     * (an overwrite does not increase the entry count). */
    if (db->hash_index != NULL &&
        ef_index_needs_grow(db->hash_entry_count, db->hash_capacity)) {
        struct ef_hash_entry probe;
        if (ef_index_find_entry_unlocked(db, key_hash, &probe) != EF_OK) {
            /* ef_index_grow_for_insert calls ef_index_rehash, which itself
             * grabs the write lock. To avoid re-entering the spinlock we
             * release it for the duration of the rehash. The release is
             * safe because ef_index_grow_for_insert is the only path that
             * calls rehash and re-checks the load factor after. */
            ef_index_write_end(db);
            err = ef_index_grow_for_insert(db);
            if (err != EF_OK) {
                return err;
            }
            err = ef_index_write_begin(db);
            if (err != EF_OK) {
                return err;
            }
            slot_offset = ef_slot_to_offset(db, slot_id);
            if (slot_offset == 0) {
                ef_index_write_end(db);
                return EF_ERR_SLOT_ID;
            }
        }
    }

    err = ef_index_put_entry(db, key_hash, slot_offset, &added);
    if (err == EF_OK && added) {
        ++db->hash_entry_count;
    }
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
                    if (db->hash_entry_count > 0U) {
                        --db->hash_entry_count;
                    }
                    return EF_OK;
                }

                if (ef_hash_probe_dist(capacity, ef_hash_home(capacity, next_val.key_hash),
                                       next_pos) == 0) {
                    ef_hash_entry_clear_atomic(entry);
                    if (db->hash_entry_count > 0U) {
                        --db->hash_entry_count;
                    }
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

    if (db == NULL || key == NULL) {
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

    /* db->hash_index and db->hash_capacity are rewritten by
     * ef_index_bind_layout during rehash; read them under the lock. */
    if (db->hash_index == NULL || db->hash_capacity == 0) {
        ef_index_write_end(db);
        return EF_ERR_NULL_ARG;
    }

    err = ef_index_remove_by_hash(db, ef_key_hash(key, strlen(key)));
    if (err == EF_OK) {
        /* Snapshot the shrink inputs under the lock so we don't race with
         * concurrent writers updating db->hash_capacity or db->hash_entry_count.
         * The actual shrink (ef_index_shrink -> ef_index_rehash_to) runs after
         * we drop the lock, since it acquires the lock itself and would
         * deadlock if called while we still hold it. */
        uint32_t entries = db->hash_entry_count;
        uint32_t capacity = db->hash_capacity;
        ef_index_write_end(db);
        ef_db_mark_meta_dirty(db);
        if (ef_index_should_shrink_snap(db, entries, capacity)) {
            (void)ef_index_shrink(db, ef_index_pick_shrink_capacity(entries, capacity));
        }
    } else {
        ef_index_write_end(db);
    }
    return err;
}

enum ef_err ef_index_remove_by_slot(struct ef_db *db, uint64_t slot_id)
{
    uint32_t i;
    uint64_t slot_offset;
    enum ef_err err;

    if (db == NULL) {
        return EF_OK;
    }

    err = ef_index_require_write(db);
    if (err != EF_OK) {
        return err;
    }

    err = ef_index_write_begin(db);
    if (err != EF_OK) {
        return err;
    }

    if (db->hash_index == NULL || db->hash_capacity == 0) {
        ef_index_write_end(db);
        return EF_OK;
    }

    slot_offset = ef_slot_to_offset(db, slot_id);
    if (slot_offset == 0) {
        ef_index_write_end(db);
        return EF_OK;
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

    if (err == EF_OK) {
        /* Snapshot the shrink inputs under the lock so we don't race with
         * concurrent writers updating db->hash_capacity or db->hash_entry_count.
         * The actual shrink (ef_index_shrink -> ef_index_rehash_to) runs after
         * we drop the lock, since it acquires the lock itself and would
         * deadlock if called while we still hold it. */
        uint32_t entries = db->hash_entry_count;
        uint32_t capacity = db->hash_capacity;
        ef_index_write_end(db);
        ef_db_mark_meta_dirty(db);
        if (ef_index_should_shrink_snap(db, entries, capacity)) {
            (void)ef_index_shrink(db, ef_index_pick_shrink_capacity(entries, capacity));
        }
    } else {
        ef_index_write_end(db);
    }
    return err;
}

/* Internal rehash shared by ef_index_rehash (grow-only) and ef_index_shrink.
 * When allow_shrink is 0 new_capacity must be > old_capacity; when 1 it may be smaller.
 * On file backend shrink is rejected with EF_ERR_GROW because truncate+remap is not
 * implemented in ef_port_grow_file; memory backend can shrink freely. */
static enum ef_err ef_index_rehash_to(struct ef_db *db, uint32_t new_capacity, int allow_shrink)
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
    int shrinking;
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

    if (new_capacity < 1U || (new_capacity & (new_capacity - 1U)) != 0) {
        return EF_ERR_GROW;
    }
    if (new_capacity > 0xFFFFU) {
        return EF_ERR_GROW;
    }

    /* Acquire the write lock BEFORE reading db->hash_capacity and
     * db->hash_index: those fields are rewritten by ef_index_bind_layout
     * during rehash, and reading them outside the lock would race with a
     * concurrent rehash's bind_layout. We do the shrinking/backend checks
     * after the lock is held where they can safely observe db->hash_capacity. */
    err = ef_index_write_begin(db);
    if (err != EF_OK) {
        return err;
    }

    old_capacity = db->hash_capacity;
    if (old_capacity == 0 || db->hash_index == NULL) {
        ef_index_write_end(db);
        return EF_ERR_NULL_ARG;
    }

    /* allow_shrink = 1  -> only shrinking is allowed (new_capacity < old_capacity).
     * allow_shrink = 0  -> growing or no-op (any new_capacity is OK as long as
     *                      it's not a shrink, which would be a programming bug).
     * The any-growth-direction tolerance handles the race where
     * ef_index_grow_for_insert's snapshot of old_capacity becomes stale
     * because a concurrent rehash already grew the table past the target
     * we computed; re-hashing to a smaller-than-current size is a harmless
     * no-op because the live entries already fit in the larger table. */
    if (allow_shrink != 0) {
        if (new_capacity >= old_capacity) {
            ef_index_write_end(db);
            return EF_ERR_GROW;
        }
        shrinking = 1;
    } else {
        if (new_capacity == 0U || (new_capacity & (new_capacity - 1U)) != 0) {
            ef_index_write_end(db);
            return EF_ERR_GROW;
        }
        if (new_capacity < old_capacity) {
            /* Concurrent rehash already grew past us; nothing to do. */
            ef_index_write_end(db);
            return EF_OK;
        }
        shrinking = 0;
    }
    if (new_capacity == old_capacity) {
        /* No-op: nothing to do. */
        ef_index_write_end(db);
        return EF_OK;
    }
    if (shrinking && db->backend == EF_BACKEND_FILE) {
        ef_index_write_end(db);
        return EF_ERR_GROW;
    }
    if (shrinking && db->hash_entry_count != 0U) {
        /* Reject shrinking below what the live entries can fit at 3/4 load.
         * Done under the lock so we observe db->hash_entry_count
         * consistently with bind_layout's updates. */
        const uint64_t lhs = (uint64_t)db->hash_entry_count *
                             EF_INDEX_REHASH_LOAD_FACTOR_DEN;
        const uint64_t rhs = (uint64_t)new_capacity *
                             EF_INDEX_REHASH_LOAD_FACTOR_NUM;
        if (lhs >= rhs) {
            ef_index_write_end(db);
            return EF_ERR_GROW;
        }
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
        err = ef_index_put_entry(db, backup[i].key_hash, backup[i].slot_offset, NULL);
        if (err != EF_OK) {
            free(backup);
            ef_index_write_end(db);
            return err;
        }
    }
    db->hash_entry_count = n_entries;

    free(backup);
    ef_db_mark_meta_dirty(db);
    ef_db_refresh_slot_crcs(db);
    ef_index_write_end(db);
    return EF_OK;
}

enum ef_err ef_index_rehash(struct ef_db *db, uint32_t new_capacity)
{
    return ef_index_rehash_to(db, new_capacity, 0);
}

enum ef_err ef_index_clear(struct ef_db *db)
{
    enum ef_err err;

    if (db == NULL) {
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

    /* db->hash_index and db->hash_capacity are rewritten by
     * ef_index_bind_layout during rehash; read them under the lock. */
    if (db->hash_index == NULL || db->hash_capacity == 0) {
        ef_index_write_end(db);
        return EF_ERR_NULL_ARG;
    }

    memset(db->hash_index, 0, (size_t)db->hash_capacity * sizeof(struct ef_hash_entry));
    db->hash_entry_count = 0U;
    ef_db_mark_meta_dirty(db);
    /* Snapshot the shrink inputs under the lock so we don't race with
     * concurrent writers updating db->hash_capacity. The actual shrink
     * (ef_index_shrink -> ef_index_rehash_to) runs after we drop the lock,
     * since it acquires the lock itself and would deadlock if called while
     * we still hold it. */
    if (db->hash_capacity > EF_DEFAULT_HASH_MIN) {
        uint32_t capacity = db->hash_capacity;
        ef_index_write_end(db);
        (void)ef_index_shrink(db, ef_index_pick_shrink_capacity(0U, capacity));
    } else {
        ef_index_write_end(db);
    }
    return EF_OK;
}

uint32_t ef_index_capacity(const struct ef_db *db)
{
    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return 0U;
    }
    return db->hash_capacity;
}

uint32_t ef_index_count_entries(const struct ef_db *db)
{
    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return 0U;
    }
    return db->hash_entry_count;
}

/* Choose the smallest power-of-two capacity that keeps the load factor below the
 * rehash threshold for the desired number of entries. Returns 0 if it cannot fit
 * within EF_INDEX_MAX_CAPACITY. */
static uint32_t ef_index_pick_capacity(uint32_t entries, uint32_t min_capacity)
{
    uint32_t cap = min_capacity;
    const uint64_t max_entries =
        (uint64_t)EF_INDEX_MAX_CAPACITY * EF_INDEX_REHASH_LOAD_FACTOR_NUM /
        EF_INDEX_REHASH_LOAD_FACTOR_DEN;

    if (cap < EF_DEFAULT_HASH_MIN) {
        cap = EF_DEFAULT_HASH_MIN;
    }
    if ((uint64_t)entries > max_entries) {
        return 0U;
    }

    while ((uint64_t)cap * EF_INDEX_REHASH_LOAD_FACTOR_NUM /
               EF_INDEX_REHASH_LOAD_FACTOR_DEN <
           (uint64_t)entries) {
        if (cap > (EF_INDEX_MAX_CAPACITY >> 1U)) {
            return 0U;
        }
        cap <<= 1U;
    }
    return cap;
}

/* Grow the index so it can absorb one more entry without exceeding the load-factor
 * threshold. This function takes the index write lock for the preliminary reads of
 * db->hash_capacity, db->hash_index, and db->hash_entry_count (which are concurrently
 * rewritten by ef_index_bind_layout during rehash), then releases it before calling
 * ef_index_rehash, which re-acquires the lock for the actual rehash critical
 * section. ef_index_rehash expects the lock to be free on entry. */
static enum ef_err ef_index_grow_for_insert(struct ef_db *db)
{
    uint32_t cap;
    uint32_t target;
    enum ef_err err;

    if (db == NULL) {
        return EF_ERR_NULL_ARG;
    }

    /* Read db->hash_capacity, db->hash_index, and db->hash_entry_count under
     * the lock so we cannot race with ef_index_bind_layout's writes. */
    err = ef_index_write_begin(db);
    if (err != EF_OK) {
        return err;
    }
    if (db->hash_index == NULL || db->hash_capacity == 0) {
        ef_index_write_end(db);
        return EF_ERR_NULL_ARG;
    }
    cap = db->hash_capacity;
    target = ef_index_pick_capacity(db->hash_entry_count + 1U, cap << 1U);
    ef_index_write_end(db);

    if (target == 0U || target <= cap) {
        return EF_ERR_INDEX_FULL;
    }

    return ef_index_rehash(db, target);
}

/* Snapshot variant that accepts the entries/capacity values to check. Used by
 * callers that need to make the shrink decision after releasing the index write
 * lock (since ef_index_shrink itself re-acquires the lock via rehash). */
static int ef_index_should_shrink_snap(const struct ef_db *db, uint32_t entries,
                                       uint32_t capacity)
{
    if (db == NULL || db->hash_index == NULL || capacity == 0) {
        return 0;
    }
    if (entries == 0U) {
        return 1;
    }
    return (uint64_t)entries * EF_INDEX_SHRINK_LOAD_FACTOR_DEN <
           (uint64_t)capacity * EF_INDEX_SHRINK_LOAD_FACTOR_NUM;
}

uint32_t ef_index_pick_shrink_capacity(uint32_t entries, uint32_t current_capacity)
{
    uint32_t cap;
    uint32_t min_capacity;
    uint32_t min_required;

    if (current_capacity == 0U || (current_capacity & (current_capacity - 1U)) != 0U) {
        return 0U;
    }
    if (current_capacity > 0xFFFFU) {
        return 0U;
    }

    /* The shrunken capacity must keep the rehash load factor satisfied
     * (entries <= capacity * 3/4), otherwise Robin Hood reinsertion will
     * fail with EF_ERR_INDEX_FULL. */
    if (entries > 0U) {
        min_required = entries * EF_INDEX_REHASH_LOAD_FACTOR_DEN /
                       EF_INDEX_REHASH_LOAD_FACTOR_NUM;
        if (entries * EF_INDEX_REHASH_LOAD_FACTOR_DEN >
            min_required * EF_INDEX_REHASH_LOAD_FACTOR_NUM) {
            ++min_required;
        }
        /* Round up to next power of two. */
        cap = EF_DEFAULT_HASH_MIN;
        while (cap < min_required) {
            cap <<= 1U;
        }
        if (cap >= current_capacity) {
            return 0U;
        }
    } else {
        cap = EF_DEFAULT_HASH_MIN;
        if (cap >= current_capacity) {
            return 0U;
        }
    }

    /* Only shrink if we are below the shrink threshold. */
    if (entries != 0U &&
        (uint64_t)entries * EF_INDEX_SHRINK_LOAD_FACTOR_DEN >=
            (uint64_t)current_capacity * EF_INDEX_SHRINK_LOAD_FACTOR_NUM) {
        return 0U;
    }

    min_capacity = EF_DEFAULT_HASH_MIN;
    while (cap > min_capacity) {
        if (entries == 0U) {
            return cap;
        }
        if ((uint64_t)cap * EF_INDEX_SHRINK_LOAD_FACTOR_DEN >=
            (uint64_t)entries * EF_INDEX_SHRINK_LOAD_FACTOR_NUM) {
            return cap;
        }
        cap >>= 1U;
    }
    return min_capacity;
}

enum ef_err ef_index_shrink(struct ef_db *db, uint32_t new_capacity)
{
    /* All shrinking checks against db->hash_capacity / db->hash_entry_count
     * (zero capacity, new_capacity < old_capacity, load-factor fit,
     * file-backend rejection) are performed inside ef_index_rehash_to under
     * the index write lock. We deliberately do NOT read db->hash_capacity /
     * db->hash_entry_count here: those fields are rewritten by
     * ef_index_bind_layout during rehash, and reading them outside the lock
     * would race with a concurrent rehash. The new_capacity == 0 guard stays
     * here because it's a pure constant. */
    if (new_capacity == 0U) {
        return EF_ERR_GROW;
    }
    return ef_index_rehash_to(db, new_capacity, 1);
}

static enum ef_err ef_index_iterate_impl(const struct ef_db *db, ef_index_iter_fn cb, void *user,
                                         int stop_on_writer)
{
    uint32_t capacity;
    uint32_t i;
    uint32_t prev_seq = 0U;
    int watching_seq = 0;

    if (db == NULL || cb == NULL) {
        return EF_ERR_NULL_ARG;
    }
    if (db->hash_index == NULL || db->hash_capacity == 0) {
        return EF_OK;
    }

    capacity = db->hash_capacity;
    if (stop_on_writer && ef_index_has_seqlock(db)) {
        prev_seq = ef_sb_index_seq_load(db->sb);
        watching_seq = 1;
    }

    for (i = 0; i < capacity; ++i) {
        struct ef_hash_entry cur;
        int rc;

        ef_hash_entry_load_atomic(&db->hash_index[i], &cur);
        if (ef_hash_entry_empty_atomic(&db->hash_index[i])) {
            continue;
        }

        if (stop_on_writer && watching_seq) {
            uint32_t now_seq = ef_sb_index_seq_load(db->sb);
            if (now_seq != prev_seq) {
                return EF_ERR_INDEX_BUSY;
            }
        }

        rc = cb(user, cur.key_hash, cur.slot_offset);
        if (rc == 1) {
            return EF_OK;
        }
        if (rc != 0) {
            return EF_ERR_USER_ABORT;
        }
    }

    return EF_OK;
}

enum ef_err ef_index_iterate(struct ef_db *db, ef_index_iter_fn cb, void *user)
{
    return ef_index_iterate_impl(db, cb, user, 0);
}

enum ef_err ef_index_iterate_until(struct ef_db *db, ef_index_iter_fn cb, void *user)
{
    return ef_index_iterate_impl(db, cb, user, 1);
}
