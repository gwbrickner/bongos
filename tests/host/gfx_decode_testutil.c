/* See gfx_decode_testutil.h. */
#include "gfx_decode_testutil.h"

#include "compress/compress.h"

#include <stdio.h>
#include <stdlib.h>

static void *daAlloc(void *ctx, size_t n) {
    DecCountAlloc *c = ctx;
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

static void daFree(void *ctx, void *p, size_t n) {
    DecCountAlloc *c = ctx;
    if (p != NULL) {
        c->live--;
        c->liveBytes -= n;
    }
    free(p);
}

void decAllocInit(DecCountAlloc *s, GfxAllocator *a, int failAt) {
    s->failAt = failAt;
    s->count = s->live = 0;
    s->liveBytes = s->peakBytes = 0;
    a->alloc = daAlloc;
    a->free = daFree;
    a->ctx = s;
}

uint32_t decLe32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t decLe16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

uint32_t decRng(uint32_t *s) {
    uint32_t x = *s != 0 ? *s : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

bool decPremulOk(uint32_t c) {
    uint32_t a = c >> 24;
    return ((c >> 16) & 0xFF) <= a && ((c >> 8) & 0xFF) <= a && (c & 0xFF) <= a;
}

bool decLoadContainer(const char *path, uint8_t **raw, size_t *rawLen) {
    *raw = NULL;
    *rawLen = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    long zn = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (zn < 5) {
        fclose(f);
        return false;
    }
    uint8_t *z = malloc((size_t)zn);
    if (z == NULL || fread(z, 1, (size_t)zn, f) != (size_t)zn) {
        fclose(f);
        free(z);
        return false;
    }
    fclose(f);
    size_t n = decLe32(z);
    uint8_t *buf = malloc(n != 0 ? n : 1);
    size_t got = 0;
    Status st = buf != NULL ? compressZlibInflate(z + 4, (size_t)zn - 4, buf, n, &got, NULL)
                            : STATUS_ERR_NO_MEMORY;
    free(z);
    if (st != STATUS_OK || got != n) {
        free(buf);
        return false;
    }
    *raw = buf;
    *rawLen = n;
    return true;
}
