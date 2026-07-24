#ifndef EF_INDEX_H
#define EF_INDEX_H

#include "endfields.h"

#define EF_HASH_ENTRY_SIZE 16U

/* Robin Hood load factor above which ef_index_put triggers an automatic rehash to the
 * next power-of-two capacity (bounded by EF_INDEX_MAX_CAPACITY). */
#define EF_INDEX_REHASH_LOAD_FACTOR_NUM 3U
#define EF_INDEX_REHASH_LOAD_FACTOR_DEN 4U
/* When the entry count drops below this load factor, an automatic shrink is triggered
 * to the next power-of-two capacity that fits the live entries. */
#define EF_INDEX_SHRINK_LOAD_FACTOR_NUM 1U
#define EF_INDEX_SHRINK_LOAD_FACTOR_DEN 8U
#define EF_INDEX_MAX_CAPACITY 0xFFFFU

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
enum ef_err ef_index_shrink(struct ef_db *db, uint32_t new_capacity);
uint32_t ef_index_pick_shrink_capacity(uint32_t entries, uint32_t current_capacity);
enum ef_err ef_index_clear(struct ef_db *db);

/* Callback for ef_index_iterate*; return 0 to continue, 1 to stop, anything else
 * to abort with EF_ERR_USER_ABORT. */
typedef int (*ef_index_iter_fn)(void *user, uint64_t key_hash, uint64_t slot_id);

enum ef_err ef_index_iterate(struct ef_db *db, ef_index_iter_fn cb, void *user);
enum ef_err ef_index_iterate_until(struct ef_db *db, ef_index_iter_fn cb, void *user);

/* Current index capacity (0 if index disabled). */
uint32_t ef_index_capacity(const struct ef_db *db);
/* Number of occupied entries (linear scan; for load-factor decisions). */
uint32_t ef_index_count_entries(const struct ef_db *db);

#endif
