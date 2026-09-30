/* See gfx-path.h. */
/* Float appears here (D-142): contraction is off so results do not depend on the target's FMA. */
#pragma STDC FP_CONTRACT OFF

#include "gfx/gfx-internal.h"

#include <string.h>

void gfxPathInit(GfxPath *p, const GfxAllocator *a) {
    memset(p, 0, sizeof(*p));
    p->alloc = a != NULL ? a : gfxAllocatorDefault();
}

void gfxPathReset(GfxPath *p) {
    p->nVerbs = 0;
    p->nPts = 0;
    p->error = STATUS_OK;
    p->hasCurrent = false;
    p->open = false;
}

void gfxPathFree(GfxPath *p) {
    if (p->verbs != NULL) {
        p->alloc->free(p->alloc->ctx, p->verbs, p->capVerbs * sizeof(uint8_t));
    }
    if (p->pts != NULL) {
        p->alloc->free(p->alloc->ctx, p->pts, p->capPts * sizeof(float));
    }
    p->verbs = NULL;
    p->pts = NULL;
    p->capVerbs = p->capPts = 0;
    gfxPathReset(p);
}

/* Makes room for `nv` more verbs and `np` more points. Sticky-fails with NO_MEMORY. */
static Status reserve(GfxPath *p, uint32_t nv, uint32_t np) {
    if (p->nVerbs + nv > GFX_PATH_MAX_VERBS) {
        return p->error = STATUS_ERR_NO_MEMORY;
    }
    if (p->nVerbs + nv > p->capVerbs) {
        uint32_t cap = p->capVerbs != 0 ? p->capVerbs : 16;
        while (cap < p->nVerbs + nv) {
            cap *= 2;
        }
        uint8_t *v = p->alloc->alloc(p->alloc->ctx, cap * sizeof(uint8_t));
        if (v == NULL) {
            return p->error = STATUS_ERR_NO_MEMORY;
        }
        if (p->verbs != NULL) {
            memcpy(v, p->verbs, p->nVerbs);
            p->alloc->free(p->alloc->ctx, p->verbs, p->capVerbs * sizeof(uint8_t));
        }
        p->verbs = v;
        p->capVerbs = cap;
    }
    if (p->nPts + np > p->capPts) {
        uint32_t cap = p->capPts != 0 ? p->capPts : 64;
        while (cap < p->nPts + np) {
            cap *= 2;
        }
        float *q = p->alloc->alloc(p->alloc->ctx, cap * sizeof(float));
        if (q == NULL) {
            return p->error = STATUS_ERR_NO_MEMORY;
        }
        if (p->pts != NULL) {
            memcpy(q, p->pts, p->nPts * sizeof(float));
            p->alloc->free(p->alloc->ctx, p->pts, p->capPts * sizeof(float));
        }
        p->pts = q;
        p->capPts = cap;
    }
    return STATUS_OK;
}

static bool finite(float v) {
    return __builtin_isfinite(v);
}

static Status push(GfxPath *p, uint8_t verb, const float *c, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (!finite(c[i])) {
            return p->error = STATUS_ERR_INVALID;
        }
    }
    Status st = reserve(p, 1, n);
    if (st != STATUS_OK) {
        return st;
    }
    p->verbs[p->nVerbs++] = verb;
    if (n != 0) {
        memcpy(p->pts + p->nPts, c, n * sizeof(float));
    }
    p->nPts += n;
    return STATUS_OK;
}

Status gfxPathMoveTo(GfxPath *p, float x, float y) {
    if (p->error != STATUS_OK) {
        return p->error;
    }
    float c[2] = {x, y};
    Status st = push(p, GFX_VERB_MOVE, c, 2);
    if (st == STATUS_OK) {
        p->startX = x;
        p->startY = y;
        p->hasCurrent = true;
        p->open = true;
    }
    return st;
}

/* After a close, the next segment starts a new subpath at the previous start point. */
static Status ensureOpen(GfxPath *p) {
    if (p->error != STATUS_OK) {
        return p->error;
    }
    if (!p->hasCurrent) {
        return p->error = STATUS_ERR_INVALID;
    }
    if (!p->open) {
        return gfxPathMoveTo(p, p->startX, p->startY);
    }
    return STATUS_OK;
}

Status gfxPathLineTo(GfxPath *p, float x, float y) {
    Status st = ensureOpen(p);
    if (st != STATUS_OK) {
        return st;
    }
    float c[2] = {x, y};
    return push(p, GFX_VERB_LINE, c, 2);
}

Status gfxPathQuadTo(GfxPath *p, float cx, float cy, float x, float y) {
    Status st = ensureOpen(p);
    if (st != STATUS_OK) {
        return st;
    }
    float c[4] = {cx, cy, x, y};
    return push(p, GFX_VERB_QUAD, c, 4);
}

Status gfxPathCubicTo(GfxPath *p, float c1x, float c1y, float c2x, float c2y, float x, float y) {
    Status st = ensureOpen(p);
    if (st != STATUS_OK) {
        return st;
    }
    float c[6] = {c1x, c1y, c2x, c2y, x, y};
    return push(p, GFX_VERB_CUBIC, c, 6);
}

Status gfxPathClose(GfxPath *p) {
    if (p->error != STATUS_OK) {
        return p->error;
    }
    if (!p->hasCurrent) {
        return p->error = STATUS_ERR_INVALID;
    }
    if (!p->open) {
        return STATUS_OK; /* already closed */
    }
    Status st = push(p, GFX_VERB_CLOSE, NULL, 0);
    if (st == STATUS_OK) {
        p->open = false;
    }
    return st;
}

Status gfxPathAddRect(GfxPath *p, float x, float y, float w, float h) {
    if (w < 0) {
        x += w;
        w = -w;
    }
    if (h < 0) {
        y += h;
        h = -h;
    }
    gfxPathMoveTo(p, x, y);
    gfxPathLineTo(p, x + w, y);
    gfxPathLineTo(p, x + w, y + h);
    gfxPathLineTo(p, x, y + h);
    return gfxPathClose(p);
}

#define KAPPA 0.55228475f /* 4/3*(sqrt(2)-1): a cubic's quarter-circle control offset */

Status gfxPathAddRoundedRect(GfxPath *p, float x, float y, float w, float h, float r) {
    if (w < 0) {
        x += w;
        w = -w;
    }
    if (h < 0) {
        y += h;
        h = -h;
    }
    float m = (w < h ? w : h) * 0.5f;
    if (!(r > 0.0f)) {
        return gfxPathAddRect(p, x, y, w, h);
    }
    if (r > m) {
        r = m;
    }
    float k = KAPPA * r;
    float x1 = x + w, y1 = y + h;
    gfxPathMoveTo(p, x + r, y);
    gfxPathLineTo(p, x1 - r, y);
    gfxPathCubicTo(p, x1 - r + k, y, x1, y + r - k, x1, y + r);
    gfxPathLineTo(p, x1, y1 - r);
    gfxPathCubicTo(p, x1, y1 - r + k, x1 - r + k, y1, x1 - r, y1);
    gfxPathLineTo(p, x + r, y1);
    gfxPathCubicTo(p, x + r - k, y1, x, y1 - r + k, x, y1 - r);
    gfxPathLineTo(p, x, y + r);
    gfxPathCubicTo(p, x, y + r - k, x + r - k, y, x + r, y);
    return gfxPathClose(p);
}

Status gfxPathAddEllipse(GfxPath *p, float cx, float cy, float rx, float ry) {
    if (rx < 0) {
        rx = -rx;
    }
    if (ry < 0) {
        ry = -ry;
    }
    float kx = KAPPA * rx, ky = KAPPA * ry;
    gfxPathMoveTo(p, cx + rx, cy);
    gfxPathCubicTo(p, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    gfxPathCubicTo(p, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    gfxPathCubicTo(p, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    gfxPathCubicTo(p, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    return gfxPathClose(p);
}
