/* See kva-internal.h. */
#include "kva-internal.h"

#include <stdbool.h>
#include <stddef.h>

void kvaStateInit(KvaState *st, uint64_t base, uint64_t end) {
    st->base = base;
    st->end = end;
    st->extents[0] = (KvaExtent){.start = base, .end = end};
    st->count = 1;
}

Status kvaAlloc(KvaState *st, uint64_t size, uint64_t *outVa) {
    if (outVa == NULL || size == 0 || (size % KVA_PAGE_SIZE) != 0) {
        return STATUS_ERR_INVALID;
    }
    uint64_t need = size + 2 * KVA_GUARD_SIZE;
    if (need < size) { /* overflow: no real KVA request can reach this on a 64-bit address space */
        return STATUS_ERR_INVALID;
    }

    for (uint32_t i = 0; i < st->count; i++) {
        uint64_t start = st->extents[i].start;
        if (st->extents[i].end - start < need) {
            continue;
        }
        uint64_t newStart = start + need;
        if (newStart == st->extents[i].end) {
            for (uint32_t j = i; j + 1 < st->count; j++) {
                st->extents[j] = st->extents[j + 1];
            }
            st->count--;
        } else {
            st->extents[i].start = newStart;
        }
        *outVa = start + KVA_GUARD_SIZE;
        return STATUS_OK;
    }
    return STATUS_ERR_NO_MEMORY;
}

Status kvaFree(KvaState *st, uint64_t va, uint64_t size) {
    if (size == 0 || (size % KVA_PAGE_SIZE) != 0 || va < KVA_GUARD_SIZE) {
        return STATUS_ERR_INVALID;
    }
    uint64_t start = va - KVA_GUARD_SIZE;
    uint64_t end = va + size + KVA_GUARD_SIZE;
    if (end < va) { /* overflow */
        return STATUS_ERR_INVALID;
    }
    if (start < st->base || end > st->end) {
        return STATUS_ERR_INVALID;
    }

    uint32_t idx = 0;
    while (idx < st->count && st->extents[idx].start < start) {
        idx++;
    }
    /* Sorted + non-overlapping means only the immediate neighbors can possibly overlap
     * [start, end) -- see kva-internal.h's contract comment. Any overlap here is a double free
     * (or freeing a range that was never allocated from this state). */
    if (idx > 0 && st->extents[idx - 1].end > start) {
        return STATUS_ERR_INVALID;
    }
    if (idx < st->count && st->extents[idx].start < end) {
        return STATUS_ERR_INVALID;
    }

    bool mergeLeft = idx > 0 && st->extents[idx - 1].end == start;
    bool mergeRight = idx < st->count && st->extents[idx].start == end;

    if (mergeLeft && mergeRight) {
        st->extents[idx - 1].end = st->extents[idx].end;
        for (uint32_t j = idx; j + 1 < st->count; j++) {
            st->extents[j] = st->extents[j + 1];
        }
        st->count--;
    } else if (mergeLeft) {
        st->extents[idx - 1].end = end;
    } else if (mergeRight) {
        st->extents[idx].start = start;
    } else {
        if (st->count >= KVA_MAX_EXTENTS) {
            return STATUS_ERR_INVALID;
        }
        for (uint32_t j = st->count; j > idx; j--) {
            st->extents[j] = st->extents[j - 1];
        }
        st->extents[idx] = (KvaExtent){.start = start, .end = end};
        st->count++;
    }
    return STATUS_OK;
}
