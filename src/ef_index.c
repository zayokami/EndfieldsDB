#include "ef_index.h"

#include <string.h>

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

enum ef_err ef_index_remove(struct ef_db *db, const char *key)
{
    uint32_t capacity;
    uint32_t home;
    uint32_t i;
    uint32_t pos;
    uint64_t key_hash;

    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0 || key == NULL) {
        return EF_ERR_NULL_ARG;
    }

    key_hash = ef_key_hash(key, strlen(key));
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

enum ef_err ef_index_clear(struct ef_db *db)
{
    if (db == NULL || db->hash_index == NULL || db->hash_capacity == 0) {
        return EF_ERR_NULL_ARG;
    }

    memset(db->hash_index, 0, (size_t)db->hash_capacity * sizeof(struct ef_hash_entry));
    return EF_OK;
}
