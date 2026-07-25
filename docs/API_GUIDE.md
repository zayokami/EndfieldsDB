# Endfields DB — API Reference

This document is the practical companion to [`README.md`](README.md).
The README explains **why** the library is shaped the way it is; this
guide is **how** to call every public function, what it returns, and
what to do on each error.

Conventions used throughout:

- All APIs return `enum ef_err` (`EF_OK` on success) unless the return
  type is otherwise documented.
- Concurrency, durability, and ownership rules are summarised in
  [§10](#10-concurrency--caller-serialisation-cheat-sheet).
- Read [`THREADING.md`](../THREADING.md) for the full concurrency model.

---

## Table of contents

1. [Headers and library targets](#1-headers-and-library-targets)
2. [Lifecycle: open / close / sync / upgrade](#2-lifecycle-open--close--sync--upgrade)
3. [Physical addressing helpers](#3-physical-addressing-helpers)
4. [Slot-level reads and writes](#4-slot-level-reads-and-writes)
5. [Allocation and free-list](#5-allocation-and-free-list)
6. [Blob (large objects > 48 bytes)](#6-blob-large-objects--48-bytes)
7. [Iterating slots](#7-iterating-slots)
8. [Robin Hood string index](#8-robin-hood-string-index)
9. [FIFO queue](#9-fifo-queue)
10. [Transactions (v5)](#10-transactions)
11. [Low-level command executor (`ef_execute`)](#11-low-level-command-executor-ef_execute)
12. [Error codes](#12-error-codes)
13. [End-to-end recipes](#13-end-to-end-recipes)

---

## 1. Headers and library targets

```c
#include "endfields.h"   // core API, slots, superblock, queue, blob, txn
#include "ef_index.h"    // Robin Hood string index
```

`ef_txn.h` is pulled in transitively through `endfields.h` for the public
transaction calls; you only need to include it explicitly if you call
the internal `ef_txn_record_*` helpers from custom code (which the
library itself does, not application code).

Link against one of:

| Target | Backend |
|--------|---------|
| `endfields` | File `mmap` + RAM arena |
| `endfields_embedded` | Pure RAM only (no file I/O) |

CMake:

```cmake
target_link_libraries(your_app PRIVATE endfields)
target_include_directories(your_app PRIVATE path/to/endfields-db/src)
```

---

## 2. Lifecycle: open / close / sync / upgrade

### Opening a file

```c
struct ef_db *db = NULL;
enum ef_err err = ef_open_ex("data.endf", /*initial_slots=*/64, &db);
if (err != EF_OK || db == NULL) {
    fprintf(stderr, "open failed: %s\n", ef_strerror(err));
    return 1;
}
```

`initial_slots` is the size to allocate **only when the file does not
exist yet**. If the file already exists, the on-disk `max_slots` is kept
and `initial_slots` is ignored.

### Opening a file with an index

```c
/* hash_capacity must be a power of two and ≤ 65535. Pick at least 2x the
 * expected number of keys so that the auto-rehash threshold (3/4) is not
 * immediately hit. */
enum ef_err err = ef_open_ex_hash("data.endf", 64, 256, &db);
```

### Opening a file read-only

```c
struct ef_db *ro = NULL;
ef_open_readonly_ex("data.endf", &ro);
/* All write APIs return EF_ERR_READONLY on this handle. */
```

### Opening a RAM arena (embedded / tests)

```c
static uint8_t arena[64 * 1024];
struct ef_db *db = NULL;
ef_open_memory_hash(arena, sizeof(arena), /*max_slots=*/256,
                    /*hash_capacity=*/64, /*init_new=*/1, &db);
```

`init_new`:

- `1` — treat `arena` as uninitialised memory; the library writes a fresh
  superblock and free-list into it.
- `0` — reopen: validate the existing superblock / CRC and keep the data.

Reopens automatically detect re-sizes that happened previously
(`ef_grow`) and re-bind the slot region accordingly.

### Closing

```c
ef_close(db);
```

`ef_close` always flushes the deferred superblock CRC and calls
`ef_sync` semantics appropriate for the backend. Pair every successful
`ef_open*` with an `ef_close` on every exit path.

### Sync

```c
ef_sync(db);                       /* default = EF_SYNC_FULL */
ef_sync_ex(db, EF_SYNC_ASYNC);     /* async hint; see platform notes */
```

`ef_sync` flushes the deferred superblock CRC and, on file backends,
calls `msync` / `FlushViewOfFile` + `FlushFileBuffers`. Per-slot CRC
writes are not deferred — they hit the page cache immediately.

### Migration (`v1 → v2/v3 → v4 → v5`)

```c
if (ef_needs_upgrade(db)) {
    enum ef_err err = ef_upgrade(db);
    if (err != EF_OK) { /* handle */ }
    ef_sync(db);  /* persist the new layout */
}
```

A writable open of any older schema migrates up to v5 in place
(append the undo-log segment, refresh the superblock CRC). A read-only
open does **not** migrate; if you need to read a v4 or older file
without touching it, that's fine, but you will not get ACID transactions.

### Introspection

```c
int      readonly = ef_is_readonly(db);
uint32_t free_now = ef_count_free_slots(db);
enum ef_err last  = ef_last_error(db);
const char *what = ef_platform_name();   /* "posix" / "win32" / "embedded" */
const char *msg  = ef_strerror(err);
```

---

## 3. Physical addressing helpers

Slots are addressed by **physical offset inside the file/buffer**, not
by memory pointer. The helpers below convert between slot IDs, offsets,
and live pointers in the current mmap/buffer view.

```c
uint64_t offset      = ef_slot_to_offset(db, slot_id);
uint64_t slot_id_out = 0;
ef_offset_to_slot_id(db, offset, &slot_id_out);
void    *ptr         = ef_offset_to_ptr(db, offset);
```

```c
struct ef_slot *s = ef_get_slot(db, slot_id);   /* bounds + CRC check */
struct ef_slot *p = ef_peek_slot(db, slot_id);  /* bounds only, no CRC */
```

`ef_get_slot` returns `NULL` on a CRC mismatch or out-of-range slot ID;
the caller should treat that as corrupted data. `ef_peek_slot` is the
hot-path variant used by code that already holds an invariant (e.g.
inside `ef_queue_pop` under the queue lock).

### Pointer chasing

```c
struct ef_slot *cur = ef_get_slot(db, head_id);
while (cur) {
    /* ... */
    cur = ef_chase(db, cur);   /* follows next_offset */
}
```

```c
uint32_t hops_done = 0;
struct ef_slot *end = ef_chase_n(db, head_offset, /*hops=*/8, &hops_done);
if (!end) {
    /* EF_ERR_CHASE_CYCLE (cycle detected) or EF_ERR_CHASE_DEPTH */
}
```

`ef_chase` walks `next_offset` once. `ef_chase_n` walks up to `hops`
times with cycle detection; on a cycle it returns `NULL` and
`*hops_done_out` reports progress.

### Field access

A slot is 64 bytes laid out as:

```
status u32 | header_crc u32 | payload[48] | next_offset u64
```

```c
uint8_t *p = ef_get_field_ptr(slot, /*field_offset=*/8);   /* → payload[0] */
```

`field_offset` is the byte offset **within the slot**. `ef_get_field_ptr`
returns a pointer to `payload[field_offset - 8]` (offset 0 is reserved
for `status`-like use, 8 is the start of payload).

---

## 4. Slot-level reads and writes

```c
/* Write the first `len` bytes of a buffer into the slot's inline payload. */
enum ef_err ef_write_payload(struct ef_db *db, uint64_t slot_id,
                             const void *data, uint8_t len);   /* len ≤ 48 */
```

```c
/* Change a slot's status. */
enum ef_err ef_set_status(struct ef_db *db, uint64_t slot_id, uint32_t status);
/* Change a slot's next_offset (free-list link / blob continuation link / queue link). */
enum ef_err ef_set_next_offset(struct ef_db *db, uint64_t slot_id, uint64_t next_offset);
/* Write a single byte at a payload offset. */
enum ef_err ef_write_field(struct ef_db *db, uint64_t slot_id, uint8_t field_offset, uint8_t value);
```

> **Important**: `ef_write_payload`, `ef_write_blob`, and
> `ef_set_status(... EF_STATUS_USED)` **do not implicitly claim FREE
> slots**. You must allocate one with `ef_alloc_slot` / `ef_alloc` first
> and then write into it. Setting `EF_STATUS_USED` on a slot you did
> not own will corrupt the file.

Reads go through `ef_get_slot` (which validates CRC) followed by direct
field access:

```c
struct ef_slot *s = ef_get_slot(db, slot_id);
if (!s) { /* CRC mismatch or bad id */ }
/* s->status, s->payload[0..47], s->next_offset are now safe to read */
```

---

## 5. Allocation and free-list

The free list is a persistent **LIFO** linked list rooted in
`superblock.free_list_head`. All slots start on the free list when the
file is created.

### Basic alloc / free

```c
uint64_t id = 0;
enum ef_err err = ef_alloc(db, &id);   /* auto-grows the file by 1 slot if empty */
if (err != EF_OK) { /* EF_ERR_SLOT_FULL after ef_grow, EF_ERR_GROW inside a txn */ }

ef_free_slot(db, id);   /* pushes back onto the free list, removes any index entry */
```

`ef_alloc` calls `ef_alloc_slot` and, **only when the pool is empty**,
`ef_grow(max_slots + 1)` and retries. This is the only API that grows
the file implicitly.

### Manual alloc (no auto-grow)

```c
uint64_t id = 0;
enum ef_err err = ef_alloc_slot(db, &id);
if (err == EF_ERR_SLOT_FULL) {
    /* Pool is empty. Caller decides whether to grow or surface an error. */
    ef_grow(db, ef_count_free_slots(db) + 64);
    err = ef_alloc_slot(db, &id);
}
```

### Allocating without zeroing payload (queue hot path)

```c
enum ef_err err = ef_alloc_ex(db, &id, /*flags=*/0);            /* keep payload */
enum ef_err err = ef_alloc_ex(db, &id, EF_ALLOC_ZERO_PAYLOAD);   /* zero payload */
```

`ef_alloc_ex(db, &id, EF_ALLOC_ZERO_PAYLOAD)` is the safer default for
application code; the queue internally uses the non-zeroing variant to
avoid paying for a 48-byte memset on every push.

### Pre-sizing (important for transactions)

`ef_grow` and the implicit grow inside `ef_alloc` are **rejected inside
a transaction** (`EF_ERR_GROW`). Pre-size before `ef_txn_begin`:

```c
/* Builder phase: outside any transaction, grow to the target capacity. */
while (ef_count_free_slots(db) < target_headroom) {
    size_t new_max = db->sb->max_slots + 64;
    ef_grow(db, new_max);
}

/* Now transactions are safe — no implicit grow will fire. */
ef_txn_begin(db);
/* ... */
```

### Pointer-returning alloc

```c
uint64_t id = 0;
struct ef_slot *s = ef_alloc_slot_ptr(db, &id);   /* no CRC check */
/* Caller owns `s` only until the next `ef_free_slot` / `ef_index_*` / `ef_grow`. */
```

Use this when you are about to write a brand-new slot and want to skip
the CRC check on the (currently FREE) header.

### Free count

```c
uint32_t free_now = ef_count_free_slots(db);
```

This is the **in-memory** count mirrored from the superblock at open
time. It is incremented on `ef_free_slot` and decremented on `ef_alloc`,
so it is accurate for normal use; treat it as a hint, not a hard
guarantee.

---

## 6. Blob (large objects > 48 bytes)

A blob is a head slot whose `payload[0..3]` is a `uint32_t` total length
and `payload[4..7]` is the magic `EF_BLOB_MAGIC`, followed by inline data
(up to 40 bytes). Anything beyond 40 bytes overflows into chained
`EF_STATUS_OVERFLOW` continuation slots via `next_offset`.

```c
/* Write a 4 KiB blob. */
uint64_t id = 0;
ef_alloc(db, &id);
ef_write_blob(db, id, big_buffer, 4096);

/* Read it back. */
char buf[4096];
size_t out_len = 0;
ef_read_blob(db, id, buf, sizeof(buf), &out_len);
```

Inline capacity:

```c
size_t inline_cap = ef_blob_inline_capacity(db);   /* typically 40 */
size_t total_size = ef_blob_size(db, slot_id);      /* 0 if slot is not a blob */
```

`ef_write_blob` and `ef_write_payload` operate on **different layers**:
the former writes the blob header and may chain overflow slots; the
latter writes raw bytes into a slot's inline payload without setting the
blob magic. Do not mix the two on the same slot.

---

## 7. Iterating slots

### Used-slot visitor

```c
static int visit(struct ef_db *db, uint64_t slot_id, struct ef_slot *s, void *ctx) {
    (void)db; (void)ctx;
    printf("slot %llu: status=%u\n",
           (unsigned long long)slot_id, (unsigned)s->status);
    return 0;   /* 0 = continue, 1 = stop */
}
ef_foreach_used(db, visit, NULL);
```

`ef_foreach_used` walks **head slots only** — it skips
`EF_STATUS_OVERFLOW` continuation slots and queue internal slots. Use
this when you want to enumerate logical records.

### Iterator (explicit cursor)

```c
struct ef_slot_iter it;
ef_slot_iter_init(db, &it);
uint64_t id;
struct ef_slot *s;
while (ef_slot_iter_next(&it, &id, &s) == 1) {
    /* s is NOT CRC-checked; use s->status to decide what to read */
}
```

`ef_slot_iter_next` returns `1` while there are more slots, `0` at the
end. It walks **all** slot indices including internal/free slots — use
`s->status` to filter.

---

## 8. Robin Hood string index

Keys are hashed with FNV-1a into `uint64_t`; only the hash is stored.
Use this when you want O(1) lookup by string key.

### Put / get / remove

```c
uint64_t id = 0;
ef_alloc(db, &id);
ef_write_payload(db, id, "alice", 5);

ef_index_put(db, "user:alice", id);          /* insert or update */
ef_index_put(db, "user:alice", other_id);    /* update; no count change */

uint64_t out = 0;
if (ef_index_get(db, "user:alice", &out) == EF_OK) {
    /* out now holds the slot id */
}

ef_index_remove(db, "user:alice");            /* by key */
ef_index_remove_by_slot(db, id);              /* by slot (called by ef_free_slot) */
```

`ef_index_put` auto-rehashes to the next power-of-two capacity when the
load factor exceeds `3/4` (capped at `EF_INDEX_MAX_CAPACITY = 65535`).
Updating an existing key does **not** bump the entry count.

`ef_index_get` may return `EF_ERR_INDEX_BUSY` if a writer is in flight
(seqlock observed an odd sequence). Treat this as transient and retry:

```c
for (int spin = 0; spin < 4; ++spin) {
    enum ef_err err = ef_index_get(db, key, &out);
    if (err == EF_OK || err == EF_ERR_NOT_FOUND) break;
    if (err == EF_ERR_INDEX_BUSY) continue;
    /* other error */
}
```

### Atomic key → slot + payload peek

```c
struct ef_slot *s = NULL;
char buf[48];
size_t len = 0;
enum ef_err err = ef_index_get_slot(db, "user:alice",
                                    &s, buf, sizeof(buf), &len);
if (err == EF_OK) {
    /* `s` is a peek (no CRC), `buf` contains the first `len` bytes of payload */
}
```

`ef_index_get_slot` resolves the key **and** copies the payload under a
single seqlock window, closing the TOCTOU gap between `ef_index_get` and
`ef_get_slot`. Use it when you do not need full CRC validation.

### Rehash / shrink

```c
/* Manual grow to a larger power-of-two capacity. */
ef_index_rehash(db, 1024);

/* Pick a recommended shrink target. */
uint32_t target = ef_index_pick_shrink_capacity(ef_index_count_entries(db),
                                                ef_index_capacity(db));
if (target != 0 && target < ef_index_capacity(db)) {
    ef_index_shrink(db, target);
}

ef_index_clear(db);   /* empty the index; capacity stays */
```

`ef_index_rehash` **moves the slot region**, so any code holding slot
offsets must recompute them after rehash. Do not run rehash concurrently
with slot writes.

### Iteration

```c
static int list_keys(void *user, uint64_t key_hash, uint64_t slot_id) {
    (void)user;
    printf("hash=%016llx slot=%llu\n",
           (unsigned long long)key_hash, (unsigned long long)slot_id);
    return 0;   /* 0 = continue, 1 = stop, any negative = abort (EF_ERR_USER_ABORT) */
}

ef_index_iterate(db, list_keys, NULL);
```

`ef_index_iterate_until` is the same as `ef_index_iterate` but allows
the caller to terminate the iteration on a specific entry without
aborting the whole traversal.

### Introspection

```c
uint32_t cap     = ef_index_capacity(db);          /* 0 → index disabled */
uint32_t entries = ef_index_count_entries(db);     /* linear scan */
uint64_t h       = ef_key_hash("user:alice", 10);  /* direct FNV-1a */
```

---

## 9. FIFO queue

A persistent dummy-head FIFO list whose head, tail, and lock live in the
superblock `reserved[]` region. Supports **multi-producer / multi-consumer**
across threads and processes.

```c
/* Producer. */
ef_queue_push(db, "ready", 5);

/* Consumer. */
char buf[48];
size_t n = 0;
enum ef_err err = ef_queue_pop(db, buf, sizeof(buf), &n);
switch (err) {
case EF_OK:                 /* got n bytes of data */                  break;
case EF_ERR_QUEUE_EMPTY:    /* nothing to pop right now */             break;
case EF_ERR_QUEUE_BUSY:     /* lock contention; retry */               break;
default:                    /* genuine error */                        break;
}
```

Max payload per message: **47 bytes** (`field_offset` is the length,
plus 1 byte for the inline length prefix). For larger messages, store
them in a slot via `ef_write_blob` and push the slot id.

### Is the queue empty?

```c
if (ef_queue_empty(db)) { /* lock-free heuristic; reliable as a hint */ }
if (ef_queue_drained(db)) { /* held under queue lock; for shutdown */ }
```

`ef_queue_empty` is a lock-free fast path that reads `dummy.next_offset`
without taking the lock. Under heavy concurrent push/pop it can
temporarily disagree with the locked state. Use it only when an
eventually-consistent check is fine.

`ef_queue_drained` takes the queue lock and confirms the queue has no
pending messages. **This is the right primitive for the
producers-finished → consumer-shutdown pattern**:

```c
/* Producer threads. */
for (int i = 0; i < 1000; ++i) ef_queue_push(db, msg, len);
/* ... signal "no more producers" */

void *consumer(void *arg) {
    for (;;) {
        char buf[64]; size_t n;
        enum ef_err e = ef_queue_pop(db, buf, sizeof(buf), &n);
        if (e == EF_OK) { process(buf, n); continue; }
        if (e == EF_ERR_QUEUE_BUSY) { /* retry */ continue; }
        if (e == EF_ERR_QUEUE_EMPTY) {
            if (producers_done && ef_queue_drained(db)) return NULL;
            /* producers still working, or a transient window; back off */
            sleep_a_bit();
            continue;
        }
        /* genuine error */
    }
}
```

### Push failure

`ef_queue_push` may fail with `EF_ERR_QUEUE_BUSY` under heavy contention.
Retry with backoff. If it fails with `EF_ERR_SLOT_FULL`, the slot pool
is exhausted — `ef_alloc` outside the queue path will grow it; push
always tries to allocate a slot before linking it into the queue.

---

## 10. Transactions (v5)

The v5 transaction model is **strict serializable isolation** with a
single per-DB `txn_lock` (independent of `index_write_lock` and
`queue_lock`). A transaction is bracketed by `ef_txn_begin` and
`ef_txn_commit` or `ef_txn_abort`.

```c
enum ef_err err = ef_txn_begin(db);
if (err != EF_OK) {
    if (err == EF_ERR_TXN_BUSY) { /* another transaction is active; retry */ }
    /* other errors */
}

ef_alloc_slot(db, &id);
ef_write_payload(db, id, "draft", 5);
ef_index_put(db, "k", id);

if (/* business condition not satisfied */ 0) {
    ef_txn_abort(db);   /* everything above is rolled back */
} else {
    ef_txn_commit(db);  /* atomic */
}
```

Inside a transaction, every mutating API — `ef_alloc_slot`, `ef_free_slot`,
`ef_write_payload`, `ef_write_blob`, `ef_set_status`, `ef_set_next_offset`,
`ef_index_put`, `ef_index_remove`, `ef_index_remove_by_slot`,
`ef_index_clear`, `ef_queue_push`, `ef_queue_pop` — appends an inverse
record to the undo log. `ef_txn_abort` reverse-replays that log to
restore the database to its state at `ef_txn_begin`.

### Constraints inside a transaction

```c
ef_txn_begin(db);

/* Rejected: EF_ERR_GROW */
ef_grow(db, db->sb->max_slots + 1);

/* Rejected: EF_ERR_INDEX_BUSY (would trigger auto-rehash) */
ef_index_put(db, "x", id);   /* if load factor already at 3/4 */

/* Allowed: rehash to a capacity that does not move slots
 * (no-op when new_capacity == current capacity). */
ef_index_rehash(db, ef_index_capacity(db));
```

**Pre-size the database before `ef_txn_begin`.** A safe pattern:

```c
/* Builder phase. */
size_t want = max_expected_keys * 2;   /* stay well below 3/4 threshold */
ef_index_rehash(db, /* power-of-two ≥ want */);
while (ef_count_free_slots(db) < 1024) {
    ef_grow(db, db->sb->max_slots + 1024);
}

ef_txn_begin(db);
/* ... application logic ... */
ef_txn_commit(db);
```

### Cross-process transaction

The same `txn_lock` byte serialises transactions across processes mapped
to the same file. If process A holds the lock and process B calls
`ef_txn_begin`, B gets `EF_ERR_TXN_BUSY`.

### Crash recovery

If a process dies while a transaction is active, the next open detects
`txn_state == ACTIVE` and a writer pid/epoch mismatch, and:
1. reverse-replays the undo log to roll back,
2. clears the transaction lock,

so the database returns to a consistent state. No operator intervention
is needed.

### When to commit vs abort

```c
ef_txn_begin(db);
err = do_application_logic(db);
return err == EF_OK ? ef_txn_commit(db) : ef_txn_abort(db);
```

`ef_txn_commit` is essentially free (clears the undo log, releases the
lock). `ef_txn_abort` costs an O(undo-log size) reverse replay; do not
use it as a control-flow tool.

### Active transaction check

```c
if (ef_txn_active(db)) { /* a transaction is in flight on this handle */ }
```

This is a coarse hint useful for assertions and debug output. The
authoritative state is the `txn_lock` byte in the superblock, which is
accessed atomically inside `ef_txn_begin`.

---

## 11. Low-level command executor (`ef_execute`)

For benchmarks and round-trip tests, the library exposes a single
dispatch entry point that takes a packed 10-byte `ef_cmd`:

```c
struct ef_cmd {
    uint8_t opcode;        /* EF_OP_* */
    uint64_t param;        /* slot id, offset, etc. */
    uint8_t field_offset;  /* payload offset, length, etc. */
} __attribute__((packed));
```

```c
uint8_t payload_out[48];
struct ef_cmd cmd = {
    .opcode = EF_OP_GET_SLOT,
    .param  = slot_id,
    .field_offset = 0,
};
void *ret = ef_execute(db, &cmd, /*aux=*/NULL);
```

Opcodes:

| Opcode | Semantics | `param` | `field_offset` | `aux` |
|--------|-----------|---------|----------------|-------|
| `EF_OP_GET_SLOT` | bounds + CRC check | slot_id | — | — |
| `EF_OP_CHASE` | single `next_offset` hop | slot_id | — | — |
| `EF_OP_CHASE_N` | multi-hop | start_offset | hops | — |
| `EF_OP_GET_FIELD` | pointer to field | slot_id | field_offset | — |
| `EF_OP_WRITE_FIELD` | write one byte | slot_id | field_offset | value byte |
| `EF_OP_WRITE_PAYLOAD` | write `len` bytes | slot_id | len | data |
| `EF_OP_SET_NEXT` | set `next_offset` | slot_id | — | u64 offset |
| `EF_OP_SET_STATUS` | set status | slot_id | — | u32 status |
| `EF_OP_ALLOC` | `ef_alloc_slot` | — | — | — |
| `EF_OP_FREE` | `ef_free_slot` | slot_id | — | — |
| `EF_OP_INDEX_PUT` | `ef_index_put` | slot_id | — | key (len-prefixed) |
| `EF_OP_INDEX_GET` | `ef_index_get` | — | — | key |
| `EF_OP_INDEX_REMOVE` | `ef_index_remove` | — | — | key |
| `EF_OP_INDEX_CLEAR` | `ef_index_clear` | — | — | — |
| `EF_OP_QUEUE_PUSH` | `ef_queue_push` | — | len | data |
| `EF_OP_QUEUE_POP` | `ef_queue_pop` | — | buf_cap | — |

`ef_execute` is what the bench suite uses; for normal application code
prefer the typed wrappers in `src/endfields.h` and `src/ef_index.h`.

---

## 12. Error codes

| Code | Meaning | Suggested action |
|------|---------|------------------|
| `EF_OK` | Success | — |
| `EF_ERR_NULL_ARG` | `db` or a required pointer is NULL | Fix caller |
| `EF_ERR_IO` | Underlying file I/O failed | Check errno, retry or surface |
| `EF_ERR_MMAP` | `mmap` / `MapViewOfFile` failed | Reduce requested size or check system limits |
| `EF_ERR_OOM` | Out of memory | Reduce capacity or fix the leak |
| `EF_ERR_BAD_MAGIC` | File header is not `ENDF` | Confirm the path |
| `EF_ERR_BAD_VERSION` | Schema version newer than the library supports | Upgrade the library |
| `EF_ERR_BAD_CHECKSUM` | Superblock CRC mismatch | File is corrupted |
| `EF_ERR_BAD_SLOT_SIZE` | `slot_size` is not 64 | The file was written by a different layout |
| `EF_ERR_FILE_SIZE` | File size doesn't match the layout | Truncate or restore from backup |
| `EF_ERR_SLOT_ID` | Slot id out of range | Caller bug |
| `EF_ERR_OFFSET` | Offset out of range | Caller bug |
| `EF_ERR_PAYLOAD_LEN` | Payload length > 48 | Resize or use blobs |
| `EF_ERR_OPCODE` | `ef_execute` got an unknown opcode | Caller bug |
| `EF_ERR_SLOT_FREE` | `ef_get_slot` on a FREE slot | Caller bug |
| `EF_ERR_SLOT_BUSY` | Slot is in a state that prevents the operation | Caller bug |
| `EF_ERR_SLOT_FULL` | Free list empty and `ef_alloc` will not grow | Call `ef_grow` or pre-allocate |
| `EF_ERR_NOT_FOUND` | Index lookup miss | Normal — handle in caller |
| `EF_ERR_CHASE_DEPTH` | `ef_chase_n` ran out of hops | Increase the hop budget |
| `EF_ERR_CHASE_CYCLE` | `ef_chase_n` detected a cycle | Data corruption |
| `EF_ERR_READONLY` | Write on a read-only handle | Open the handle with write permission |
| `EF_ERR_GROW` | `ef_grow` (or auto-grow inside `ef_alloc`) is rejected | Pre-size **before** `ef_txn_begin` |
| `EF_ERR_QUEUE_EMPTY` | `ef_queue_pop` on an empty queue | Normal — retry / exit |
| `EF_ERR_QUEUE_BUSY` | Queue lock contention | Retry with backoff |
| `EF_ERR_INDEX_FULL` | Hash table is at `EF_INDEX_MAX_CAPACITY` | Re-design key space |
| `EF_ERR_INDEX_BUSY` | Index seqlock observed a writer; retry | Retry with backoff |
| `EF_ERR_USER_ABORT` | `ef_index_iterate` callback returned negative | Inspect the callback |
| `EF_ERR_TXN_BUSY` | Another transaction is active | Retry with backoff |
| `EF_ERR_TXN_LOG_FULL` | Undo log ran out of slots | Increase `EF_UNDO_LOG_DEFAULT_BYTES` or shorten the txn |

`ef_strerror(err)` returns a static, human-readable string for every
code above.

---

## 13. End-to-end recipes

### Append-only append-log built on the slot allocator

```c
struct ef_db *db;
ef_open_ex("log.endf", 1024, &db);

const char *line = "user logged in\n";
size_t len = strlen(line);

uint64_t id = 0;
ef_alloc_slot(db, &id);
ef_write_payload(db, id, line, (uint8_t)len);
/* Optionally also index by sequence number:
 *   ef_index_put(db, "seq:42", id);
 */
ef_sync(db);
ef_close(db);
```

### Slot-addressed key/value store with ACID

```c
struct ef_db *db;
ef_open_ex_hash("kv.endf", 1024, 256, &db);

/* Pre-size so the txn does not grow. */
while (ef_count_free_slots(db) < 1024) ef_grow(db, db->sb->max_slots + 1024);

ef_txn_begin(db);

uint64_t id = 0;
ef_alloc_slot(db, &id);
ef_write_payload(db, id, "alice", 5);
ef_index_put(db, "user:alice", id);

uint64_t id2 = 0;
ef_alloc_slot(db, &id2);
ef_write_payload(db, id2, "bob", 3);
ef_index_put(db, "user:bob", id2);

ef_txn_commit(db);

ef_close(db);
```

### Producer-consumer with clean shutdown

```c
struct ef_db *db;
ef_open_ex("pipe.endf", 4096, &db);

/* Producer thread */
for (int i = 0; i < 1000; ++i) {
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "msg-%d", i);
    while (ef_queue_push(db, msg, (uint8_t)n) == EF_ERR_QUEUE_BUSY) { /* spin */ }
}
atomic_store(&producers_done, 1);

/* Consumer thread */
for (;;) {
    char buf[64]; size_t n = 0;
    enum ef_err e = ef_queue_pop(db, buf, sizeof(buf), &n);
    if (e == EF_OK) { handle(buf, n); continue; }
    if (e == EF_ERR_QUEUE_BUSY) { /* spin */ continue; }
    if (e == EF_ERR_QUEUE_EMPTY) {
        if (atomic_load(&producers_done) && ef_queue_drained(db)) break;
        nanosleep((struct timespec[]){{0, 1e6}}, NULL);
        continue;
    }
    /* genuine error */
}

ef_close(db);
```

### Read-only inspection of a live database

```c
struct ef_db *ro;
ef_open_readonly_ex("data.endf", &ro);

ef_foreach_used(ro, dump, NULL);

ef_close(ro);
```

`ef_open_readonly_ex` does not migrate the schema. If the file is on
v1/v2/v3/v4 and you open it read-only, you cannot start a transaction
on this handle.

### Iterate the index, draining into a flat array

```c
struct drain_ctx {
    uint64_t *out;
    size_t cap, n;
};

static int collect(void *user, uint64_t key_hash, uint64_t slot_id) {
    struct drain_ctx *c = user;
    if (c->n >= c->cap) return 1;          /* stop: array full */
    c->out[c->n++] = slot_id;
    return 0;
}

uint64_t ids[1024];
struct drain_ctx ctx = { ids, 1024, 0 };
ef_index_iterate(db, collect, &ctx);
```

### Manual blob indexing

```c
/* Write a 4 KiB blob. */
uint64_t id = 0;
ef_alloc(db, &id);
ef_write_blob(db, id, buffer, 4096);
ef_index_put(db, "report:2026-Q3", id);

/* Read it back. */
uint64_t out = 0;
if (ef_index_get(db, "report:2026-Q3", &out) == EF_OK) {
    char buf[4096]; size_t n = 0;
    ef_read_blob(db, out, buf, sizeof(buf), &n);
    /* process n bytes of buf */
}
```

---

## 14. Concurrency / caller-serialisation cheat sheet

| API | Thread safety |
|-----|---------------|
| `ef_open*` / `ef_close` | Single-threaded; don't share handles across processes via `fork` after open |
| `ef_alloc_slot` / `ef_alloc` / `ef_free_slot` | ✅ CAS on GCC/Clang (`ef_alloc` may fail inside a txn — pre-size) |
| `ef_write_payload` / `ef_write_blob` / `ef_set_status` / `ef_set_next_offset` | ⚠️ Caller serialises slot-level writes |
| `ef_queue_push` / `ef_queue_pop` | ✅ MPMC (across processes and threads) |
| `ef_queue_empty` | ✅ Lock-free heuristic |
| `ef_queue_drained` | ✅ Locked; use for shutdown |
| `ef_index_get` | ✅ Lock-free multi-reader (seqlock); retry on `EF_ERR_INDEX_BUSY` |
| `ef_index_get_slot` | ✅ Same as `ef_index_get`; no CRC |
| `ef_index_put` / `remove` / `clear` | 🔒 Single writer (`index_write_lock`); may queue |
| `ef_index_rehash` / `ef_index_shrink` | 🔒 Single writer; **moves slot region during rehash** — recompute offsets |
| `ef_index_iterate` / `ef_index_iterate_until` | 🔒 Single writer outside a writer; OK to read alongside other readers |
| `ef_txn_begin` / `commit` / `abort` | 🔒 Single transaction per handle; same lock across processes |
| `ef_sync` / `ef_close` | ⚠️ Caller serialises against all writers |
| `ef_grow` | ⚠️ Caller serialises; rejected inside a transaction |
| `ef_foreach_used` / `ef_slot_iter_next` | ⚠️ Caller serialises against slot-region mutators (rehash, free_list admin) |

For the full rationale, including the order of operations in the queue
and free-list paths, see [`THREADING.md`](../THREADING.md).
