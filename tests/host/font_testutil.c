#include "font_testutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *ftuLoad(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n + 1);
    if (p == NULL || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    p[n] = 0;
    if (size != NULL) {
        *size = (size_t)n;
    }
    return p;
}

static const char *const fontPaths[FTU_COUNT] = {
    "tests/data/font/synth-fallback.ttf",    "tests/data/font/synth-gpos.ttf",
    "tests/data/font/synth-grid.ttf",        "data/fonts/LiberationSans-Regular.ttf",
    "data/fonts/LiberationMono-Regular.ttf", "tests/data/font/synth-symbol.ttf",
};

const uint8_t *ftuFont(int which, size_t *size) {
    static uint8_t *cache[FTU_COUNT];
    static size_t sizes[FTU_COUNT];
    if (cache[which] == NULL) {
        cache[which] = ftuLoad(fontPaths[which], &sizes[which]);
    }
    if (size != NULL) {
        *size = sizes[which];
    }
    return cache[which];
}

static void *fuAlloc(void *ctx, size_t n) {
    FtuAlloc *c = ctx;
    if (c->count++ == c->failAt) {
        return NULL;
    }
    void *p = malloc(n != 0 ? n : 1);
    if (p != NULL) {
        c->live++;
        c->liveBytes += n;
        if (c->liveBytes > c->peakBytes) {
            c->peakBytes = c->liveBytes;
        }
    }
    return p;
}

static void fuFree(void *ctx, void *p, size_t n) {
    FtuAlloc *c = ctx;
    if (p != NULL) {
        c->live--;
        c->liveBytes -= n;
    }
    free(p);
}

void ftuAllocInit(FtuAlloc *a, GfxAllocator *out, int failAt) {
    memset(a, 0, sizeof *a);
    a->failAt = failAt;
    out->alloc = fuAlloc;
    out->free = fuFree;
    out->ctx = a;
}

const char *ftuOracle(const char *name) {
    static struct {
        char name[64];
        uint8_t *text;
    } cache[8];
    for (int i = 0; i < 8; i++) {
        if (cache[i].text != NULL && strcmp(cache[i].name, name) == 0) {
            return (const char *)cache[i].text;
        }
    }
    char path[160];
    snprintf(path, sizeof path, "tests/data/font/%s", name);
    for (int i = 0; i < 8; i++) {
        if (cache[i].text == NULL) {
            cache[i].text = ftuLoad(path, NULL);
            snprintf(cache[i].name, sizeof cache[i].name, "%s", name);
            return (const char *)cache[i].text;
        }
    }
    return NULL;
}

int ftuNextLine(const char *text, size_t *pos, char *line, size_t cap) {
    if (text == NULL || text[*pos] == 0) {
        return 0;
    }
    size_t n = 0;
    while (text[*pos] != 0 && text[*pos] != '\n') {
        if (n + 1 < cap) {
            line[n++] = text[*pos];
        }
        (*pos)++;
    }
    line[n] = 0;
    if (text[*pos] == '\n') {
        (*pos)++;
    }
    return 1;
}

uint32_t ftuGet16(const uint8_t *d, uint32_t off) {
    return ((uint32_t)d[off] << 8) | d[off + 1];
}

uint32_t ftuGet32(const uint8_t *d, uint32_t off) {
    return (ftuGet16(d, off) << 16) | ftuGet16(d, off + 2);
}

void ftuPut16(uint8_t *d, uint32_t off, uint32_t v) {
    d[off] = (uint8_t)(v >> 8);
    d[off + 1] = (uint8_t)v;
}

void ftuPut32(uint8_t *d, uint32_t off, uint32_t v) {
    ftuPut16(d, off, v >> 16);
    ftuPut16(d, off + 2, v & 0xFFFFu);
}

uint32_t ftuTable(const uint8_t *d, size_t n, const char *tag, uint32_t *len) {
    (void)n;
    uint32_t nt = ftuGet16(d, 4);
    for (uint32_t i = 0; i < nt; i++) {
        const uint8_t *r = d + 12 + 16 * i;
        if (memcmp(r, tag, 4) == 0) {
            if (len != NULL) {
                *len = ftuGet32(d, 12 + 16 * i + 12);
            }
            return ftuGet32(d, 12 + 16 * i + 8);
        }
    }
    return 0;
}
