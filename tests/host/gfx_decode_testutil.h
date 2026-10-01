/* Shared helpers for the JPEG and GIF decoder tests (M12.7): a failing/counting allocator, the
 * fixture-container loader, a deterministic PRNG. Kept separate from gfx_image_test.c on purpose
 * (that file's helpers are static and its fixture format is different). */
#ifndef HOST_GFX_DECODE_TESTUTIL_H
#define HOST_GFX_DECODE_TESTUTIL_H

#include "gfx/gfx.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int failAt; /* fail allocation number failAt (0-based); -1 = never */
    int count, live;
    size_t liveBytes, peakBytes;
} DecCountAlloc;

/* Points `a` at `s` (reset to zero, failing allocation number `failAt`). */
void decAllocInit(DecCountAlloc *s, GfxAllocator *a, int failAt);

/* Container: u32 LE rawLen, then a zlib stream of rawLen bytes. On success *raw is malloc'd
 * (caller frees). */
bool decLoadContainer(const char *path, uint8_t **raw, size_t *rawLen);

uint32_t decLe32(const uint8_t *p);
uint32_t decLe16(const uint8_t *p);
uint32_t decRng(uint32_t *state); /* xorshift32 */
bool decPremulOk(uint32_t argb);

#endif
