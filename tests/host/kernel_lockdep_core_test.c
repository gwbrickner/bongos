/* Host tests for the lock validator's pure core (kernel/sync/lockdep-core.c, M3.4, D-186). */
#include "framework/test.h"
#include "lockdep-core.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static LockdepGraph *newGraph(void) {
    return calloc(1, sizeof(LockdepGraph)); /* all-zero is a valid empty graph */
}

static int captureCalls;
static void captureStub(LockdepTrace *dst) {
    dst->count = 1;
    dst->pc[0] = 0x1000u + (uint64_t)captureCalls;
    captureCalls++;
}

/* A little fixture: 32 distinct lock objects/keys. */
static char keys[LOCKDEP_MAX_CLASSES + 8];

static uint16_t cls(LockdepGraph *g, unsigned i) {
    uint16_t id = 0;
    if (lockdepCoreClassOf(g, &keys[i], "k", &id) != LOCKDEP_OK) {
        abort();
    }
    return id;
}

/* Acquire with the full check+commit sequence the kernel glue runs. */
static int acq(LockdepGraph *g, LockdepHeldStack *hs, unsigned inst, unsigned classIdx, uint8_t ctx,
               bool irqsOn, bool trylock) {
    LockdepFinding f;
    uint16_t id = cls(g, classIdx);
    int v = (int)lockdepCoreCheckAcquire(g, hs, &keys[inst], id, ctx, irqsOn, trylock, &f);
    lockdepCoreCommitAcquire(g, hs, &keys[inst], id, ctx, irqsOn, trylock,
                             v != LOCKDEP_REPORT_INVERSION, 0, captureStub);
    return v;
}

static void rel(LockdepGraph *g, LockdepHeldStack *hs, unsigned inst, bool irqsOn) {
    lockdepCoreCommitRelease(g, hs, &keys[inst], irqsOn, captureStub);
}

TEST(lockdepClassRegistrationDedups) {
    LockdepGraph *g = newGraph();
    uint16_t a = 0, b = 0, a2 = 0;
    ASSERT_EQ((int)lockdepCoreClassOf(g, &keys[0], "a", &a), LOCKDEP_OK);
    ASSERT_EQ((int)lockdepCoreClassOf(g, &keys[1], "b", &b), LOCKDEP_OK);
    ASSERT_EQ((int)lockdepCoreClassOf(g, &keys[0], "a", &a2), LOCKDEP_OK);
    ASSERT_EQ(a, 1);
    ASSERT_EQ(b, 2);
    ASSERT_EQ(a2, a);
    ASSERT_EQ(g->classCount, 2u);
    ASSERT_EQ((int)lockdepCoreClassOf(g, NULL, "x", &a), LOCKDEP_REPORT_BAD_INIT);
    ASSERT_EQ((int)lockdepCoreClassOf(g, &keys[2], NULL, &a), LOCKDEP_REPORT_BAD_INIT);
    free(g);
}

TEST(lockdepClassTableFull) {
    LockdepGraph *g = newGraph();
    uint16_t id = 0;
    for (unsigned i = 0; i < LOCKDEP_MAX_CLASSES; i++) {
        ASSERT_EQ((int)lockdepCoreClassOf(g, &keys[i], "k", &id), LOCKDEP_OK);
    }
    ASSERT_EQ(id, LOCKDEP_MAX_CLASSES);
    ASSERT_EQ((int)lockdepCoreClassOf(g, &keys[LOCKDEP_MAX_CLASSES], "k", &id), LOCKDEP_FULL);
    ASSERT_EQ((int)lockdepCoreClassOf(g, &keys[5], "k", &id),
              LOCKDEP_OK); /* existing still found */
    ASSERT_EQ(id, 6);
    free(g);
}

TEST(lockdepDirectInversion) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    ASSERT_EQ(acq(g, &hs, 0, 0, 0, false, false), LOCKDEP_OK); /* A */
    ASSERT_EQ(acq(g, &hs, 1, 1, 0, false, false), LOCKDEP_OK); /* B: edge A->B */
    rel(g, &hs, 1, false);
    rel(g, &hs, 0, false);
    ASSERT_TRUE(lockdepCoreHasEdge(g, 1, 2));
    ASSERT_EQ(g->edgeCount, 1u);

    ASSERT_EQ(acq(g, &hs, 1, 1, 0, false, false), LOCKDEP_OK); /* B */
    LockdepFinding f;
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[0], 1, 0, false, false, &f),
              LOCKDEP_REPORT_INVERSION);
    ASSERT_EQ(f.newClass, 1);
    ASSERT_EQ(f.heldClass, 2);
    ASSERT_EQ(f.pathLen, 1u);
    ASSERT_EQ(f.pathClass[0], 1);
    ASSERT_EQ(f.pathClass[1], 2);
    ASSERT_TRUE(!f.pathTruncated);
    /* The glue skips the edge after reporting: B->A must not appear. */
    lockdepCoreCommitAcquire(g, &hs, &keys[0], 1, 0, false, false, false, 0, captureStub);
    ASSERT_TRUE(!lockdepCoreHasEdge(g, 2, 1));
    ASSERT_EQ(g->edgeCount, 1u);
    free(g);
}

TEST(lockdepTransitiveInversion) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    /* A->B, B->C recorded in separate sections. */
    acq(g, &hs, 0, 0, 0, false, false);
    acq(g, &hs, 1, 1, 0, false, false);
    rel(g, &hs, 1, false);
    rel(g, &hs, 0, false);
    acq(g, &hs, 1, 1, 0, false, false);
    acq(g, &hs, 2, 2, 0, false, false);
    rel(g, &hs, 2, false);
    rel(g, &hs, 1, false);
    /* Hold C, take A: A->B->C exists. */
    acq(g, &hs, 2, 2, 0, false, false);
    LockdepFinding f;
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[0], 1, 0, false, false, &f),
              LOCKDEP_REPORT_INVERSION);
    ASSERT_EQ(f.pathLen, 2u);
    ASSERT_EQ(f.pathClass[0], 1);
    ASSERT_EQ(f.pathClass[1], 2);
    ASSERT_EQ(f.pathClass[2], 3);
    ASSERT_TRUE(lockdepCoreDependsOn(g, 1, 3));
    ASSERT_TRUE(!lockdepCoreDependsOn(g, 3, 1));
    free(g);
}

TEST(lockdepDiamondIsNoCycle) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    /* A->B, A->C, B->D, C->D */
    unsigned pairs[4][2] = {{0, 1}, {0, 2}, {1, 3}, {2, 3}};
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(acq(g, &hs, pairs[i][0], pairs[i][0], 0, false, false), LOCKDEP_OK);
        ASSERT_EQ(acq(g, &hs, pairs[i][1], pairs[i][1], 0, false, false), LOCKDEP_OK);
        rel(g, &hs, pairs[i][1], false);
        rel(g, &hs, pairs[i][0], false);
    }
    ASSERT_EQ(g->edgeCount, 4u);
    ASSERT_EQ(hs.depth, 0u);
    /* A->D directly is consistent with both paths. */
    ASSERT_EQ(acq(g, &hs, 0, 0, 0, false, false), LOCKDEP_OK);
    ASSERT_EQ(acq(g, &hs, 3, 3, 0, false, false), LOCKDEP_OK);
    free(g);
}

TEST(lockdepRecursion) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    LockdepFinding f;
    acq(g, &hs, 0, 0, 0, false, false);
    /* Same instance. */
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[0], 1, 0, false, false, &f),
              LOCKDEP_REPORT_RECURSION);
    ASSERT_TRUE(f.sameInstance);
    /* Same instance even as a trylock, even in another context. */
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[0], 1, 1, false, true, &f),
              LOCKDEP_REPORT_RECURSION);
    /* Another instance of the same class (a different key slot, same class id). */
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[9], 1, 0, false, false, &f),
              LOCKDEP_REPORT_RECURSION);
    ASSERT_TRUE(!f.sameInstance);
    /* ... but as a trylock it is allowed. */
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[9], 1, 0, false, true, &f), LOCKDEP_OK);
    free(g);
}

TEST(lockdepIrqContextSegmentRecordsNoEdge) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    acq(g, &hs, 0, 0, 0, false, false);                        /* process holds A */
    ASSERT_EQ(acq(g, &hs, 1, 1, 1, false, false), LOCKDEP_OK); /* handler takes B */
    ASSERT_EQ(g->edgeCount, 0u);
    /* Within the handler a nested lock does get an edge. */
    ASSERT_EQ(acq(g, &hs, 2, 2, 1, false, false), LOCKDEP_OK);
    ASSERT_EQ(g->edgeCount, 1u);
    ASSERT_TRUE(lockdepCoreHasEdge(g, 2, 3));
    /* The same class held by the interrupted code is not recursion for the handler. */
    rel(g, &hs, 2, false);
    rel(g, &hs, 1, false);
    ASSERT_EQ(acq(g, &hs, 8, 0, 1, false, false), LOCKDEP_OK); /* class of A, other instance */
    free(g);
}

TEST(lockdepIrqUsageBothDirections) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    LockdepFinding f;
    /* Class 0: taken with IRQs on, then in a handler -> inconsistent. */
    ASSERT_EQ(acq(g, &hs, 0, 0, 0, true, false), LOCKDEP_OK);
    rel(g, &hs, 0, true);
    uint16_t id = cls(g, 0);
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[0], id, 1, false, false, &f),
              LOCKDEP_REPORT_IRQ_INCONSISTENT);
    ASSERT_EQ(f.usageNew, LOCKDEP_USAGE_IN_HARDIRQ);
    ASSERT_EQ(f.usageOld, LOCKDEP_USAGE_IRQS_ON);
    /* Class 1: taken in a handler, then with IRQs on -> inconsistent. */
    ASSERT_EQ(acq(g, &hs, 1, 1, 1, false, false), LOCKDEP_OK);
    rel(g, &hs, 1, false);
    id = cls(g, 1);
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[1], id, 0, true, false, &f),
              LOCKDEP_REPORT_IRQ_INCONSISTENT);
    ASSERT_EQ(f.usageNew, LOCKDEP_USAGE_IRQS_ON);
    ASSERT_EQ(f.usageOld, LOCKDEP_USAGE_IN_HARDIRQ);
    /* Taken with IRQs off (irqsave) in process context is fine either way. */
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[1], id, 0, false, false, &f), LOCKDEP_OK);
    free(g);
}

TEST(lockdepReleaseChecks) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    LockdepFinding f;
    ASSERT_EQ((int)lockdepCoreCheckRelease(g, &hs, &keys[0], false, &f), LOCKDEP_REPORT_NOT_HELD);
    /* A lock used in a handler, then held in process context while IRQs get enabled. */
    acq(g, &hs, 0, 0, 1, false, false);
    rel(g, &hs, 0, false);
    ASSERT_EQ(acq(g, &hs, 0, 0, 0, false, false), LOCKDEP_OK); /* IRQs off at acquire */
    ASSERT_EQ((int)lockdepCoreCheckRelease(g, &hs, &keys[0], false, &f), LOCKDEP_OK);
    ASSERT_EQ((int)lockdepCoreCheckRelease(g, &hs, &keys[0], true, &f),
              LOCKDEP_REPORT_IRQ_INCONSISTENT);
    /* Out-of-order release keeps the rest of the stack. */
    acq(g, &hs, 1, 1, 0, false, false);
    acq(g, &hs, 2, 2, 0, false, false);
    rel(g, &hs, 1, false);
    ASSERT_EQ(hs.depth, 2u);
    ASSERT_TRUE(hs.held[0].instance == &keys[0]);
    ASSERT_TRUE(hs.held[1].instance == &keys[2]);
    free(g);
}

TEST(lockdepTrylockEdges) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    acq(g, &hs, 0, 0, 0, false, false);
    ASSERT_EQ(acq(g, &hs, 1, 1, 0, false, true), LOCKDEP_OK); /* trylock B: no edge A->B */
    ASSERT_EQ(g->edgeCount, 0u);
    ASSERT_EQ(acq(g, &hs, 2, 2, 0, false, false), LOCKDEP_OK); /* C under A and (trylocked) B */
    ASSERT_TRUE(lockdepCoreHasEdge(g, 1, 3));
    ASSERT_TRUE(lockdepCoreHasEdge(g, 2, 3));
    ASSERT_TRUE(!lockdepCoreHasEdge(g, 1, 2));
    free(g);
}

TEST(lockdepHeldOverflowAndEdgeFull) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    LockdepFinding f;
    for (unsigned i = 0; i < LOCKDEP_MAX_HELD; i++) {
        ASSERT_EQ(acq(g, &hs, i, i, 0, false, false), LOCKDEP_OK);
    }
    uint16_t id = cls(g, LOCKDEP_MAX_HELD);
    ASSERT_EQ(
        (int)lockdepCoreCheckAcquire(g, &hs, &keys[LOCKDEP_MAX_HELD], id, 0, false, false, &f),
        LOCKDEP_REPORT_HELD_OVERFLOW);
    free(g);

    /* Edge table full: the commit says FULL but still pushes. */
    g = newGraph();
    memset(&hs, 0, sizeof(hs));
    g->edgeCount = LOCKDEP_MAX_EDGES;
    acq(g, &hs, 0, 0, 0, false, false);
    uint16_t b = cls(g, 1);
    ASSERT_EQ(
        (int)lockdepCoreCommitAcquire(g, &hs, &keys[1], b, 0, false, false, true, 0, captureStub),
        LOCKDEP_FULL);
    ASSERT_EQ(hs.depth, 2u);
    free(g);
}

TEST(lockdepCaptureOnlyOnNewEdgeOrUsage) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    captureCalls = 0;
    acq(g, &hs, 0, 0, 0, true, false); /* usage IRQS_ON of class A */
    ASSERT_EQ(captureCalls, 1);
    acq(g, &hs, 1, 1, 0, true, false); /* usage of B + edge A->B */
    ASSERT_EQ(captureCalls, 3);
    rel(g, &hs, 1, true);
    rel(g, &hs, 0, true);
    acq(g, &hs, 0, 0, 0, true, false); /* nothing new */
    acq(g, &hs, 1, 1, 0, true, false);
    ASSERT_EQ(captureCalls, 3);
    ASSERT_EQ(g->edges[0].trace.count, 1u);
    free(g);
}

TEST(lockdepLongPathTruncates) {
    LockdepGraph *g = newGraph();
    LockdepHeldStack hs = {0};
    /* Chain 0->1->2->...->20, then hold 20 and take 0. */
    for (unsigned i = 0; i < 20; i++) {
        acq(g, &hs, i, i, 0, false, false);
        acq(g, &hs, i + 1, i + 1, 0, false, false);
        rel(g, &hs, i + 1, false);
        rel(g, &hs, i, false);
    }
    acq(g, &hs, 20, 20, 0, false, false);
    LockdepFinding f;
    ASSERT_EQ((int)lockdepCoreCheckAcquire(g, &hs, &keys[0], 1, 0, false, false, &f),
              LOCKDEP_REPORT_INVERSION);
    ASSERT_TRUE(f.pathTruncated);
    ASSERT_EQ(f.pathLen, LOCKDEP_MAX_PATH);
    ASSERT_EQ(f.pathClass[f.pathLen], 21);
    free(g);
}
