/* The lock validator's pure core (M3.4, D-186): lock classes, the acquisition-order graph, the
 * per-CPU held-lock stack and every check, with no kernel dependencies (no klog, no arch calls, no
 * statics), so it host-tests (tests/host/kernel_lockdep_core_test.c). kernel/sync/lockdep.c is the
 * kernel glue: it supplies the lock, the IRQ/context state, the stack capture and the reporting.
 *
 * All memory is fixed-size and caller-provided; an all-zero LockdepGraph is a valid empty graph, so
 * no init call is needed. None of these functions locks anything: the caller serializes access to
 * the graph (kernel: lockdep.c's raw graph lock with IRQs off) and owns the per-CPU held stack. */
#ifndef KERNEL_LOCKDEP_CORE_H
#define KERNEL_LOCKDEP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LOCKDEP_MAX_CLASSES 256u /* class id k lives at classes[k - 1]; id 0 means unassigned */
#define LOCKDEP_MAX_EDGES   512u
#define LOCKDEP_MAX_HELD    16u /* held locks per CPU */
#define LOCKDEP_TRACE_DEPTH 10u
#define LOCKDEP_MAX_PATH    16u /* edges reported along an inversion chain */

typedef struct LockdepTrace {
    uint64_t pc[LOCKDEP_TRACE_DEPTH];
    uint32_t count;
} LockdepTrace;

enum { LOCKDEP_USAGE_IN_HARDIRQ = 0, LOCKDEP_USAGE_IRQS_ON = 1, LOCKDEP_USAGE_COUNT = 2 };

typedef struct LockdepClass {
    const void *key; /* identity: the static lock's address or the init site's key */
    const char *name;
    uint8_t usageMask; /* bit LOCKDEP_USAGE_*: seen taken in a hard IRQ / with IRQs enabled */
    LockdepTrace usageTrace[LOCKDEP_USAGE_COUNT]; /* where each usage was first seen */
} LockdepClass;

typedef struct LockdepEdge {
    uint16_t from, to;  /* class ids: `from` was held while `to` was acquired */
    LockdepTrace trace; /* where the dependency was first recorded */
} LockdepEdge;

typedef struct LockdepHeld {
    const void *instance; /* the lock object (its LockdepMap) */
    uint16_t classId;
    uint8_t irqCtx; /* 0 = process context, 1 = inside a hard-IRQ handler */
    uint8_t trylock;
    uint8_t irqsOn; /* IF as the acquirer saw it (the release check applies only if 0) */
    uint64_t ip;    /* the acquire call site */
} LockdepHeld;

typedef struct LockdepHeldStack {
    LockdepHeld held[LOCKDEP_MAX_HELD];
    uint32_t depth;
} LockdepHeldStack;

typedef struct LockdepGraph {
    uint32_t classCount, edgeCount;
    LockdepClass classes[LOCKDEP_MAX_CLASSES];
    uint64_t adj[LOCKDEP_MAX_CLASSES][LOCKDEP_MAX_CLASSES / 64]; /* adj[from-1] bit (to-1) */
    LockdepEdge edges[LOCKDEP_MAX_EDGES];
    /* Scratch for the BFS, here so nothing large ever lands on the 16 KiB boot stack. */
    uint16_t bfsQueue[LOCKDEP_MAX_CLASSES], bfsParent[LOCKDEP_MAX_CLASSES];
    uint64_t bfsSeen[LOCKDEP_MAX_CLASSES / 64];
} LockdepGraph;

typedef enum LockdepVerdict {
    LOCKDEP_OK = 0,
    LOCKDEP_REPORT_INVERSION,
    LOCKDEP_REPORT_RECURSION,
    LOCKDEP_REPORT_IRQ_INCONSISTENT,
    LOCKDEP_REPORT_NOT_HELD,
    LOCKDEP_REPORT_HELD_OVERFLOW,
    LOCKDEP_REPORT_BAD_INIT,
    LOCKDEP_FULL, /* a table is full: not a report, the validator turns itself off */
} LockdepVerdict;

typedef struct LockdepFinding {
    LockdepVerdict kind;
    uint16_t newClass;          /* the class being acquired (or released) */
    uint16_t heldClass;         /* the held class it conflicts with (0 if none) */
    uint32_t heldIndex;         /* index of that entry in the held stack */
    uint8_t usageNew, usageOld; /* IRQ_INCONSISTENT: the usage now / the conflicting earlier one */
    bool sameInstance;          /* RECURSION: the very same lock object */
    uint32_t pathLen;           /* INVERSION: edges in pathClass */
    bool pathTruncated;         /* the chain is longer than LOCKDEP_MAX_PATH; its start was cut */
    uint16_t pathClass[LOCKDEP_MAX_PATH + 1]; /* chain newClass -> ... -> heldClass */
} LockdepFinding;

/* Fills `dst` with the caller's stack. Called by the commit functions, only when a new edge or a
 * first usage is recorded (never on the fast path). */
typedef void (*LockdepCaptureFn)(LockdepTrace *dst);

/* Finds or registers the class for `key`, naming it `name` (kept by pointer: must outlive the
 * graph). A NULL `key` or `name` returns LOCKDEP_REPORT_BAD_INIT; a full table, LOCKDEP_FULL. */
LockdepVerdict lockdepCoreClassOf(LockdepGraph *g, const void *key, const char *name,
                                  uint16_t *outId);

/* The checks for acquiring `instance` of class `classId` (a query: only the BFS scratch changes).
 * `irqCtx` is 1 inside a hard-IRQ handler. `irqsOn` is IF as the caller saw it (ignored in a
 * handler). Returns the first finding, in this order: held-stack overflow; the same instance held
 * anywhere (RECURSION, sameInstance); the same class held in the current context segment
 * (RECURSION), unless `trylock`; an IRQ-usage conflict; an order inversion (a new lock acquired
 * while the graph already has a path new -> held), unless `trylock`. The "segment" is the topmost
 * run of held entries taken in the same context as this acquire: an acquire inside a handler is
 * not ordered against locks the interrupted code holds (else every process-context lock would
 * wrongly precede every IRQ lock). `f` is filled only when the result is not LOCKDEP_OK. */
LockdepVerdict lockdepCoreCheckAcquire(LockdepGraph *g, const LockdepHeldStack *hs,
                                       const void *instance, uint16_t classId, uint8_t irqCtx,
                                       bool irqsOn, bool trylock, LockdepFinding *f);

/* Records an acquire that CheckAcquire cleared (or whose report the caller let through): sets the
 * class usage bit, adds the held->new order edges (not for a trylock, and not when `addEdges` is
 * false, which the caller uses after an inversion report so the bad edge is not recorded), then
 * pushes the held entry. `capture` is called for each new edge and first usage. Returns
 * LOCKDEP_FULL if the edge table filled up (the push still happens), else LOCKDEP_OK. */
LockdepVerdict lockdepCoreCommitAcquire(LockdepGraph *g, LockdepHeldStack *hs, const void *instance,
                                        uint16_t classId, uint8_t irqCtx, bool irqsOn, bool trylock,
                                        bool addEdges, uint64_t ip, LockdepCaptureFn capture);

/* Checks for releasing `instance`: NOT_HELD if it is not on the stack; IRQ_INCONSISTENT if it was
 * taken in process context with IRQs off and IRQs are now enabled (`irqsOn`) although its class is
 * also used in a hard IRQ (IRQs were enabled while it was held; a lock acquired with IRQs on is
 * already reported at acquire). Releases may be out of order. */
LockdepVerdict lockdepCoreCheckRelease(const LockdepGraph *g, const LockdepHeldStack *hs,
                                       const void *instance, bool irqsOn, LockdepFinding *f);

/* Records the release: marks IRQS_ON when a process-context lock is released with IRQs enabled,
 * then removes the topmost entry for `instance` (entries above it shift down). A missing entry is
 * ignored. */
void lockdepCoreCommitRelease(LockdepGraph *g, LockdepHeldStack *hs, const void *instance,
                              bool irqsOn, LockdepCaptureFn capture);

/* The index of the edge from -> to, or -1. Linear; for reports and tests. */
int lockdepCoreEdgeFind(const LockdepGraph *g, uint16_t from, uint16_t to);

/* True iff the graph has a direct edge from -> to (a bit test). */
bool lockdepCoreHasEdge(const LockdepGraph *g, uint16_t from, uint16_t to);

/* True iff some chain of edges leads from -> to (BFS; changes only the scratch). */
bool lockdepCoreDependsOn(LockdepGraph *g, uint16_t from, uint16_t to);

#endif
