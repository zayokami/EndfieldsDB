#ifndef EF_CONFIG_H
#define EF_CONFIG_H

/* Schema version 2 adds superblock and slot header CRC. */
#define EF_SCHEMA_VERSION 2U

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

/* Likely/unlikely branch hints. */
#if defined(__GNUC__) || defined(__clang__)
#define EF_LIKELY(x)   __builtin_expect(!!(x), 1)
#define EF_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define EF_LIKELY(x)   (x)
#define EF_UNLIKELY(x) (x)
#endif

#endif
