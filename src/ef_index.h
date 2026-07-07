#ifndef EF_INDEX_H
#define EF_INDEX_H

#include "endfields.h"

#define EF_HASH_ENTRY_SIZE 16U

/* Robin Hood load factor above which ef_index_put triggers an automatic rehash
 * to the next power-of-two capacity. */
#define EF_INDEX_REHASH_LOAD_FACTOR_NUM 3U
#define EF_INDEX_REHASH_LOAD_FACTOR_DEN 4U
/* The v4 superblock stores capacity in u16, while capacity must be a power of two. */
#define EF_INDEX_MAX_CAPACITY 0x8000U

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
enum ef_err ef_index_remove_by_slot(struct ef_db *db, uint64_t slot_id);
enum ef_err ef_index_rehash(struct ef_db *db, uint32_t new_capacity);
enum ef_err ef_index_clear(struct ef_db *db);

/* Current index capacity (0 if index disabled). */
uint32_t ef_index_capacity(const struct ef_db *db);
/* Number of occupied entries (linear scan; for load-factor decisions). */
uint32_t ef_index_count_entries(const struct ef_db *db);

#endif
