/* The kernel virtual area allocator's pure core (D-088, ROADMAP M2.3): a sorted, coalescing list
 * of free `[start, end)` extents, host-tested directly by tests/host/kernel_kva_test.c. No
 * allocation, no locks, no arch calls -- kernel/mm/vmm.c owns the one real instance (VM_KVA_BASE..
 * VM_KVA_END) and the vmmLock around it. */
#ifndef KERNEL_MM_KVA_INTERNAL_H
#define KERNEL_MM_KVA_INTERNAL_H

#include "uapi/status.h"

#include <stdint.h>

#define KVA_MAX_EXTENTS 512
#define KVA_PAGE_SIZE   4096ULL
#define KVA_GUARD_SIZE  4096ULL /* one unmapped guard page on each side of every allocation */

typedef struct {
    uint64_t start, end; /* [start, end): free, page-aligned */
} KvaExtent;

typedef struct {
    KvaExtent extents[KVA_MAX_EXTENTS]; /* sorted ascending by start; non-overlapping, non-
                                         * touching (touching neighbors are always merged) */
    uint32_t count;
} KvaState;

/* Resets `st` to one free extent covering [base, end). Requires base < end, both 4 KiB-aligned. No
 * locks; pure. */
void kvaStateInit(KvaState *st, uint64_t base, uint64_t end);

/* First-fit allocation of `size` (nonzero, 4 KiB-aligned) bytes plus a KVA_GUARD_SIZE guard page
 * on each side: reserves `size + 2*KVA_GUARD_SIZE` bytes from the lowest-addressed free extent
 * with enough room and returns the usable (post-guard) VA via `*outVa`. Returns
 * STATUS_ERR_NO_MEMORY if no free extent is big enough, STATUS_ERR_INVALID for a bad `size` or a
 * full extent table. No locks; pure. */
Status kvaAlloc(KvaState *st, uint64_t size, uint64_t *outVa);

/* Returns `[va - KVA_GUARD_SIZE, va + size + KVA_GUARD_SIZE)` to the free extents, coalescing with
 * any touching neighbor. `va`/`size` must be exactly what a prior kvaAlloc() call was given/
 * returned -- returns STATUS_ERR_INVALID if that range overlaps an already-free extent (a double
 * free) or falls outside [st's own bounds] entirely; the caller decides whether that's a panic.
 * No locks; pure. */
Status kvaFree(KvaState *st, uint64_t va, uint64_t size);

#endif
