/* Host tests for the timer heap in kernel/core/time-core.c (M3.3, D-176): ordering, FIFO
 * tie-break, arbitrary removal and the capacity limit, against a brute-force reference. */
#include "framework/test.h"
#include "time-core.h"

#include <stdint.h>
#include <stdlib.h>

static uint64_t seed = 0x1234567887654321ull;
static uint32_t rnd32(void) {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(seed >> 33);
}

static TimerObj pool[TIMER_HEAP_CAP + 8];

static void resetPool(void) {
    for (unsigned i = 0; i < sizeof(pool) / sizeof(pool[0]); i++) {
        pool[i] = (TimerObj){0};
    }
}

TEST(timerHeapOrdersByDeadlineThenSeq) {
    TimerHeap h;
    timerHeapInit(&h);
    resetPool();
    uint64_t dl[8] = {50, 10, 30, 10, 20, 50, 5, 10};
    for (int i = 0; i < 8; i++) {
        pool[i].deadlineNs = dl[i];
        ASSERT_EQ(timerHeapInsert(&h, &pool[i]), STATUS_OK);
        ASSERT_EQ(pool[i].state, TIMER_ARMED);
        ASSERT_TRUE(timerHeapCheck(&h));
    }
    /* expected order: 5(6) 10(1) 10(3) 10(7) 20(4) 30(2) 50(0) 50(5) */
    int order[8] = {6, 1, 3, 7, 4, 2, 0, 5};
    for (int i = 0; i < 8; i++) {
        ASSERT_TRUE(timerHeapPeek(&h) == &pool[order[i]]);
        ASSERT_TRUE(timerHeapPop(&h) == &pool[order[i]]);
        ASSERT_TRUE(timerHeapCheck(&h));
    }
    ASSERT_TRUE(timerHeapPop(&h) == NULL);
    ASSERT_TRUE(timerHeapPeek(&h) == NULL);
}

TEST(timerHeapRemoveRootLastAndMiddle) {
    TimerHeap h;
    timerHeapInit(&h);
    resetPool();
    for (int i = 0; i < 15; i++) {
        pool[i].deadlineNs = (uint64_t)(i + 1) * 10;
        ASSERT_EQ(timerHeapInsert(&h, &pool[i]), STATUS_OK);
    }
    timerHeapRemove(&h, &pool[0]); /* root */
    ASSERT_TRUE(timerHeapCheck(&h));
    ASSERT_TRUE(timerHeapPeek(&h) == &pool[1]);
    timerHeapRemove(&h, &pool[h.slot[h.count - 1] - pool]); /* the last slot */
    ASSERT_TRUE(timerHeapCheck(&h));
    timerHeapRemove(&h, h.slot[3]); /* a middle node */
    ASSERT_TRUE(timerHeapCheck(&h));
    ASSERT_EQ(h.count, 12u);
    uint64_t prev = 0;
    while (h.count > 0) {
        TimerObj *t = timerHeapPop(&h);
        ASSERT_TRUE(t->deadlineNs >= prev);
        prev = t->deadlineNs;
    }
}

TEST(timerHeapCapacityIsEnforced) {
    TimerHeap h;
    timerHeapInit(&h);
    resetPool();
    for (uint32_t i = 0; i < TIMER_HEAP_CAP; i++) {
        pool[i].deadlineNs = 1000 + i;
        ASSERT_EQ(timerHeapInsert(&h, &pool[i]), STATUS_OK);
    }
    pool[TIMER_HEAP_CAP].deadlineNs = 1;
    pool[TIMER_HEAP_CAP].state = TIMER_IDLE;
    ASSERT_EQ(timerHeapInsert(&h, &pool[TIMER_HEAP_CAP]), STATUS_ERR_NO_MEMORY);
    ASSERT_EQ(h.count, (uint32_t)TIMER_HEAP_CAP);
    ASSERT_EQ(pool[TIMER_HEAP_CAP].state, TIMER_IDLE); /* unchanged */
    ASSERT_TRUE(timerHeapCheck(&h));
    ASSERT_TRUE(timerHeapPeek(&h) == &pool[0]);
    timerHeapRemove(&h, &pool[5]);
    ASSERT_EQ(timerHeapInsert(&h, &pool[TIMER_HEAP_CAP]), STATUS_OK); /* room again */
    ASSERT_TRUE(timerHeapPeek(&h) == &pool[TIMER_HEAP_CAP]);
}

TEST(timerHeapRandomOpsMatchReference) {
    TimerHeap h;
    timerHeapInit(&h);
    resetPool();
    /* the reference: per-object armed flag; the expected next is the min (deadline, seq) */
    for (int step = 0; step < 100000; step++) {
        uint32_t op = rnd32() % 4;
        uint32_t i = rnd32() % TIMER_HEAP_CAP;
        if (op <= 1) {
            if (pool[i].state != TIMER_ARMED) {
                pool[i].deadlineNs = rnd32() % 64; /* many equal deadlines */
                ASSERT_EQ(timerHeapInsert(&h, &pool[i]), STATUS_OK);
            }
        } else if (op == 2) {
            if (pool[i].state == TIMER_ARMED) {
                timerHeapRemove(&h, &pool[i]);
                pool[i].state = TIMER_IDLE;
            }
        } else {
            TimerObj *best = NULL;
            for (uint32_t k = 0; k < TIMER_HEAP_CAP; k++) {
                if (pool[k].state != TIMER_ARMED) {
                    continue;
                }
                if (best == NULL || pool[k].deadlineNs < best->deadlineNs ||
                    (pool[k].deadlineNs == best->deadlineNs && pool[k].seq < best->seq)) {
                    best = &pool[k];
                }
            }
            ASSERT_TRUE(timerHeapPeek(&h) == best);
            TimerObj *got = timerHeapPop(&h);
            ASSERT_TRUE(got == best);
            if (got != NULL) {
                got->state = TIMER_IDLE;
            }
        }
        ASSERT_TRUE(timerHeapCheck(&h));
    }
}
