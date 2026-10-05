/*
 * atomics.h - Minimal portable atomics used to share state between the UI
 * thread and the real-time audio callbacks.
 *
 * GCC and Clang (including MinGW) use the __atomic builtins, MSVC uses the
 * Interlocked intrinsics. Only what we actually need is provided: 32-bit
 * integers and 64-bit unsigned counters.
 */
#ifndef CWTTY_ATOMICS_H
#define CWTTY_ATOMICS_H

#include <stdint.h>

#if defined(_MSC_VER) && !defined(__clang__)

#include <intrin.h>

typedef volatile long    at_int;
typedef volatile __int64 at_u64;

static __inline int at_get(at_int *p) { return (int)_InterlockedOr(p, 0); }
static __inline void at_set(at_int *p, int v) { _InterlockedExchange(p, (long)v); }
static __inline int at_add(at_int *p, int v) { return (int)_InterlockedExchangeAdd(p, (long)v) + v; }

static __inline uint64_t at_get64(at_u64 *p)
{
    return (uint64_t)_InterlockedCompareExchange64(p, 0, 0);
}

static __inline void at_set64(at_u64 *p, uint64_t v)
{
    __int64 old;
    do {
        old = *p;
    } while (_InterlockedCompareExchange64(p, (__int64)v, old) != old);
}

#else

typedef volatile int32_t  at_int;
typedef volatile uint64_t at_u64;

static inline int at_get(at_int *p) { return (int)__atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void at_set(at_int *p, int v) { __atomic_store_n(p, (int32_t)v, __ATOMIC_RELEASE); }
static inline int at_add(at_int *p, int v) { return (int)__atomic_add_fetch(p, (int32_t)v, __ATOMIC_ACQ_REL); }
static inline uint64_t at_get64(at_u64 *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void at_set64(at_u64 *p, uint64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

#endif

#endif
