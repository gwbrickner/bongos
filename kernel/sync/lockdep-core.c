/* See lockdep-core.h. Pure: no kernel dependencies. */
#include "lockdep-core.h"

#include <stdbool.h>
#include <stdint.h>

static bool adjTest(const LockdepGraph *g, uint16_t from, uint16_t to) {
    return (g->adj[from - 1u][(to - 1u) / 64u] >> ((to - 1u) % 64u)) & 1u;
}

static void adjSet(LockdepGraph *g, uint16_t from, uint16_t to) {
    g->adj[from - 1u][(to - 1u) / 64u] |= 1ULL << ((to - 1u) % 64u);
}

LockdepVerdict lockdepCoreClassOf(LockdepGraph *g, const void *key, const char *name,
                                  uint16_t *outId) {
    if (key == NULL || name == NULL) {
        return LOCKDEP_REPORT_BAD_INIT;
    }
    for (uint32_t i = 0; i < g->classCount; i++) {
        if (g->classes[i].key == key) {
            *outId = (uint16_t)(i + 1u);
            return LOCKDEP_OK;
        }
    }
    if (g->classCount == LOCKDEP_MAX_CLASSES) {
        return LOCKDEP_FULL;
    }
    LockdepClass *c = &g->classes[g->classCount];
    c->key = key;
    c->name = name;
    c->usageMask = 0;
    g->classCount++;
    *outId = (uint16_t)g->classCount;
    return LOCKDEP_OK;
}

bool lockdepCoreHasEdge(const LockdepGraph *g, uint16_t from, uint16_t to) {
    if (from == 0 || to == 0 || from > g->classCount || to > g->classCount) {
        return false;
    }
    return adjTest(g, from, to);
}

int lockdepCoreEdgeFind(const LockdepGraph *g, uint16_t from, uint16_t to) {
    for (uint32_t i = 0; i < g->edgeCount; i++) {
        if (g->edges[i].from == from && g->edges[i].to == to) {
            return (int)i;
        }
    }
    return -1;
}

/* BFS from `src` for `dst` over the adjacency matrix. On success returns the number of nodes
 * visited-parent chain via bfsParent (bfsParent[x - 1] = predecessor id, 0 for the source). */
static bool bfsFind(LockdepGraph *g, uint16_t src, uint16_t dst) {
    for (uint32_t i = 0; i < LOCKDEP_MAX_CLASSES / 64u; i++) {
        g->bfsSeen[i] = 0;
    }
    uint32_t head = 0, tail = 0;
    g->bfsQueue[tail++] = src;
    g->bfsParent[src - 1u] = 0;
    g->bfsSeen[(src - 1u) / 64u] |= 1ULL << ((src - 1u) % 64u);
    while (head < tail) {
        uint16_t cur = g->bfsQueue[head++];
        if (cur == dst) {
            return true;
        }
        for (uint32_t w = 0; w < LOCKDEP_MAX_CLASSES / 64u; w++) {
            uint64_t bits = g->adj[cur - 1u][w] & ~g->bfsSeen[w];
            while (bits != 0) {
                uint32_t b = (uint32_t)__builtin_ctzll(bits);
                bits &= bits - 1;
                uint16_t next = (uint16_t)(w * 64u + b + 1u);
                g->bfsSeen[w] |= 1ULL << b;
                g->bfsParent[next - 1u] = cur;
                g->bfsQueue[tail++] = next;
            }
        }
    }
    return false;
}

bool lockdepCoreDependsOn(LockdepGraph *g, uint16_t from, uint16_t to) {
    if (from == 0 || to == 0 || from > g->classCount || to > g->classCount || from == to) {
        return false;
    }
    return bfsFind(g, from, to);
}

/* Index of the lowest held entry of the topmost run that shares `irqCtx` (the context segment). */
static uint32_t segmentBase(const LockdepHeldStack *hs, uint8_t irqCtx) {
    uint32_t base = hs->depth;
    while (base > 0 && hs->held[base - 1u].irqCtx == irqCtx) {
        base--;
    }
    return base;
}

/* Rebuilds the BFS chain src ... dst (after bfsFind(g, src, dst) succeeded) into f->pathClass,
 * keeping the last LOCKDEP_MAX_PATH edges if it is longer. */
static void buildPath(const LockdepGraph *g, uint16_t src, uint16_t dst, LockdepFinding *f) {
    uint16_t rev[LOCKDEP_MAX_PATH + 1u];
    uint32_t n = 0;
    uint32_t total = 0;
    for (uint16_t x = dst; x != 0; x = g->bfsParent[x - 1u]) {
        if (n < LOCKDEP_MAX_PATH + 1u) {
            rev[n++] = x;
        }
        total++;
        if (x == src) {
            break;
        }
    }
    f->pathTruncated = total > n;
    f->pathLen = n - 1u;
    for (uint32_t i = 0; i < n; i++) {
        f->pathClass[i] = rev[n - 1u - i];
    }
}

static void findingInit(LockdepFinding *f, LockdepVerdict kind, uint16_t newClass) {
    *f = (LockdepFinding){0};
    f->kind = kind;
    f->newClass = newClass;
}

LockdepVerdict lockdepCoreCheckAcquire(LockdepGraph *g, const LockdepHeldStack *hs,
                                       const void *instance, uint16_t classId, uint8_t irqCtx,
                                       bool irqsOn, bool trylock, LockdepFinding *f) {
    if (hs->depth >= LOCKDEP_MAX_HELD) {
        findingInit(f, LOCKDEP_REPORT_HELD_OVERFLOW, classId);
        return LOCKDEP_REPORT_HELD_OVERFLOW;
    }
    for (uint32_t i = 0; i < hs->depth; i++) {
        if (hs->held[i].instance == instance) {
            findingInit(f, LOCKDEP_REPORT_RECURSION, classId);
            f->heldClass = classId;
            f->heldIndex = i;
            f->sameInstance = true;
            return LOCKDEP_REPORT_RECURSION;
        }
    }
    uint32_t base = segmentBase(hs, irqCtx);
    if (!trylock) {
        for (uint32_t i = base; i < hs->depth; i++) {
            if (hs->held[i].classId == classId) {
                findingInit(f, LOCKDEP_REPORT_RECURSION, classId);
                f->heldClass = classId;
                f->heldIndex = i;
                return LOCKDEP_REPORT_RECURSION;
            }
        }
    }

    const LockdepClass *cls = &g->classes[classId - 1u];
    if (irqCtx != 0 && (cls->usageMask & (1u << LOCKDEP_USAGE_IRQS_ON)) != 0) {
        findingInit(f, LOCKDEP_REPORT_IRQ_INCONSISTENT, classId);
        f->usageNew = LOCKDEP_USAGE_IN_HARDIRQ;
        f->usageOld = LOCKDEP_USAGE_IRQS_ON;
        return LOCKDEP_REPORT_IRQ_INCONSISTENT;
    }
    if (irqCtx == 0 && irqsOn && (cls->usageMask & (1u << LOCKDEP_USAGE_IN_HARDIRQ)) != 0) {
        findingInit(f, LOCKDEP_REPORT_IRQ_INCONSISTENT, classId);
        f->usageNew = LOCKDEP_USAGE_IRQS_ON;
        f->usageOld = LOCKDEP_USAGE_IN_HARDIRQ;
        return LOCKDEP_REPORT_IRQ_INCONSISTENT;
    }

    if (!trylock) {
        for (uint32_t i = base; i < hs->depth; i++) {
            uint16_t h = hs->held[i].classId;
            if (h == classId || adjTest(g, h, classId)) {
                continue;
            }
            if (bfsFind(g, classId, h)) {
                findingInit(f, LOCKDEP_REPORT_INVERSION, classId);
                f->heldClass = h;
                f->heldIndex = i;
                buildPath(g, classId, h, f);
                return LOCKDEP_REPORT_INVERSION;
            }
        }
    }
    return LOCKDEP_OK;
}

static void markUsage(LockdepClass *c, unsigned usage, LockdepCaptureFn capture) {
    if ((c->usageMask & (1u << usage)) == 0) {
        c->usageMask = (uint8_t)(c->usageMask | (1u << usage));
        if (capture != NULL) {
            capture(&c->usageTrace[usage]);
        }
    }
}

LockdepVerdict lockdepCoreCommitAcquire(LockdepGraph *g, LockdepHeldStack *hs, const void *instance,
                                        uint16_t classId, uint8_t irqCtx, bool irqsOn, bool trylock,
                                        bool addEdges, uint64_t ip, LockdepCaptureFn capture) {
    LockdepVerdict result = LOCKDEP_OK;
    LockdepClass *cls = &g->classes[classId - 1u];
    if (irqCtx != 0) {
        markUsage(cls, LOCKDEP_USAGE_IN_HARDIRQ, capture);
    } else if (irqsOn) {
        markUsage(cls, LOCKDEP_USAGE_IRQS_ON, capture);
    }
    if (!trylock && addEdges) {
        for (uint32_t i = segmentBase(hs, irqCtx); i < hs->depth; i++) {
            uint16_t h = hs->held[i].classId;
            if (h == classId || adjTest(g, h, classId)) {
                continue;
            }
            if (g->edgeCount == LOCKDEP_MAX_EDGES) {
                result = LOCKDEP_FULL;
                continue;
            }
            LockdepEdge *e = &g->edges[g->edgeCount++];
            e->from = h;
            e->to = classId;
            e->trace.count = 0;
            if (capture != NULL) {
                capture(&e->trace);
            }
            adjSet(g, h, classId);
        }
    }
    if (hs->depth < LOCKDEP_MAX_HELD) {
        hs->held[hs->depth++] = (LockdepHeld){
            .instance = instance,
            .classId = classId,
            .irqCtx = irqCtx,
            .trylock = trylock ? 1u : 0u,
            .irqsOn = irqsOn ? 1u : 0u,
            .ip = ip,
        };
    }
    return result;
}

/* Index of the topmost entry for `instance`, or -1. */
static int heldFind(const LockdepHeldStack *hs, const void *instance) {
    for (uint32_t i = hs->depth; i > 0; i--) {
        if (hs->held[i - 1u].instance == instance) {
            return (int)(i - 1u);
        }
    }
    return -1;
}

LockdepVerdict lockdepCoreCheckRelease(const LockdepGraph *g, const LockdepHeldStack *hs,
                                       const void *instance, bool irqsOn, LockdepFinding *f) {
    int idx = heldFind(hs, instance);
    if (idx < 0) {
        findingInit(f, LOCKDEP_REPORT_NOT_HELD, 0);
        return LOCKDEP_REPORT_NOT_HELD;
    }
    const LockdepHeld *h = &hs->held[idx];
    if (h->irqCtx == 0 && h->irqsOn == 0 && irqsOn &&
        (g->classes[h->classId - 1u].usageMask & (1u << LOCKDEP_USAGE_IN_HARDIRQ)) != 0) {
        findingInit(f, LOCKDEP_REPORT_IRQ_INCONSISTENT, h->classId);
        f->heldIndex = (uint32_t)idx;
        f->usageNew = LOCKDEP_USAGE_IRQS_ON;
        f->usageOld = LOCKDEP_USAGE_IN_HARDIRQ;
        return LOCKDEP_REPORT_IRQ_INCONSISTENT;
    }
    return LOCKDEP_OK;
}

void lockdepCoreCommitRelease(LockdepGraph *g, LockdepHeldStack *hs, const void *instance,
                              bool irqsOn, LockdepCaptureFn capture) {
    int idx = heldFind(hs, instance);
    if (idx < 0) {
        return;
    }
    const LockdepHeld *h = &hs->held[idx];
    if (h->irqCtx == 0 && irqsOn) {
        markUsage(&g->classes[h->classId - 1u], LOCKDEP_USAGE_IRQS_ON, capture);
    }
    for (uint32_t i = (uint32_t)idx; i + 1u < hs->depth; i++) {
        hs->held[i] = hs->held[i + 1u];
    }
    hs->depth--;
}
