#ifndef EF_UNDO_H
#define EF_UNDO_H

#include "endfields.h"
#include "ef_atomic_unaligned.h"

/* v5 undo log layout.
 *
 * The undo log is a single, fixed-size segment appended after the slot area in
 * the database file. Its purpose is to record every mutating action that
 * occurs inside a transaction so that ef_txn_abort can reverse the changes in
 * reverse order. The log is only written to during an active transaction; it
 * is otherwise idle.
 *
 * On-disk layout:
 *   [ undo_header (32B) ][ record 0 (32B) ][ record 1 (32B) ] ... [ record N-1 (32B) ]
 *
 * The header is identified by a 4-byte magic. Its CRC covers the entire
 * 32-byte header for tamper detection. `tail` is the file-relative byte
 * offset where the next record will be written; it advances monotonically
 * while a transaction is active and is reset to sizeof(header) on commit.
 *
 * Each record describes a single mutating action and the data needed to undo
 * it. Records are 32 bytes; `target_offset` and `aux` carry slot / index /
 * queue context, and `before[16]` carries a 16-byte snapshot of the affected
 * fields. Records are appended in write order; abort replays them in reverse.
 */
#define EF_UNDO_HEADER_MAGIC 0x4F444E55U /* 'UNDO' little-endian */
#define EF_UNDO_RECORD_SIZE  32U
#define EF_UNDO_HEADER_SIZE  32U

/* Undo record kinds. aux and before[] are interpreted differently per kind;
 * see the comments in ef_undo.c for the per-kind layout. */
enum ef_undo_kind {
    EF_UNDO_KIND_NONE = 0,
    /* Slot allocation: a slot was popped from the free list. Restoring means
     * pushing the slot back to the head of the free list. aux = 0. before
     * holds the slot's prior status/next_offset/header_crc snapshot. */
    EF_UNDO_KIND_SLOT_ALLOC,
    /* Slot free: a slot was returned to the free list. Restoring means
     * unlinking the slot from the free list. aux = slot_offset. before holds
     * the slot's prior status/next_offset/header_crc snapshot. */
    EF_UNDO_KIND_SLOT_FREE,
    /* Slot status change (non-alloc/non-free, e.g. USED -> QUEUED or
     * OVERFLOW). aux = slot_offset. before holds the prior status + crc. */
    EF_UNDO_KIND_SLOT_STATUS,
    /* Slot next_offset change. aux = slot_offset. before holds the prior
     * next_offset (8 bytes) + a 0-padded snapshot. */
    EF_UNDO_KIND_SLOT_NEXT,
    /* Slot payload write. aux = slot_offset. before holds the first 16 bytes
     * of the prior payload (CRC verification still applies on USED). */
    EF_UNDO_KIND_SLOT_PAYLOAD,
    /* Index entry insertion: a new (key_hash, slot_offset) was inserted. aux
     * = slot_offset. before holds key_hash (8 bytes) + 0 pad; on abort we
     * remove the entry by key_hash. */
    EF_UNDO_KIND_INDEX_PUT,
    /* Index entry removal: an existing entry was removed. aux = slot_offset.
     * before holds key_hash + prior slot_offset, both restored on abort. */
    EF_UNDO_KIND_INDEX_REMOVE,
    /* Queue push: a new node was linked into the FIFO. aux = slot_offset of
     * the new node. before holds the prior tail's next_offset (8 bytes) and
     * the new node's prior status. */
    EF_UNDO_KIND_QUEUE_PUSH,
    /* Queue pop: a node was removed from the FIFO. aux = slot_offset of the
     * popped node. before holds the prior dummy's next_offset, popped node
     * next_offset, and prior tail value. */
    EF_UNDO_KIND_QUEUE_POP,
    /* Free count change. aux = delta. before unused. */
    EF_UNDO_KIND_FREE_COUNT
};

#pragma pack(push, 1)
struct ef_undo_record {
    uint8_t  kind;
    uint8_t  flags;          /* reserved */
    uint16_t size;           /* record body size in bytes; always 32 currently */
    uint64_t target_offset;  /* primary target (slot offset / queue node offset) */
    uint64_t aux;            /* secondary (slot_offset, delta, etc.) */
    uint8_t  before[12];     /* per-kind snapshot (see enum ef_undo_kind docs) */
};
#pragma pack(pop)

_Static_assert(sizeof(struct ef_undo_record) == 32, "ef_undo_record must be 32 bytes");

struct ef_undo_header {
    uint32_t magic;
    uint32_t crc;
    uint64_t tail;             /* offset (bytes) of the next record within the log */
    uint32_t record_count;     /* number of records in the current transaction */
    uint32_t reserved;
    uint8_t  pad[8];           /* pad to 32 bytes total */
};

_Static_assert(sizeof(struct ef_undo_header) == 32, "ef_undo_header must be 32 bytes");

/* Undo log accessors. All accessors are in-memory pointers to the persistent
 * segment; they operate on the mmap directly. */
struct ef_undo_header *ef_undo_header_ptr(const struct ef_db *db);
struct ef_undo_record *ef_undo_record_at(const struct ef_db *db, uint64_t offset_in_log);

/* Compute header CRC; returns the value that should be stored in header.crc.
 * Internal helper. Declared here for visibility from tests; defined in ef_undo.c. */
uint32_t ef_undo_header_crc_compute(const struct ef_undo_header *h);

/* Reset the undo log to a clean state (zero records). Safe to call on a
 * freshly-opened v5 file. */
void ef_undo_reset(struct ef_db *db);

/* Validate the header (magic + CRC). Returns 1 if consistent, 0 if the log
 * is empty / unwritten / corrupt. */
int ef_undo_header_valid(const struct ef_undo_header *h);

/* Append a record of the given kind. `before` points to 12 bytes that are
 * copied into the record's snapshot. Returns EF_ERR_TXN_LOG_FULL if full. */
enum ef_err ef_undo_record_append(struct ef_db *db, uint8_t kind, uint64_t target_offset,
                                  uint64_t aux, const void *before);

/* Replay undo records in reverse order, restoring slot/index/queue state to
 * the pre-begin snapshot. Returns EF_OK on full replay; surfaces undo errors
 * to the caller via ef_set_error. */
enum ef_err ef_undo_replay_reverse(struct ef_db *db);

/* Debug-only: walk the undo log from header.tail back to sizeof(header) in
 * reverse. Returns the count of records walked. */
uint32_t ef_undo_walk_reverse(const struct ef_db *db);

#endif
