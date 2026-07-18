# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Endfields DB is a C11 embedded database core built around fixed 64-byte slots. It uses memory-mapped I/O (POSIX/Win32) or a pure RAM buffer as the backend, and addresses data by physical file offset rather than pointers. Key subsystems are a persistent LIFO free-list allocator, a cross-process FIFO MPMC queue, a Robin Hood string hash index, and deferred superblock CRC commits.

Current schema version is **v4** (`EF_SCHEMA_VERSION 4` in `src/ef_config.h`). v4 adds multi-reader/single-writer seqlock protection to the hash index by packing `hash_capacity`, `queue_lock`, `index_write_lock`, and `index_seq` into the 28-byte `superblock.reserved` region.

## Common commands

### Build and test (host)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Windows with MinGW:

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Run a single test executable

Tests are split into separate binaries (see `CMakeLists.txt`). To run just one:

```bash
./build/endfields_core_test
./build/endfields_index_queue_test
./build/endfields_bench
./build/endfields_embedded_test
```

Use `ctest -R <name>` for CTest-level filtering:

```bash
ctest --test-dir build -R endfields_core --output-on-failure
```

### Strict CI build locally

CI uses `-Werror` and short bench loops. Reproduce with:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DENDFIELDS_WARNINGS_AS_ERRORS=ON -DENDFIELDS_CI_FAST=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 900 --no-tests=error
```

### Sanitizers / coverage

These require GCC or Clang and are mutually exclusive with each other except as noted:

```bash
# ASan + UBSan
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENDFIELDS_SANITIZE=ON \
  -DENDFIELDS_WARNINGS_AS_ERRORS=ON -DENDFIELDS_CI_FAST=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# ThreadSanitizer
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENDFIELDS_TSAN=ON \
  -DENDFIELDS_WARNINGS_AS_ERRORS=ON -DENDFIELDS_CI_FAST=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Coverage
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENDFIELDS_COVERAGE=ON \
  -DENDFIELDS_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
lcov --directory build --capture --output-file coverage.info
```

### Embedded-only build

```bash
cmake -B build-embedded -DENDFIELDS_EMBEDDED_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-embedded --parallel
ctest --test-dir build-embedded --output-on-failure
```

### Static analysis

CI runs `clang-tidy` and `cppcheck` over `src/`. Generate `compile_commands.json` first:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENDFIELDS_WARNINGS_AS_ERRORS=ON
clang-tidy src/*.c -p build --quiet
cppcheck --inline-suppr --quiet --error-exitcode=1 \
  --enable=warning,performance,portability -I src --suppress=missingIncludeSystem src/
```

The `.clang-tidy` config enables `clang-analyzer-*`, `concurrency-*`, and `misc-definitions-in-headers`, and suppresses the insecure-buffer-handling warning.

## High-level architecture

### Physical layout and addressing

All data is stored in 64-byte aligned units. Two file layouts exist:

- **With hash index** (`hash_capacity > 0`): `[superblock 64B][hash index capacity × 16B][slots max_slots × 64B]`
- **Without hash index** (`hash_capacity = 0`): `[superblock 64B][slots max_slots × 64B]`

Slots are accessed by physical offset, not pointer. `ef_slot_to_offset` / `ef_offset_to_slot_id` convert between slot IDs and offsets. `ef_offset_to_ptr` maps an offset into the current mmap/buffer view. The `slots_base` field in `struct ef_db` tracks where the slot region begins; it changes during `ef_index_rehash` because rehash moves the slot region to make room for a larger hash index.

### Slot lifecycle and allocation

- `EF_STATUS_FREE`, `EF_STATUS_USED`, `EF_STATUS_OVERFLOW`, `EF_STATUS_QUEUED`, `EF_STATUS_QUEUE_DUMMY`, and transitional `EF_STATUS_QUEUE_LINK` / `EF_STATUS_QUEUE_DEQ` statuses are defined in `src/endfields.h`.
- The free list is a persistent LIFO linked list using `next_offset`, rooted in `superblock.free_list_head`.
- `ef_alloc_slot` pops from the free list (CAS on GCC/Clang). `ef_alloc` grows the file/buffer by one slot when the pool is empty.
- `ef_free_slot` removes any index entry pointing to the slot (`ef_index_remove_by_slot`) and pushes it back onto the free list.
- `ef_return_slot_to_pool` is the internal primitive used by queue pop/push failure paths; it does not touch the index.
- **`ef_write_payload`, `ef_write_blob`, and `ef_set_status(..., EF_STATUS_USED)` do NOT implicitly claim FREE slots**. Callers must allocate a slot first via `ef_alloc*` and then write to it.

### Hash index

Implemented in `src/ef_index.c`:

- Keys are hashed with FNV-1a; only the hash is stored, not the original key string.
- Robin Hood linear probing with a max capacity of `0x8000` and automatic rehash when load factor exceeds 3/4.
- v4 readers use a seqlock (`index_seq` in `superblock.reserved`); writers acquire `index_write_lock`.
- `ef_index_get_slot` atomically resolves a key to a slot and copies the payload under one seqlock window, avoiding the TOCTOU gap between `ef_index_get` and `ef_get_slot`.
- `ef_index_rehash` moves the slot region, so it must update every physical offset stored in the superblock (free list, queue head/tail) and inside slots (`next_offset`) before reinserting entries.

### FIFO queue

Implemented in `src/endfields.c`:

- Dummy-head list; head, tail, and a single superblock spinlock live in `superblock.reserved`.
- Supports multi-producer/multi-consumer across threads and processes.
- `ef_queue_empty` is a lock-free heuristic only. For consumer shutdown after producers finish, loop on `ef_queue_pop` and confirm with `ef_queue_drained` under the lock.
- High contention can return `EF_ERR_QUEUE_BUSY`; callers must retry.

### CRC and deferred superblock commit

- Slot headers carry a CRC for `USED`, `OVERFLOW`, and queue-related states. `ef_get_slot` validates it; `ef_peek_slot` does not and is meant for hot paths where invariants are already held.
- The superblock CRC is computed lazily: `ef_db_mark_meta_dirty` sets `sb_meta_dirty`, and `ef_db_commit_meta` / `ef_sync` / `ef_close` flush it.
- CRC32 has a portable slicing-by-4 path (`src/ef_crc.c`) and an x86/x64 PCLMULQDQ fast path (`src/ef_crc_pclmul.c`).

### Backend abstraction

`src/ef_port.c` hides POSIX (`mmap`/`msync`) and Win32 (`CreateFileMapping`/`MapViewOfFile`/`FlushViewOfFile`) file mapping. The pure-memory backend is always available and is selected at compile time with `EF_PLATFORM_EMBEDDED` / `EF_NO_FILE_IO`, or at runtime via `ef_open_memory*`.

### Instruction executor

`ef_execute` in `src/endfields.c` dispatches a 10-byte `struct ef_cmd` to perform get-slot, chase, field read/write, payload write, set next/status, alloc/free, and chase-N operations. It is the low-level command interface used by tests and benchmarks.

## Critical cross-file constraints

When modifying code, keep these invariants in mind:

- **`superblock.reserved[28]` layout** is version-sensitive and centralized in `src/ef_sb_layout.h` and `src/ef_sb_layout.c`. Do not hardcode offsets; use the helpers there. v3→v4 migration happens on writable open in `ef_sb_migrate_v3_index_layout`.
- **`ef_index_rehash` moves slots**. Any code holding slot offsets computed before a rehash must recompute them afterwards. This affects `free_list_head`, queue head/tail, blob chains, and hash entries.
- **Queue pop must return slots inside the critical section**. `ef_queue_pop` calls `ef_return_slot_to_pool` while still holding `queue_lock` so the slot cannot be reused while still reachable from the queue.
- **Index writes serialize through `index_write_lock`**, but multiple threads may attempt concurrently. Any function that can fail with `EF_ERR_INDEX_BUSY` (including `ef_index_remove_by_slot`, which is called by `ef_free_slot` and queue paths) must be retry-safe.
- **`ef_sync` / `ef_close` must be externally serialized** with other writers.
- **Slot payload/blob/chase APIs are not thread-safe**. The index and queue protect their own metadata, but callers must synchronize slot data access.
- **Schema migration**: `ef_needs_upgrade` / `ef_upgrade` upgrade v1/v2/v3 files to v4. Read-only opens do not migrate.

## Source file map

| File | Purpose |
|------|---------|
| `src/endfields.h` | Public API, `ef_slot`, `ef_superblock`, `ef_db`, error enums, opcodes |
| `src/endfields.c` | Open/close, layout binding, slot access, allocation/grow, blob, queue, executor, CRC commit |
| `src/ef_index.h` / `src/ef_index.c` | Robin Hood index, rehash, v4 seqlock/lock |
| `src/ef_sb_layout.h` / `src/ef_sb_layout.c` | Superblock `reserved[]` field layout and v3→v4 migration |
| `src/ef_port.h` / `src/ef_port.c` | File/memory mapping backend abstraction |
| `src/ef_crc.h` / `src/ef_crc.c` / `src/ef_crc_pclmul.c` | CRC32 implementations |
| `src/ef_atomic_unaligned.h` | Atomic helpers for unaligned mmap fields; GCC/Clang/MSVC are lock-free, unknown compilers use a mutex fallback |
| `src/ef_config.h` | Schema version, prefetch, atomics, platform switches |
| `tests/test_core.c` | Core, file, memory, CRC, blob tests |
| `tests/test_index_queue.c` | Index, queue, concurrency, migration tests |
| `tests/test_common.h` / `tests/test_common.c` | Shared test helpers |
| `bench/endfields_bench.c` | Performance and scenario benchmarks |
| `src/main_embedded.c` | Embedded-only RAM test entry |

## CI reference

`.github/workflows/ci.yml` gates merges with jobs for Linux GCC (Release, Debug, no-prefetch, ASan+UBSan, TSan), Linux Clang, macOS Clang, Windows MSVC, Windows MinGW (Release/Debug), coverage, static analysis, and embedded-only. The `CI gate` job requires all others to succeed.
