# Endfields DB

[![CI](https://github.com/zayokami/EndfieldsDB/actions/workflows/ci.yml/badge.svg)](https://github.com/zayokami/EndfieldsDB/actions/workflows/ci.yml)

A C11 embedded database core built around fixed 64-byte slots and file/buffer
physical-offset addressing. It provides a persistent LIFO free-list allocator,
a cross-process MPMC FIFO queue, a Robin Hood string hash index, abortable
strict-serializable transactions, and on-demand schema migrations.

Small footprint, few dependencies (C standard library plus the platform `mmap`
API). Drops into a server, an embedded target, or a CLI tool as a lightweight
persistence layer.

> Other languages: [简体中文](README.zh-CN.md)

---

## What problem does it solve

Many C projects need a small, controllable local store: write a few records,
read them back, look them up by string key, possibly queue work across
processes, occasionally want ACID transactions and crash recovery.

Endfields DB ships these as a **library**, not a separate service:

- **No extra layer** — there is no dedicated server / protocol / client.
  `#include "endfields.h"` and call `ef_alloc` / `ef_write_payload` /
  `ef_index_get` / `ef_txn_begin` directly.
- **Physical-offset addressing** — data is laid out in 64-byte aligned slots.
  `ef_offset_to_ptr(slot_id)` is an O(1) pointer lookup, perfect for `mmap`
  and `ef_chase`-style pointer chasing.
- **Zero-copy** — slots are mapped directly into the caller's address space.
  The payload bytes *are* the file bytes; no serialization layer in between.
- **One library, many backends** — the same `ef_open_ex` lands on a disk
  `mmap`, while `ef_open_memory` runs in an embedded RAM arena. Schema, API,
  CRC scheme, and concurrency model are identical.
- **Opt-in capabilities** — no index? `hash_capacity = 0` keeps the file
  layout identical to legacy v3. Need ACID? Wrap calls in
  `ef_txn_begin` / `commit`.

---

## At a glance

| Category | What you get |
|----------|--------------|
| Persistence | Disk `mmap` (POSIX `mmap` / Win32 `MapViewOfFile`) or pure RAM arena; `ef_sync` for explicit flush |
| Data model | Fixed 64-byte slots, 48-byte inline payload; oversized objects spill to an `OVERFLOW` chained blob |
| Addressing | slot id ↔ physical offset ↔ mmap pointer; `ef_chase` / `ef_chase_n` walk the `next_offset` chain |
| Allocator | Persistent LIFO free list (`ef_alloc_slot` / `ef_free_slot`); `ef_alloc` auto-grows when the pool is empty |
| Index | Robin Hood string hash (`ef_index_put/get/remove/iterate/clear`), auto-rehash + manual shrink |
| Queue | Cross-process MPMC dummy-head FIFO (`ef_queue_push/pop`), guarded by a superblock spinlock |
| Transactions (v5) | `ef_txn_begin` / `commit` / `abort`; strict serializable isolation; reverse-replay undo log; crash recovery |
| Integrity | Slot header CRC32 + deferred superblock CRC; x86 PCLMULQDQ fast path |
| Migration | v1 → v2/v3 → v4 → v5 upgraded in place; writable open auto-appends the undo-log segment; read-only opens keep the old layout |

Current schema version: **v5** (`EF_SCHEMA_VERSION 5` in `src/ef_config.h`).

---

## 30-second quickstart

```c
#include "endfields.h"
#include "ef_index.h"

struct ef_db *db = NULL;

/* File backend + Robin Hood index (hash_capacity must be a power of two, ≤ 65535) */
if (ef_open_ex_hash("data.endf", 64, 256, &db) != EF_OK || db == NULL) {
    return 1;
}

/* Allocate a slot, write its payload, index it */
uint64_t id = 0;
ef_alloc(db, &id);
ef_write_payload(db, id, "hello", 5);
ef_index_put(db, "greeting", id);

/* Look it up by key */
uint64_t found = 0;
if (ef_index_get(db, "greeting", &found) == EF_OK) {
    /* ... */
}

/* ACID transaction: abort to roll back any failing branch */
ef_txn_begin(db);
ef_alloc_slot(db, &id);
ef_write_payload(db, id, "draft", 5);
ef_index_put(db, "k", id);
if (/* business condition not satisfied */ 0) {
    ef_txn_abort(db);   /* all writes are undone */
} else {
    ef_txn_commit(db);  /* atomic */
}

ef_sync(db);
ef_close(db);
```

Full API listings: [`src/endfields.h`](src/endfields.h),
[`src/ef_index.h`](src/ef_index.h),
[`src/ef_txn.h`](src/ef_txn.h).

---

## Key concepts

### Slot

All data lives in **64-byte** aligned slots. The superblock itself is one slot:

```
status u32 | header_crc u32 | payload[48] | next_offset u64   →  64 bytes total
```

`status` values are defined in [`src/endfields.h`](src/endfields.h)
(`EF_STATUS_USED`, `_FREE`, `_OVERFLOW`, `_QUEUED`, `_QUEUE_DUMMY`, etc.).
`next_offset` doubles as the free-list link, the blob chain link, and the
queue link; `ef_chase` / `ef_chase_n` walk it.

### Physical addressing

Data is addressed by **byte offset inside the file/buffer**, not by memory
pointer:

| Conversion | Purpose |
|------------|---------|
| `ef_slot_to_offset(slot_id)` | id → physical offset |
| `ef_offset_to_slot_id(db, offset, *out)` | offset → id |
| `ef_offset_to_ptr(db, offset)` | offset → a read/write pointer in the current mmap/buffer |
| `ef_chase(db, start, fn, ctx)` | walk one or many `next_offset` links |

This means slots can be remapped, files can be remapped, rehash can move
the entire slot region — as long as the offsets stay consistent.

### File layout

```
v5 (with index + transactions):
[superblock 64B][hash index capacity × 16B][slot region max_slots × 64B][undo-log segment]

v3/v4 (with index, no transactions):
[superblock 64B][hash index capacity × 16B][slot region max_slots × 64B]

v3/v4 (no index, backward compatible):
[superblock 64B][slot region max_slots × 64B]
```

New files default to `hash_capacity = 0` (compact, no-index layout). Any
schema is automatically migrated up to v5 on a writable open, with the
undo-log segment appended and **existing data left untouched**.

### FIFO queue

A dummy-head list whose head, tail, and lock all live in the superblock
`reserved[]`. `push` and `pop` happen under the same spinlock; the dummy
sentinel slot is allocated lazily.

- `ef_queue_empty` is a **lock-free** heuristic — only reliable as a hint.
- For multi-consumer shutdown, after all producers finish, use
  `ef_queue_drained` (a lock-held check) to confirm the queue is empty.
- Heavy contention returns `EF_ERR_QUEUE_BUSY`; callers must retry.

### Index

String keys are hashed with FNV-1a into `uint64_t`; Robin Hood linear probing
is used. **Only the hash is stored, never the original key.**

- `ef_index_put` auto-rehashes to the next power-of-two capacity when the
  load factor exceeds `3/4` (capped at `EF_INDEX_MAX_CAPACITY = 65535`).
- `ef_index_get` is a **lock-free multi-reader** lookup via seqlock. If a
  writer is in flight it returns `EF_ERR_INDEX_BUSY`; callers must retry.
- All writers (`put` / `remove` / `rehash` / `clear`) serialise through
  `index_write_lock`.
- `ef_index_rehash` **moves the slot region**, so it must not run
  concurrently with slot writes.

### Transactions (v5)

```c
ef_txn_begin(db);
// Every mutating API (alloc/free/index_put/index_remove/queue_push/queue_pop/
// write_payload/write_blob/...) appends an inverse record to the undo log.
ef_txn_commit(db);   // commit: clear undo log, release txn_lock
ef_txn_abort(db);    // rollback: reverse-replay the undo log, release lock
```

Key points:

- **Strict serializable isolation**: a dedicated `txn_lock` is decoupled
  from `index_write_lock` and `queue_lock`, so there is no deadlock between
  the three locks.
- During a transaction, `ef_grow` returns `EF_ERR_GROW` and `ef_index_put`
  returns `EF_ERR_INDEX_BUSY` when it would trigger a rehash. **Pre-size
  the database before `ef_txn_begin`.**
- **Cross-process crash recovery**: on open, if `txn_state == ACTIVE` and
  the writer pid / epoch does not match the current process, the undo log
  is replayed automatically and the lock is released.
- v4 → v5 migration is transparent to existing data: `ef_sb_migrate_v4_txn_layout`
  appends the undo-log segment and refreshes the superblock CRC on the
  first writable open.

---

## Concurrency model at a glance

Full details: [`THREADING.md`](THREADING.md). Quick reference:

| Resource | Thread safety |
|----------|--------------|
| `ef_queue_push` / `ef_queue_pop` | ✅ MPMC (across processes and threads) |
| `ef_index_get` | ✅ Lock-free multi-reader (seqlock) |
| `ef_index_put` / `remove` / `rehash` | 🔒 Single writer (`index_write_lock`); multiple threads queue |
| `ef_txn_begin` / `commit` / `abort` | 🔒 Single transaction (`txn_lock` CAS); no deadlock with the locks above |
| Free list | ✅ CAS pop/push on GCC/Clang |
| Slot payload / blob data / `ef_chase` | ⚠️ Caller serialises |
| `ef_sync` / `ef_close` | ⚠️ Caller serialises against all writers |

During a transaction, `ef_grow` and auto-rehash are disabled; the caller is
expected to **pre-allocate the needed capacity before `ef_txn_begin`**.

---

## Build

Requires **CMake ≥ 3.16** plus a C11-capable compiler (GCC / Clang / MSVC / MinGW).

### Minimal build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows with MinGW:

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Strict CI (reproduce locally)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DENDFIELDS_WARNINGS_AS_ERRORS=ON -DENDFIELDS_CI_FAST=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 900 --no-tests=error
```

CI matrix (see [`.github/workflows/ci.yml`](.github/workflows/ci.yml)):
Linux GCC / Clang (Release, Debug, ASan+UBSan, TSan, coverage),
macOS Clang, Windows MSVC, Windows MinGW, static analysis
(`clang-tidy` + `cppcheck`), embedded-only.

### CMake options

| Option | Default | Purpose |
|--------|---------|---------|
| `ENDFIELDS_EMBEDDED_ONLY` | OFF | Build only the RAM backend |
| `ENDFIELDS_ENABLE_PREFETCH` | ON | Enable `__builtin_prefetch` on the chase hot path |
| `ENDFIELDS_WARNINGS_AS_ERRORS` | OFF | `-Werror` |
| `ENDFIELDS_SANITIZE` | OFF | ASan + UBSan |
| `ENDFIELDS_TSAN` | OFF | ThreadSanitizer |
| `ENDFIELDS_COVERAGE` | OFF | gcov / lcov |
| `ENDFIELDS_CI_FAST` | OFF | Short bench loops |

### Linking

```cmake
target_link_libraries(your_app PRIVATE endfields)
target_include_directories(your_app PRIVATE path/to/endfields-db/src)
```

Artifacts: `libendfields.a` (file + memory backend),
`libendfields_embedded.a` (pure RAM),
and the four test executables `endfields_core_test`,
`endfields_index_queue_test`, `endfields_embedded_test`, `endfields_bench`.

---

## Test coverage

Tests are split by entry point (see [`CMakeLists.txt`](CMakeLists.txt)):

- `tests/test_core.c` — basic read/write, CRC, blob, grow, reopen,
  transaction commit/abort/persist/grow-forbidden
- `tests/test_index_queue.c` — index put/get/rehash/shrink/iterate,
  queue MPMC, v3→v4 / v4→v5 migration, transaction behaviour over
  index and queue
- `src/main_embedded.c` — pure RAM backend
- `bench/endfields_bench.c` — chase / queue / MPMC / hash /
  `bench_txn_roundtrip`

Run a single binary directly: `./build/endfields_core_test`. Filter by
CTest name: `ctest --test-dir build -R txn --output-on-failure`.

---

## Project layout

```
endfields-db/
├── CMakeLists.txt
├── README.md                      # you are here
├── README.zh-CN.md                # 简体中文
├── THREADING.md                    # concurrency / cross-process semantics
├── PROJECT_INDEX.md                # codebase handover index
├── CLAUDE.md                       # project guide for AI assistants
├── src/
│   ├── endfields.h / .c            # public API and core implementation
│   ├── ef_index.h / .c             # Robin Hood index (v4 seqlock + auto-rehash + shrink)
│   ├── ef_sb_layout.h / .c         # superblock reserved[] layout + v3→v4→v5 migration
│   ├── ef_txn.h / .c               # transaction API and state machine (v5)
│   ├── ef_undo.h / .c              # undo-log segment, record format, replay/reset (v5)
│   ├── ef_blob.c                   # large object chaining
│   ├── ef_grow.c                   # auto-grow
│   ├── ef_port.h / .c              # file/memory I/O abstraction
│   ├── ef_atomic_unaligned.h       # atomic helpers for mmap fields
│   ├── ef_crc.h / .c / _pclmul.c   # CRC32 portable + x86 PCLMULQDQ
│   ├── ef_config.h                 # schema version + platform switches
│   ├── main.c / main_embedded.c    # legacy test entry points (superseded by tests/)
├── tests/
│   ├── test_common.h / .c
│   ├── test_core.c
│   └── test_index_queue.c
├── bench/
│   └── endfields_bench.c
└── .github/workflows/ci.yml
```

---

## Where to go next

1. **Just want to use the API**: the **30-second quickstart** above, plus
   the headers [`src/endfields.h`](src/endfields.h),
   [`src/ef_index.h`](src/ef_index.h),
   [`src/ef_txn.h`](src/ef_txn.h).
2. **Care about concurrency**: [`THREADING.md`](THREADING.md).
3. **Want to understand the code organisation**: [`PROJECT_INDEX.md`](PROJECT_INDEX.md).
4. **Need to modify `superblock.reserved[]`**: only touch
   [`src/ef_sb_layout.h`](src/ef_sb_layout.h) / `.c`, and add tests
   for every migration path (v3→v4, v4→v5, embedded init).
5. **Touching a concurrent path**: locally run ASan + UBSan and TSan
   configurations; add a concurrent variant for any new feature test.

---

## License

[MIT License](LICENSE) — Copyright (c) 2026 zayoka
