/* See compare.h. */
#include "compare.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png.h"

static void setErr(char *errbuf, size_t errbufCap, const char *msg) {
    if (errbuf != NULL && errbufCap > 0) {
        size_t i = 0;
        for (; msg[i] != '\0' && i < errbufCap - 1; i++) {
            errbuf[i] = msg[i];
        }
        errbuf[i] = '\0';
    }
}

bool maskLoad(const char *path, uint32_t imgWidth, uint32_t imgHeight, MaskList *out, char *errbuf,
              size_t errbufCap) {
    out->rects = NULL;
    out->count = 0;
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        setErr(errbuf, errbufCap, "cannot open mask file");
        return false;
    }
    uint32_t cap = 0;
    char line[256];
    int lineNo = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        lineNo++;
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0' || *p == '\n' || *p == '#') {
            continue;
        }
        long x, y, w, h;
        if (sscanf(p, "%ld %ld %ld %ld", &x, &y, &w, &h) != 4 || x < 0 || y < 0 || w < 0 || h < 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "mask file: malformed line %d", lineNo);
            setErr(errbuf, errbufCap, msg);
            fclose(f);
            free(out->rects);
            out->rects = NULL;
            out->count = 0;
            return false;
        }
        if ((uint32_t)x >= imgWidth || (uint32_t)y >= imgHeight) {
            continue; /* entirely outside the image: clip to nothing */
        }
        uint32_t cw = ((uint32_t)x + (uint32_t)w > imgWidth) ? imgWidth - (uint32_t)x : (uint32_t)w;
        uint32_t ch =
            ((uint32_t)y + (uint32_t)h > imgHeight) ? imgHeight - (uint32_t)y : (uint32_t)h;
        if (cw == 0 || ch == 0) {
            continue;
        }
        if (out->count >= cap) {
            cap = cap == 0 ? 8 : cap * 2;
            MaskRect *n = realloc(out->rects, cap * sizeof(MaskRect));
            if (n == NULL) {
                setErr(errbuf, errbufCap, "out of memory");
                fclose(f);
                free(out->rects);
                out->rects = NULL;
                out->count = 0;
                return false;
            }
            out->rects = n;
        }
        out->rects[out->count].x = (uint32_t)x;
        out->rects[out->count].y = (uint32_t)y;
        out->rects[out->count].w = cw;
        out->rects[out->count].h = ch;
        out->count++;
    }
    fclose(f);
    return true;
}

void maskFree(MaskList *m) {
    free(m->rects);
    m->rects = NULL;
    m->count = 0;
}

bool maskContains(const MaskList *m, uint32_t x, uint32_t y) {
    if (m == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < m->count; i++) {
        const MaskRect *r = &m->rects[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h) {
            return true;
        }
    }
    return false;
}

static uint32_t channelDelta(const Image *a, const Image *b, uint32_t x, uint32_t y) {
    size_t idx = ((size_t)y * a->width + x) * 3;
    int dr = (int)a->rgb[idx + 0] - (int)b->rgb[idx + 0];
    int dg = (int)a->rgb[idx + 1] - (int)b->rgb[idx + 1];
    int db = (int)a->rgb[idx + 2] - (int)b->rgb[idx + 2];
    if (dr < 0) {
        dr = -dr;
    }
    if (dg < 0) {
        dg = -dg;
    }
    if (db < 0) {
        db = -db;
    }
    uint32_t m = (uint32_t)dr;
    if ((uint32_t)dg > m) {
        m = (uint32_t)dg;
    }
    if ((uint32_t)db > m) {
        m = (uint32_t)db;
    }
    return m;
}

bool imagesCompare(const Image *actual, const Image *ref, int maxChannelDelta,
                   uint32_t maxDiffPixels, const MaskList *mask, CompareResult *result) {
    memset(result, 0, sizeof(*result));
    if (actual->width != ref->width || actual->height != ref->height) {
        result->dimensionsMatch = false;
        return false;
    }
    result->dimensionsMatch = true;

    for (uint32_t y = 0; y < actual->height; y++) {
        for (uint32_t x = 0; x < actual->width; x++) {
            if (maskContains(mask, x, y)) {
                continue;
            }
            uint32_t delta = channelDelta(actual, ref, x, y);
            if (delta > result->maxDeltaSeen) {
                result->maxDeltaSeen = delta;
            }
            if ((int)delta > maxChannelDelta) {
                if (!result->haveFirstDiff) {
                    result->haveFirstDiff = true;
                    result->firstDiffX = x;
                    result->firstDiffY = y;
                }
                result->diffPixelCount++;
            }
        }
    }
    return result->diffPixelCount <= maxDiffPixels;
}

bool writeDiffImage(const char *path, const Image *actual, const Image *ref, const MaskList *mask,
                    int maxChannelDelta, char *errbuf, size_t errbufCap) {
    if (actual->width != ref->width || actual->height != ref->height) {
        setErr(errbuf, errbufCap, "dimension mismatch: cannot render a diff image");
        return false;
    }
    Image diff;
    if (!imageAlloc(&diff, actual->width, actual->height)) {
        setErr(errbuf, errbufCap, "out of memory");
        return false;
    }
    for (uint32_t y = 0; y < actual->height; y++) {
        for (uint32_t x = 0; x < actual->width; x++) {
            size_t idx = ((size_t)y * actual->width + x) * 3;
            if (maskContains(mask, x, y)) {
                diff.rgb[idx + 0] = 0x00;
                diff.rgb[idx + 1] = 0x00;
                diff.rgb[idx + 2] = 0x40;
                continue;
            }
            uint32_t delta = channelDelta(actual, ref, x, y);
            if ((int)delta > maxChannelDelta) {
                diff.rgb[idx + 0] = 0xFF;
                diff.rgb[idx + 1] = 0x00;
                diff.rgb[idx + 2] = 0x00;
            } else {
                diff.rgb[idx + 0] = (uint8_t)(actual->rgb[idx + 0] / 4);
                diff.rgb[idx + 1] = (uint8_t)(actual->rgb[idx + 1] / 4);
                diff.rgb[idx + 2] = (uint8_t)(actual->rgb[idx + 2] / 4);
            }
        }
    }
    bool ok = pngWrite(path, &diff, errbuf, errbufCap);
    imageFree(&diff);
    return ok;
}
