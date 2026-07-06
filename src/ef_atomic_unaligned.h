#ifndef EF_ATOMIC_UNALIGNED_H
#define EF_ATOMIC_UNALIGNED_H

#include <stdint.h>
#include <string.h>

#if defined(__GNUC__) || defined(__clang__)
#define EF_ATOMIC_THREAD_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)

static inline uint64_t ef_atomic_load_u64(const volatile void *ptr)
{
    uint64_t value;

    memcpy(&value, (const void *)ptr, sizeof(value));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return value;
}

static inline void ef_atomic_store_u64(volatile void *ptr, uint64_t value)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy((void *)ptr, (const void *)&value, sizeof(value));
}

static inline int ef_atomic_cas_u64(volatile void *ptr, uint64_t *expected, uint64_t desired)
{
    uint64_t cur;

    for (;;) {
        memcpy(&cur, (const void *)ptr, sizeof(cur));
        if (cur != *expected) {
            *expected = cur;
            return 0;
        }
        memcpy((void *)ptr, (const void *)&desired, sizeof(desired));
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        memcpy(&cur, (const void *)ptr, sizeof(cur));
        if (cur == desired) {
            return 1;
        }
        *expected = cur;
    }
}

static inline void ef_atomic_store_u32(volatile void *ptr, uint32_t value)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy((void *)ptr, (const void *)&value, sizeof(value));
}

static inline int ef_atomic_cas_u32(volatile void *ptr, uint32_t *expected, uint32_t desired)
{
    uint32_t cur;

    for (;;) {
        memcpy(&cur, (const void *)ptr, sizeof(cur));
        if (cur != *expected) {
            *expected = cur;
            return 0;
        }
        memcpy((void *)ptr, (const void *)&desired, sizeof(desired));
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        memcpy(&cur, (const void *)ptr, sizeof(cur));
        if (cur == desired) {
            return 1;
        }
        *expected = cur;
    }
}
#else
#define EF_ATOMIC_THREAD_FENCE() ((void)0)

static inline uint64_t ef_atomic_load_u64(const volatile void *ptr)
{
    return *(const volatile uint64_t *)ptr;
}

static inline void ef_atomic_store_u64(volatile void *ptr, uint64_t value)
{
    *(volatile uint64_t *)ptr = value;
}

static inline int ef_atomic_cas_u64(volatile void *ptr, uint64_t *expected, uint64_t desired)
{
    volatile uint64_t *p = (volatile uint64_t *)ptr;

    if (*p != *expected) {
        *expected = *p;
        return 0;
    }
    *p = desired;
    return 1;
}

static inline void ef_atomic_store_u32(volatile void *ptr, uint32_t value)
{
    *(volatile uint32_t *)ptr = value;
}

static inline int ef_atomic_cas_u32(volatile void *ptr, uint32_t *expected, uint32_t desired)
{
    volatile uint32_t *p = (volatile uint32_t *)ptr;

    if (*p != *expected) {
        *expected = *p;
        return 0;
    }
    *p = desired;
    return 1;
}
#endif

#endif /* EF_ATOMIC_UNALIGNED_H */
