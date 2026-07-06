#ifndef EF_ATOMIC_UNALIGNED_H
#define EF_ATOMIC_UNALIGNED_H

#include <stdint.h>
#include <string.h>

static inline int ef_atomic_ptr_is_aligned(const volatile void *ptr, size_t alignment)
{
    return ((uintptr_t)ptr & (uintptr_t)(alignment - 1U)) == 0U;
}

#if defined(__GNUC__) || defined(__clang__)
#if defined(__SANITIZE_THREAD__)
#define ef_atomic_thread_fence(order) __atomic_signal_fence(order)
#else
#define ef_atomic_thread_fence(order) __atomic_thread_fence(order)
#endif
#define EF_ATOMIC_THREAD_FENCE() ef_atomic_thread_fence(__ATOMIC_SEQ_CST)

static inline uint64_t ef_atomic_load_u64(const volatile void *ptr)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint64_t))) {
        return __atomic_load_n((const volatile uint64_t *)ptr, __ATOMIC_ACQUIRE);
    }

    {
        uint64_t value;

        memcpy(&value, (const void *)ptr, sizeof(value));
        ef_atomic_thread_fence(__ATOMIC_ACQUIRE);
        return value;
    }
}

static inline void ef_atomic_store_u64(volatile void *ptr, uint64_t value)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint64_t))) {
        __atomic_store_n((volatile uint64_t *)ptr, value, __ATOMIC_RELEASE);
        return;
    }

    ef_atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy((void *)ptr, (const void *)&value, sizeof(value));
}

static inline int ef_atomic_cas_u64(volatile void *ptr, uint64_t *expected, uint64_t desired)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint64_t))) {
        uint64_t exp = *expected;

        if (__atomic_compare_exchange_n((volatile uint64_t *)ptr, &exp, desired, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            return 1;
        }
        *expected = exp;
        return 0;
    }

    {
        uint64_t cur;

        for (;;) {
            memcpy(&cur, (const void *)ptr, sizeof(cur));
            if (cur != *expected) {
                *expected = cur;
                return 0;
            }
            memcpy((void *)ptr, (const void *)&desired, sizeof(desired));
            ef_atomic_thread_fence(__ATOMIC_SEQ_CST);
            memcpy(&cur, (const void *)ptr, sizeof(cur));
            if (cur == desired) {
                return 1;
            }
            *expected = cur;
        }
    }
}

static inline void ef_atomic_store_u32(volatile void *ptr, uint32_t value)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint32_t))) {
        __atomic_store_n((volatile uint32_t *)ptr, value, __ATOMIC_RELEASE);
        return;
    }

    ef_atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy((void *)ptr, (const void *)&value, sizeof(value));
}

static inline uint32_t ef_atomic_load_u32(const volatile void *ptr)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint32_t))) {
        return __atomic_load_n((const volatile uint32_t *)ptr, __ATOMIC_ACQUIRE);
    }

    {
        uint32_t value;

        memcpy(&value, (const void *)ptr, sizeof(value));
        ef_atomic_thread_fence(__ATOMIC_ACQUIRE);
        return value;
    }
}

static inline int ef_atomic_cas_u32(volatile void *ptr, uint32_t *expected, uint32_t desired)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint32_t))) {
        uint32_t exp = *expected;

        if (__atomic_compare_exchange_n((volatile uint32_t *)ptr, &exp, desired, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            return 1;
        }
        *expected = exp;
        return 0;
    }

    {
        uint32_t cur;

        for (;;) {
            memcpy(&cur, (const void *)ptr, sizeof(cur));
            if (cur != *expected) {
                *expected = cur;
                return 0;
            }
            memcpy((void *)ptr, (const void *)&desired, sizeof(desired));
            ef_atomic_thread_fence(__ATOMIC_SEQ_CST);
            memcpy(&cur, (const void *)ptr, sizeof(cur));
            if (cur == desired) {
                return 1;
            }
            *expected = cur;
        }
    }
}
#elif defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>

#define EF_ATOMIC_THREAD_FENCE() MemoryBarrier()

static inline uint64_t ef_atomic_load_u64(const volatile void *ptr)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint64_t))) {
        uint64_t value = *(const volatile uint64_t *)ptr;

        MemoryBarrier();
        return value;
    }

    {
        uint64_t value;

        memcpy(&value, (const void *)ptr, sizeof(value));
        MemoryBarrier();
        return value;
    }
}

static inline void ef_atomic_store_u64(volatile void *ptr, uint64_t value)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint64_t))) {
        *(volatile uint64_t *)ptr = value;
        MemoryBarrier();
        return;
    }

    MemoryBarrier();
    memcpy((void *)ptr, (const void *)&value, sizeof(value));
}

static inline int ef_atomic_cas_u64(volatile void *ptr, uint64_t *expected, uint64_t desired)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint64_t))) {
        volatile __int64 *word = (volatile __int64 *)ptr;
        __int64 exp = (__int64)*expected;
        __int64 prev = _InterlockedCompareExchange64(word, (__int64)desired, exp);

        if (prev == exp) {
            return 1;
        }
        *expected = (uint64_t)prev;
        return 0;
    }

    {
        uint64_t cur;

        for (;;) {
            memcpy(&cur, (const void *)ptr, sizeof(cur));
            if (cur != *expected) {
                *expected = cur;
                return 0;
            }
            memcpy((void *)ptr, (const void *)&desired, sizeof(desired));
            MemoryBarrier();
            memcpy(&cur, (const void *)ptr, sizeof(cur));
            if (cur == desired) {
                return 1;
            }
            *expected = cur;
        }
    }
}

static inline void ef_atomic_store_u32(volatile void *ptr, uint32_t value)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint32_t))) {
        *(volatile uint32_t *)ptr = value;
        MemoryBarrier();
        return;
    }

    MemoryBarrier();
    memcpy((void *)ptr, (const void *)&value, sizeof(value));
}

static inline uint32_t ef_atomic_load_u32(const volatile void *ptr)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint32_t))) {
        uint32_t value = *(const volatile uint32_t *)ptr;

        MemoryBarrier();
        return value;
    }

    {
        uint32_t value;

        memcpy(&value, (const void *)ptr, sizeof(value));
        MemoryBarrier();
        return value;
    }
}

static inline int ef_atomic_cas_u32(volatile void *ptr, uint32_t *expected, uint32_t desired)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint32_t))) {
        volatile long *word = (volatile long *)ptr;
        long exp = (long)*expected;
        long prev = _InterlockedCompareExchange(word, (long)desired, exp);

        if (prev == exp) {
            return 1;
        }
        *expected = (uint32_t)prev;
        return 0;
    }

    {
        uint32_t cur;

        for (;;) {
            memcpy(&cur, (const void *)ptr, sizeof(cur));
            if (cur != *expected) {
                *expected = cur;
                return 0;
            }
            memcpy((void *)ptr, (const void *)&desired, sizeof(desired));
            MemoryBarrier();
            memcpy(&cur, (const void *)ptr, sizeof(cur));
            if (cur == desired) {
                return 1;
            }
            *expected = cur;
        }
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

static inline uint32_t ef_atomic_load_u32(const volatile void *ptr)
{
    return *(const volatile uint32_t *)ptr;
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

#if defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
static inline uint32_t ef_atomic_fetch_add_u32(volatile void *ptr, uint32_t delta)
{
    if (ef_atomic_ptr_is_aligned(ptr, sizeof(uint32_t))) {
#if defined(__GNUC__) || defined(__clang__)
        return (uint32_t)__atomic_fetch_add((volatile uint32_t *)ptr, delta, __ATOMIC_RELAXED);
#else
        return (uint32_t)_InterlockedExchangeAdd((volatile long *)ptr, (long)delta);
#endif
    }

    {
        uint32_t prev;

        for (;;) {
            uint32_t next;

            memcpy(&prev, (const void *)ptr, sizeof(prev));
            next = prev + delta;
            if (ef_atomic_cas_u32(ptr, &prev, next)) {
                return prev;
            }
        }
    }
}

static inline uint32_t ef_atomic_fetch_sub_u32(volatile void *ptr, uint32_t delta)
{
    return ef_atomic_fetch_add_u32(ptr, (uint32_t)(0U - delta));
}

static inline uint8_t ef_atomic_load_u8(const volatile void *ptr)
{
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t align_off = addr & (uintptr_t)3U;
    const volatile void *word_ptr = (const volatile void *)(addr - align_off);
    uint32_t word = ef_atomic_load_u32(word_ptr);

    return (uint8_t)((word >> (align_off * 8U)) & 0xFFU);
}

static inline void ef_atomic_store_u8(volatile void *ptr, uint8_t value)
{
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t align_off = addr & (uintptr_t)3U;
    volatile void *word_ptr = (volatile void *)(addr - align_off);
    uint32_t exp_word;
    uint32_t des_word;

    exp_word = ef_atomic_load_u32((const void *)word_ptr);
    for (;;) {
        des_word = (exp_word & ~((uint32_t)0xFFU << (align_off * 8U))) |
                   ((uint32_t)value << (align_off * 8U));
        if (ef_atomic_cas_u32(word_ptr, &exp_word, des_word)) {
            return;
        }
    }
}
#endif

#endif /* EF_ATOMIC_UNALIGNED_H */
