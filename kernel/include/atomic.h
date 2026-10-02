/* Atomic operation wrappers (M3.4, D-185): UPPER_SNAKE macros over the compiler's __atomic builtins
 * (the C11 memory model), applied to plain naturally aligned 1/2/4/8-byte integers and pointers
 * with the memory order spelled out at every call site. No `_Atomic` qualifiers, so shared structs
 * stay plain. No 16-byte atomics: without -mcx16 they lower to a libcall and the kernel has no
 * compiler-rt (D-104). Header-only, no locks, safe from any context. */
#ifndef KERNEL_ATOMIC_H
#define KERNEL_ATOMIC_H

#include <stdbool.h>

#define MEM_RELAXED __ATOMIC_RELAXED
#define MEM_ACQUIRE __ATOMIC_ACQUIRE
#define MEM_RELEASE __ATOMIC_RELEASE
#define MEM_ACQ_REL __ATOMIC_ACQ_REL
#define MEM_SEQ_CST __ATOMIC_SEQ_CST

#define ATOMIC_LOAD(p, mo)         __atomic_load_n((p), (mo))
#define ATOMIC_STORE(p, v, mo)     __atomic_store_n((p), (v), (mo))
#define ATOMIC_FETCH_ADD(p, v, mo) __atomic_fetch_add((p), (v), (mo))
#define ATOMIC_FETCH_SUB(p, v, mo) __atomic_fetch_sub((p), (v), (mo))
#define ATOMIC_FETCH_OR(p, v, mo)  __atomic_fetch_or((p), (v), (mo))
#define ATOMIC_FETCH_AND(p, v, mo) __atomic_fetch_and((p), (v), (mo))
#define ATOMIC_XCHG(p, v, mo)      __atomic_exchange_n((p), (v), (mo))
/* Strong compare-exchange: on failure `*expPtr` receives the observed value. Returns true if the
 * exchange happened. */
#define ATOMIC_CMPXCHG(p, expPtr, desired, moOk, moFail)                                           \
    __atomic_compare_exchange_n((p), (expPtr), (desired), false, (moOk), (moFail))
#define ATOMIC_FENCE(mo) __atomic_thread_fence(mo)
/* Compiler-only barrier (no instruction): keeps loads/stores from being reordered across it. */
#define COMPILER_BARRIER() __atomic_signal_fence(__ATOMIC_SEQ_CST)

#endif
