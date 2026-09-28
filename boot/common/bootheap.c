/* See bootheap.h. */
#include "include/bootheap.h"

#include "include/bootmem.h"

BootStatus bootHeapInit(BootHeap *heap, const BootMemRegion *regions, uint32_t count) {
    heap->count = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (regions[i].type != BOOT_MEM_USABLE) {
            continue;
        }
        uint64_t base = regions[i].base;
        uint64_t end = base + regions[i].length;
        if (end < base) {
            continue; /* overflow: skip */
        }
        if (base < BOOT_HEAP_MIN) {
            base = BOOT_HEAP_MIN;
        }
        if (end > BOOT_HEAP_MAX) {
            end = BOOT_HEAP_MAX;
        }
        base = bootAlignUp(base, BOOT_HEAP_PAGE_SIZE);
        end = bootAlignDown(end, BOOT_HEAP_PAGE_SIZE);
        if (end <= base) {
            continue; /* nothing left after clipping */
        }
        if (heap->count >= BOOT_HEAP_MAX_REGIONS) {
            return BOOT_ERR_MEMMAP_CAPACITY;
        }
        heap->regions[heap->count].base = base;
        heap->regions[heap->count].end = end;
        heap->count++;
    }
    return BOOT_OK;
}

BootStatus bootHeapAllocPages(BootHeap *heap, uint32_t pages, uint64_t *outPhys) {
    uint64_t need = (uint64_t)pages * BOOT_HEAP_PAGE_SIZE;
    for (uint32_t i = 0; i < heap->count; i++) {
        BootHeapRegion *r = &heap->regions[i];
        if (r->end - r->base >= need) {
            *outPhys = r->base;
            r->base += need;
            return BOOT_OK;
        }
    }
    return BOOT_ERR_NO_MEMORY;
}
