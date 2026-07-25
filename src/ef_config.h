#ifndef EF_CONFIG_H
#define EF_CONFIG_H

/* Schema v2: superblock + slot header CRC. v3: hash index + queue. v4: index seqlock MRSW.
 * v5: persistent undo log + independent txn lock for read-write transactions. */
#define EF_SCHEMA_VERSION 5U
#define EF_SCHEMA_VERSION_V4 4U
#define EF_SCHEMA_VERSION_V3 3U
#define EF_SCHEMA_VERSION_V2 2U

/* Default undo log record count for v5 transactions. Each record is 32B, so the
 * default log size is 128 KiB. Configurable via ef_open_ex_hash_with / ef_open_memory_hash_with
 * (undo_log_slots). The log lives in its own segment after the slot area and is
 * independent of the slot CRC region. */
#define EF_UNDO_LOG_DEFAULT_SLOTS 4096U

/* v5 undo log segment size = header (32B) + records (32B * slots). Tests
 * sizing arenas should add this constant to cover the persistent segment. */
#define EF_UNDO_LOG_BYTES(slots) \
    (32U + (uint64_t)(slots) * 32U)
#define EF_UNDO_LOG_DEFAULT_BYTES EF_UNDO_LOG_BYTES(EF_UNDO_LOG_DEFAULT_SLOTS)

/* Default Robin Hood table size for new databases (power-of-two friendly). */
#define EF_DEFAULT_HASH_MIN 16U

/* Default maximum pointer-chase depth (cycle guard). */
#define EF_CHASE_MAX_DEPTH 1024U

/* Platform selection (auto-detect unless overridden). */
#if defined(EF_PLATFORM_EMBEDDED)
#define EF_HAS_FILE_IO 0
#elif defined(EF_NO_FILE_IO)
#define EF_HAS_FILE_IO 0
#else
#define EF_HAS_FILE_IO 1
#endif

/* Optional CPU prefetch on chase hot path (GCC/Clang). */
#if defined(EF_ENABLE_PREFETCH) && (defined(__GNUC__) || defined(__clang__))
#define EF_PREFETCH_R(addr) __builtin_prefetch((addr), 0, 3)
#else
#define EF_PREFETCH_R(addr) ((void)0)
#endif

/* GCC/Clang __atomic_* or MSVC Interlocked/MemoryBarrier. */
#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
#define EF_HAS_HW_ATOMICS 1
#else
#define EF_HAS_HW_ATOMICS 0
#endif

/* Likely/unlikely branch hints. */
#if defined(__GNUC__) || defined(__clang__)
#define EF_LIKELY(x)   __builtin_expect(!!(x), 1)
#define EF_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define EF_LIKELY(x)   (x)
#define EF_UNLIKELY(x) (x)
#endif

#endif
