#ifndef ENDFIELDS_H
#define ENDFIELDS_H

#include <stdint.h>
#include <stddef.h>

#include "ef_config.h"

#define EF_MAGIC_0 'E'
#define EF_MAGIC_1 'N'
#define EF_MAGIC_2 'D'
#define EF_MAGIC_3 'F'

#define EF_SLOT_SIZE 64U
#define EF_SLOT_SHIFT 6U
#define EF_SLOT_MASK  63U
#define EF_PAYLOAD_SIZE 48U
#define EF_PAYLOAD_SIZE_LEGACY 52U
#define EF_SUPERBLOCK_SIZE 64U

#define EF_OP_GET_SLOT      0x01U
#define EF_OP_CHASE         0x02U
#define EF_OP_GET_FIELD     0x03U
#define EF_OP_WRITE_PAYLOAD 0x04U
#define EF_OP_SET_NEXT      0x05U
#define EF_OP_SET_STATUS    0x06U
#define EF_OP_WRITE_FIELD   0x07U
#define EF_OP_ALLOC         0x08U
#define EF_OP_FREE          0x09U
#define EF_OP_CHASE_N       0x0AU

#define EF_STATUS_FREE 0U
#define EF_STATUS_USED 1U
#define EF_STATUS_OVERFLOW 2U
#define EF_STATUS_QUEUED 3U
#define EF_STATUS_QUEUE_DUMMY 4U
#define EF_STATUS_QUEUE_LINK 5U
#define EF_STATUS_QUEUE_DEQ 6U

#define EF_BLOB_LEN_SIZE 4U
#define EF_BLOB_MAGIC 0x424F4C42U /* 'BLOB' little-endian */
#define EF_BLOB_HDR_SIZE 8U

#define EF_FLAG_NONE      0U
#define EF_FLAG_SB_CRC    0x01U
#define EF_FLAG_SLOT_CRC  0x02U

#pragma pack(push, 1)

struct ef_slot {
    uint32_t status;
    uint32_t header_crc;
    char payload[48];
    uint64_t next_offset;
};

struct ef_superblock {
    char magic[4];
    uint32_t slot_size;
    uint64_t max_slots;
    uint64_t free_list_head;
    uint32_t schema_version;
    uint32_t flags;
    uint32_t free_count;
    uint8_t reserved[28];
};

struct ef_cmd {
    uint8_t opcode;
    uint64_t param;
    uint8_t field_offset;
};

#pragma pack(pop)

_Static_assert(sizeof(struct ef_slot) == 64, "struct ef_slot size must be 64 bytes");
_Static_assert(sizeof(struct ef_superblock) == 64, "struct ef_superblock size must be 64 bytes");
_Static_assert(sizeof(struct ef_cmd) == 10, "struct ef_cmd size must be 10 bytes");

enum ef_err {
    EF_OK = 0,
    EF_ERR_NULL_ARG,
    EF_ERR_IO,
    EF_ERR_MMAP,
    EF_ERR_OOM,
    EF_ERR_BAD_MAGIC,
    EF_ERR_BAD_VERSION,
    EF_ERR_BAD_CHECKSUM,
    EF_ERR_BAD_SLOT_SIZE,
    EF_ERR_FILE_SIZE,
    EF_ERR_SLOT_ID,
    EF_ERR_OFFSET,
    EF_ERR_PAYLOAD_LEN,
    EF_ERR_OPCODE,
    EF_ERR_SLOT_FREE,
    EF_ERR_SLOT_BUSY,
    EF_ERR_SLOT_FULL,
    EF_ERR_NOT_FOUND,
    EF_ERR_CHASE_DEPTH,
    EF_ERR_CHASE_CYCLE,
    EF_ERR_READONLY,
    EF_ERR_GROW,
    EF_ERR_QUEUE_EMPTY,
    EF_ERR_QUEUE_BUSY,
    EF_ERR_INDEX_FULL
};

enum ef_backend {
    EF_BACKEND_NONE = 0,
    EF_BACKEND_FILE = 1,
    EF_BACKEND_MEMORY = 2
};

struct ef_hash_entry;

struct ef_db {
    int fd;
    void *mmap_addr;
    size_t file_size;
    size_t map_capacity;
    struct ef_superblock *sb;
    struct ef_slot *slots;
    enum ef_err last_err;
    enum ef_backend backend;
    uint64_t slots_base;
    uint32_t hash_capacity;
    struct ef_hash_entry *hash_index;
    int readonly;
    uint8_t sb_meta_dirty;
#ifdef _WIN32
    void *map_handle;
#endif
};

struct ef_db *ef_open(const char *filepath, uint64_t initial_slots);
enum ef_err ef_open_ex(const char *filepath, uint64_t initial_slots, struct ef_db **db_out);
struct ef_db *ef_open_readonly(const char *filepath);
enum ef_err ef_open_readonly_ex(const char *filepath, struct ef_db **db_out);
enum ef_err ef_open_memory(void *buffer, size_t buffer_size, uint64_t max_slots, int init_new, struct ef_db **db_out);
enum ef_err ef_open_ex_hash(const char *filepath, uint64_t initial_slots, uint32_t hash_capacity,
                            struct ef_db **db_out);
enum ef_err ef_open_memory_hash(void *buffer, size_t buffer_size, uint64_t max_slots,
                                uint32_t hash_capacity, int init_new, struct ef_db **db_out);
void ef_close(struct ef_db *db);

int ef_is_readonly(const struct ef_db *db);
enum ef_err ef_grow(struct ef_db *db, uint64_t new_max_slots);

int ef_needs_upgrade(const struct ef_db *db);
enum ef_err ef_upgrade(struct ef_db *db);

enum ef_err ef_last_error(const struct ef_db *db);
const char *ef_strerror(enum ef_err err);
const char *ef_platform_name(void);

enum ef_sync_mode {
    EF_SYNC_FULL = 0,
    EF_SYNC_ASYNC = 1
};

enum ef_err ef_sync(struct ef_db *db);
enum ef_err ef_sync_ex(struct ef_db *db, enum ef_sync_mode mode);

uint64_t ef_slot_to_offset(const struct ef_db *db, uint64_t slot_id);
enum ef_err ef_offset_to_slot_id(const struct ef_db *db, uint64_t offset, uint64_t *slot_id_out);

size_t ef_payload_capacity(const struct ef_db *db);
void *ef_slot_payload_ptr(const struct ef_db *db, struct ef_slot *slot);

struct ef_slot *ef_get_slot(struct ef_db *db, uint64_t slot_id);
void *ef_offset_to_ptr(struct ef_db *db, uint64_t offset);
struct ef_slot *ef_chase(struct ef_db *db, struct ef_slot *current_slot);
struct ef_slot *ef_chase_n(struct ef_db *db, uint64_t start_offset, uint32_t hops, uint32_t *hops_done_out);
void *ef_get_field_ptr(struct ef_slot *slot, uint8_t field_offset);

enum ef_err ef_set_status(struct ef_db *db, uint64_t slot_id, uint32_t status);
enum ef_err ef_set_next_offset(struct ef_db *db, uint64_t slot_id, uint64_t next_offset);
enum ef_err ef_write_payload(struct ef_db *db, uint64_t slot_id, const void *data, uint8_t len);
enum ef_err ef_write_field(struct ef_db *db, uint64_t slot_id, uint8_t field_offset, uint8_t value);

enum ef_err ef_alloc_slot(struct ef_db *db, uint64_t *slot_id_out);
struct ef_slot *ef_alloc_slot_ptr(struct ef_db *db, uint64_t *slot_id_out);
enum ef_err ef_free_slot(struct ef_db *db, uint64_t slot_id);
uint64_t ef_count_free_slots(const struct ef_db *db);

/* Iterate USED head slots (skips EF_STATUS_OVERFLOW continuation slots). */
typedef int (*ef_slot_visit_fn)(struct ef_db *db, uint64_t slot_id, struct ef_slot *slot, void *ctx);
enum ef_err ef_foreach_used(struct ef_db *db, ef_slot_visit_fn fn, void *ctx);

struct ef_slot_iter {
    struct ef_db *db;
    uint64_t index;
};

void ef_slot_iter_init(struct ef_db *db, struct ef_slot_iter *it);
int ef_slot_iter_next(struct ef_slot_iter *it, uint64_t *slot_id_out, struct ef_slot **slot_out);

/* Blob storage: head payload[0..3] = uint32_t total_len; overflow via next_offset chain. */
size_t ef_blob_inline_capacity(const struct ef_db *db);
size_t ef_blob_size(const struct ef_db *db, uint64_t slot_id);
enum ef_err ef_write_blob(struct ef_db *db, uint64_t slot_id, const void *data, size_t len);
enum ef_err ef_read_blob(struct ef_db *db, uint64_t slot_id, void *buf, size_t buf_cap, size_t *out_len);

/* Persistent LIFO free-list allocator with tail grow (ef_alloc_slot / ef_free_slot). */
enum ef_err ef_alloc(struct ef_db *db, uint64_t *slot_id_out);

/* Cross-process FIFO queue (dummy-head list, head/tail/lock in superblock reserved). */
enum ef_err ef_queue_push(struct ef_db *db, const void *data, uint8_t len);
enum ef_err ef_queue_pop(struct ef_db *db, void *buf, size_t buf_cap, size_t *out_len);
int ef_queue_empty(const struct ef_db *db);
/* True when the queue has no pending messages (checked under the queue lock). */
int ef_queue_drained(struct ef_db *db);

void ef_db_refresh_slot_crcs(struct ef_db *db);

/* Flush deferred superblock CRC (also runs inside ef_sync / ef_close). */
enum ef_err ef_db_commit_meta(struct ef_db *db);
void ef_db_mark_meta_dirty(struct ef_db *db);

void *ef_execute(struct ef_db *db, struct ef_cmd *cmd, const void *aux);

#endif
