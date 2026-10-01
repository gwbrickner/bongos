/* vmalloc (ARCHITECTURE §6.2 item 5, D-092/D-097, ROADMAP M2.4): eager, page-granular kernel
 * allocations with guard pages, for anything over KMALLOC_MAX_SIZE (kmalloc.h). Built on M2.3's
 * vmmMapKernel()/the KVA allocator -- the guard pages come from vmmKvaAlloc() itself. Never called
 * from kernel/mm/pmm.c or vmm.c (kernel/mm/README.md's no-recursion rule). */
#ifndef KERNEL_VMALLOC_H
#define KERNEL_VMALLOC_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t VmallocFlags;
#define VMALLOC_ZERO        (1u << 0) /* zero-fill every page before returning */
#define VMALLOC_FLAGS_VALID VMALLOC_ZERO
#define VMALLOC_MAX_SIZE    (1ULL << 30) /* 1 GiB */

/* Creates the internal area-tracking cache. Boot-time only, BSP, IF=0; called once from
 * kernelMain right after slabInit(). Must run before any vmalloc()/vfree() call. */
void vmallocInit(void);

/* Allocates `size` bytes (1..VMALLOC_MAX_SIZE), rounded up to a whole number of 4 KiB pages, each
 * backed by its own pmm frame and mapped RW/NX/WB in the kernel virtual area, with an unmapped
 * guard page on each side of the whole range (vmmKvaAlloc()'s own guarantee). Never demand-
 * faulted: every page is allocated and mapped before this returns. Not zeroed unless
 * VMALLOC_ZERO. Returns NULL for "no memory" (pmm exhaustion, KVA exhaustion, or a page-table
 * allocation failure partway through -- every partial mapping is unwound first) or if `size` is
 * over VMALLOC_MAX_SIZE; size 0 or an unknown flag panics via panicBug(). No locks required of
 * the caller; never sleeps. Must not be called from IRQ context or with IRQs already disabled
 * (load-bearing once M3.5 adds a real TLB shootdown to vfree() -- today's single-CPU/IF=0 kernel
 * can't yet violate this itself, but no caller should rely on that). The one sanctioned exception
 * is boot-time code on the BSP before any AP exists, where vfree()'s local INVLPG is already a full
 * shootdown (acpiInit(), D-170); it must still never be called from IRQ context. */
void *vmalloc(size_t size, VmallocFlags flags);

/* Frees a pointer vmalloc() returned. NULL is a no-op. Any other misuse (a pointer this subsystem
 * never handed out, an interior pointer) panics via panicBug() -- as does a double free caught
 * before the freed VA range is handed to a new vmalloc() call; first-fit reuse means a double
 * free of an already-reused VA instead corrupts that new, unrelated allocation, the same
 * inherent risk any VA-keyed free carries. Same contract as vmalloc() otherwise. */
void vfree(void *ptr);

typedef struct {
    uint64_t areas, pages; /* live vmalloc() allocations, and their total page count */
} VmallocStats;
/* No locks required of the caller; IRQ-safe; never sleeps. */
void vmallocGetStats(VmallocStats *out);

typedef enum {
    VMALLOC_BUG_NONE = 0,
    VMALLOC_BUG_BAD_POINTER, /* not page-aligned, or outside the kernel virtual area */
    VMALLOC_BUG_NOT_MAPPED,  /* not currently mapped (already freed, or never vmalloc()'d) */
    VMALLOC_BUG_NOT_VMALLOC, /* mapped, but not a vmalloc() page (e.g. the framebuffer) */
    VMALLOC_BUG_NOT_HEAD,    /* an interior page of a live area, not its first page */
    VMALLOC_BUG_CORRUPT,     /* bad magic, or a page claiming a different area than its neighbors */
    VMALLOC_BUG_BAD_SIZE,    /* vmalloc(0) */
    VMALLOC_BUG_BAD_FLAGS    /* an unknown flag */
} VmallocBugKind;

/* The kind of the most recent vmalloc bug, for ktests via archTrapCatch(TRAP_CATCH_KERNEL_BUG).
 * Clears to VMALLOC_BUG_NONE on read, same reasoning as kmalloc.h's slabTakeLastBug(). No locks
 * required of the caller; IRQ-safe; never sleeps. */
VmallocBugKind vmallocTakeLastBug(void);

#endif
