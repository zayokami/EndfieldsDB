#ifndef EF_INDEX_H
#define EF_INDEX_H

#include "endfields.h"

#define EF_HASH_ENTRY_SIZE 16U

#pragma pack(push, 1)
struct ef_hash_entry {
    uint64_t key_hash;
    uint64_t slot_offset;
};
#pragma pack(pop)

_Static_assert(sizeof(struct ef_hash_entry) == 16, "ef_hash_entry must be 16 bytes");

uint64_t ef_key_hash(const char *key, size_t key_len);

enum ef_err ef_index_put(struct ef_db *db, const char *key, uint64_t slot_id);
enum ef_err ef_index_get(struct ef_db *db, const char *key, uint64_t *slot_id_out);
enum ef_err ef_index_remove(struct ef_db *db, const char *key);
enum ef_err ef_index_clear(struct ef_db *db);

#endif
