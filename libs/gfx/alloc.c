/* See gfx.h. The only libs/gfx file that includes <stdlib.h>. */
#include <stdlib.h>

#include "gfx/gfx.h"

static void *defaultAlloc(void *ctx, size_t size) {
    (void)ctx;
    return malloc(size != 0 ? size : 1);
}

static void defaultFree(void *ctx, void *ptr, size_t size) {
    (void)ctx;
    (void)size;
    free(ptr);
}

static const GfxAllocator DEFAULT_ALLOCATOR = {defaultAlloc, defaultFree, NULL};

const GfxAllocator *gfxAllocatorDefault(void) {
    return &DEFAULT_ALLOCATOR;
}
