/* Shared helpers for the libs/gfx font-engine host tests (M12.3, D-158): file loading, a counting
 * and failure-injecting allocator, an oracle-text reader and small sfnt patching helpers. Test-only
 * (POSIX/stdio allowed). */
#ifndef TESTS_HOST_FONT_TESTUTIL_H
#define TESTS_HOST_FONT_TESTUTIL_H

#include <stddef.h>
#include <stdint.h>

#include "gfx/gfx.h"

/* A malloc'd copy of the file, or NULL. Paths are relative to the repository root. */
uint8_t *ftuLoad(const char *path, size_t *size);

/* The fixtures under tests/data/font/ and the shipped fonts (loaded once, never freed). */
enum {
    FTU_SYNTH_FALLBACK,
    FTU_SYNTH_GPOS,
    FTU_SYNTH_GRID,
    FTU_SANS,
    FTU_MONO,
    FTU_SYNTH_SYMBOL,
    FTU_SYNTH_BAD,
    FTU_COUNT
};
const uint8_t *ftuFont(int which, size_t *size);

/* An allocator that can fail the Nth allocation (failAt, 0-based; -1 = never) and tracks live
 * blocks and bytes. `live` must be 0 after everything is torn down. */
typedef struct {
    int failAt;
    int count, live;
    size_t liveBytes, peakBytes;
} FtuAlloc;
void ftuAllocInit(FtuAlloc *a, GfxAllocator *out, int failAt);

/* The oracle text files (the .oracle files and utf8.cases under tests/data/font): NUL-terminated,
 * loaded once. */
const char *ftuOracle(const char *name);
/* Copies the next line at *pos into line (cap bytes); false at the end. */
int ftuNextLine(const char *text, size_t *pos, char *line, size_t cap);

/* sfnt helpers: absolute offset of table `tag` (0 if absent) and its length. */
uint32_t ftuTable(const uint8_t *d, size_t n, const char *tag, uint32_t *len);
void ftuPut16(uint8_t *d, uint32_t off, uint32_t v);
void ftuPut32(uint8_t *d, uint32_t off, uint32_t v);
uint32_t ftuGet16(const uint8_t *d, uint32_t off);
uint32_t ftuGet32(const uint8_t *d, uint32_t off);

/* A table override for ftuRebuild: `data == NULL` removes the table. */
typedef struct {
    const char *tag;
    const uint8_t *data;
    uint32_t len;
} FtuTable;
/* Rebuilds the sfnt `src` with the tables in `ov` replaced, added or removed. The overrides are
 * laid out after every other table, in `ov` order, and the file ends exactly at the last byte of
 * the last one (so ASan sees any read past it). Returns a malloc'd buffer. */
uint8_t *ftuRebuild(const uint8_t *src, const FtuTable *ov, int nOv, size_t *outN);

#endif
