/* Host tests for the ticket-lock core (kernel/include/spinlock-raw.h, M3.4, D-183): trylock
 * semantics, mutual exclusion under real threads, and FIFO hand-off order. */
#include "framework/test.h"
#include "spinlock-raw.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>

TEST(rawSpinTryLockSemantics) {
    RawSpinlock l = RAW_SPINLOCK_INIT;
    ASSERT_TRUE(!rawSpinIsLocked(&l));
    ASSERT_TRUE(rawSpinTryLock(&l));
    ASSERT_TRUE(rawSpinIsLocked(&l));
    uint32_t nextBefore = l.next;
    ASSERT_TRUE(!rawSpinTryLock(&l));
    ASSERT_EQ(l.next, nextBefore); /* a failed trylock consumes no ticket */
    rawSpinUnlock(&l);
    ASSERT_TRUE(!rawSpinIsLocked(&l));
    ASSERT_TRUE(rawSpinTryLock(&l));
    rawSpinUnlock(&l);
}

TEST(rawSpinLockUnlockCounters) {
    RawSpinlock l = RAW_SPINLOCK_INIT;
    rawSpinLock(&l);
    ASSERT_EQ(l.next, 1u);
    ASSERT_EQ(l.owner, 0u);
    rawSpinUnlock(&l);
    ASSERT_EQ(l.owner, 1u);
    /* Counter wrap-around: free iff next == owner, whatever the values. */
    l.next = 0xFFFFFFFFu;
    l.owner = 0xFFFFFFFFu;
    ASSERT_TRUE(rawSpinTryLock(&l));
    ASSERT_EQ(l.next, 0u);
    ASSERT_TRUE(rawSpinIsLocked(&l));
    rawSpinUnlock(&l);
    ASSERT_EQ(l.owner, 0u);
    ASSERT_TRUE(!rawSpinIsLocked(&l));
}

#define MUTEX_THREADS 4
#define MUTEX_ITERS   200000

static RawSpinlock mutexLock = RAW_SPINLOCK_INIT;
static uint64_t mutexCounter;

static void *mutexWorker(void *arg) {
    (void)arg;
    for (int i = 0; i < MUTEX_ITERS; i++) {
        rawSpinLock(&mutexLock);
        mutexCounter++; /* plain, non-atomic: only the lock protects it */
        rawSpinUnlock(&mutexLock);
    }
    return NULL;
}

TEST(rawSpinLockMutualExclusion) {
    pthread_t th[MUTEX_THREADS];
    mutexCounter = 0;
    mutexLock = (RawSpinlock)RAW_SPINLOCK_INIT;
    for (int i = 0; i < MUTEX_THREADS; i++) {
        ASSERT_EQ(pthread_create(&th[i], NULL, mutexWorker, NULL), 0);
    }
    for (int i = 0; i < MUTEX_THREADS; i++) {
        ASSERT_EQ(pthread_join(th[i], NULL), 0);
    }
    ASSERT_EQ(mutexCounter, (uint64_t)MUTEX_THREADS * MUTEX_ITERS);
}

static RawSpinlock fifoLock = RAW_SPINLOCK_INIT;
static uint32_t fifoOrder[2];
static uint32_t fifoCount;

static void *fifoWorker(void *arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    rawSpinLock(&fifoLock);
    fifoOrder[fifoCount++] = id;
    rawSpinUnlock(&fifoLock);
    return NULL;
}

TEST(rawSpinLockIsFifo) {
    fifoLock = (RawSpinlock)RAW_SPINLOCK_INIT;
    fifoCount = 0;
    rawSpinLock(&fifoLock); /* main holds ticket 0 */
    pthread_t t1, t2;
    ASSERT_EQ(pthread_create(&t1, NULL, fifoWorker, (void *)(uintptr_t)1), 0);
    while (ATOMIC_LOAD(&fifoLock.next, MEM_ACQUIRE) != 2u) {
        sched_yield();
    }
    ASSERT_EQ(pthread_create(&t2, NULL, fifoWorker, (void *)(uintptr_t)2), 0);
    while (ATOMIC_LOAD(&fifoLock.next, MEM_ACQUIRE) != 3u) {
        sched_yield();
    }
    rawSpinUnlock(&fifoLock);
    ASSERT_EQ(pthread_join(t1, NULL), 0);
    ASSERT_EQ(pthread_join(t2, NULL), 0);
    ASSERT_EQ(fifoCount, 2u);
    ASSERT_EQ(fifoOrder[0], 1u);
    ASSERT_EQ(fifoOrder[1], 2u);
}
