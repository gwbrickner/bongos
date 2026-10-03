/* Adversarial SMP ktests added by the M3.5 bug sweep: the per-CPU slab magazines under concurrent
 * use (D-199, D-200), concurrent vmalloc/vfree and their TLB shootdowns (D-196), the CPU-local
 * timer and trap-catch rules (D-190, D-199) and smpWorkPost()'s misuse checks (D-202). Work on an
 * AP reports through shared memory; the BSP asserts (D-202). */
#include "atomic.h"
#include "cpu-local.h"
#include "kmalloc.h"
#include "ktest.h"
#include "pmm.h"
#include "smp.h"
#include "timekeeping.h"
#include "vmalloc.h"

#include <arch/cpu.h>
#include <arch/trap.h>
#include <stdbool.h>
#include <stdint.h>

static uint32_t sweepRnd(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return (uint32_t)(x >> 11);
}

/* --- slab: a cache's magazines on other CPUs ---------------------------------------------------
 */

#define REMOTE_OBJS 40u

static SlabCache *remoteCache;
static void *remoteObjs[REMOTE_OBJS];
static volatile uint32_t remoteAllocFailed;

static void remoteAllocFreeFn(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < REMOTE_OBJS; i++) {
        remoteObjs[i] = slabAlloc(remoteCache, 0);
        if (remoteObjs[i] == NULL) {
            remoteAllocFailed = 1;
        }
    }
    for (uint32_t i = 0; i < REMOTE_OBJS; i++) {
        slabFree(remoteCache, remoteObjs[i]);
    }
}

/* Objects freed on another CPU sit in THAT CPU's magazine: the stats count them as cached, and
 * destroying the cache from this CPU flushes them back (no "cache busy"), returning every page. */
KTEST(smp_slab_destroy_flushes_remote_magazines) {
    if (smpOnlineCount() < 2) {
        return;
    }
    slabShrinkAll();
    pmmDrainAllCaches();
    PmmStats before;
    pmmGetStats(&before);
    KTEST_ASSERT(slabCacheCreate("sweep-remote", 200, 0, NULL, NULL, &remoteCache) == STATUS_OK);
    remoteAllocFailed = 0;
    SmpWork w = {.fn = remoteAllocFreeFn, .arg = NULL, .remaining = 1};
    smpWorkPost(1, &w);
    smpWorkWait(&w);
    KTEST_ASSERT_EQ(remoteAllocFailed, 0);
    SlabCacheStats st;
    slabCacheGetStats(remoteCache, &st);
    KTEST_ASSERT(st.objsCached > 0); /* cpu 1's magazine, seen from cpu 0 */
    KTEST_ASSERT_EQ(st.objsAllocated, 0);
    slabCacheDestroy(remoteCache); /* slabBug(CACHE_BUSY) if cpu 1's magazine were left alone */
    remoteCache = NULL;
    pmmDrainAllCaches();
    PmmStats after;
    pmmGetStats(&after);
    KTEST_ASSERT_EQ(after.freePages, before.freePages);
}

/* --- slab: every CPU at once, with cross-CPU frees and frees from handlers ----------------------
 */

#define SLAB_STRESS_OPS   4000u
#define SLAB_STRESS_LIVE  32u
#define SLAB_STRESS_XCHG  64u
#define SLAB_IRQ_PERIOD   200000ull /* the per-CPU timer that allocates and frees from a handler */
#define SLAB_IRQ_OBJ_SIZE 96u

typedef struct {
    uint64_t seed;
    uint32_t bad, ops, irqRuns, irqBad;
    TimerObj timer;
    volatile uint32_t stopTimer;
    void *irqHeld; /* an object the handler keeps across runs, freed on its next run */
} SlabStressCpu;

static SlabStressCpu slabStress[CPU_MAX];
static void *volatile slabExchange[SLAB_STRESS_XCHG];

static uint64_t slabStamp(uint32_t cpu, uint32_t slot, uint32_t size) {
    return ((uint64_t)cpu << 56) | ((uint64_t)slot << 32) | size | 0x5A00000000000000ull;
}

static void slabStressTimer(TimerObj *t, void *ctx) {
    SlabStressCpu *s = ctx;
    /* D-200: a handler may kmalloc/kfree; here it races the thread loop below for the very same
     * size class (and magazine) on this CPU, including while that loop has dropped the slab lock to
     * grow or release a slab. */
    if (s->irqHeld != NULL) {
        if (*(volatile uint64_t *)s->irqHeld != (uint64_t)(uintptr_t)s->irqHeld) {
            s->irqBad++;
        }
        kfree(s->irqHeld);
        s->irqHeld = NULL;
    }
    void *p = kmalloc(SLAB_IRQ_OBJ_SIZE, 0);
    if (p == NULL) {
        s->irqBad++;
    } else {
        *(volatile uint64_t *)p = (uint64_t)(uintptr_t)p;
        s->irqHeld = p;
    }
    s->irqRuns++;
    if (!s->stopTimer) {
        (void)timerArm(t, timeMonotonicNs() + SLAB_IRQ_PERIOD);
    }
}

typedef struct {
    uint64_t *p;
    uint32_t size;
} SlabLive;

static void slabStressFn(void *arg) {
    (void)arg;
    uint32_t cpu = smpThisCpu();
    SlabStressCpu *s = &slabStress[cpu];
    s->seed = 0xD1B54A32D192ED03ULL * (cpu + 3);
    timerInit(&s->timer, slabStressTimer, s);
    (void)timerArm(&s->timer, timeMonotonicNs() + SLAB_IRQ_PERIOD);
    SlabLive live[SLAB_STRESS_LIVE] = {0};
    for (uint32_t op = 0; op < SLAB_STRESS_OPS; op++) {
        uint32_t slot = sweepRnd(&s->seed) % SLAB_STRESS_LIVE;
        if (live[slot].p != NULL) {
            uint64_t want = slabStamp(cpu, slot, live[slot].size);
            uint32_t words = live[slot].size / 8u;
            for (uint32_t i = 0; i < words; i++) {
                if (live[slot].p[i] != want) {
                    s->bad++;
                    break;
                }
            }
            if ((sweepRnd(&s->seed) & 3u) == 0) {
                /* Hand it to whoever comes next; free what was parked (another CPU's, unchecked
                 * since its stamp names that CPU). */
                uint32_t x = sweepRnd(&s->seed) % SLAB_STRESS_XCHG;
                void *other = __atomic_exchange_n(&slabExchange[x], live[slot].p, __ATOMIC_ACQ_REL);
                kfree(other);
            } else {
                kfree(live[slot].p);
            }
            live[slot].p = NULL;
        } else {
            static const uint32_t sizes[] = {16, 40, 96, 200, 512, 1024, 4096};
            uint32_t size = sizes[sweepRnd(&s->seed) % (sizeof(sizes) / sizeof(sizes[0]))];
            uint64_t *p = kmalloc(size, 0);
            if (p == NULL) {
                continue;
            }
            uint64_t stamp = slabStamp(cpu, slot, size);
            for (uint32_t i = 0; i < size / 8u; i++) {
                p[i] = stamp;
            }
            live[slot].p = p;
            live[slot].size = size;
        }
        s->ops++;
    }
    for (uint32_t i = 0; i < SLAB_STRESS_LIVE; i++) {
        kfree(live[i].p);
    }
    /* Stop the handler on this CPU (timers are CPU-local, D-199) and free what it holds. */
    s->stopTimer = 1;
    uint64_t f = archIrqSave();
    (void)timerCancel(&s->timer);
    void *held = s->irqHeld;
    s->irqHeld = NULL;
    archIrqRestore(f);
    kfree(held);
}

/* Every CPU kmallocs and kfrees mixed sizes at once, a quarter of them freed by a different CPU,
 * while a timer handler on each CPU allocates and frees in the same size classes. No object may be
 * overwritten, nothing may be reported, and with every magazine flushed the pmm is back where it
 * was. */
KTEST(smp_slab_concurrent_stress) {
    slabShrinkAll();
    pmmDrainAllCaches();
    PmmStats before;
    pmmGetStats(&before);
    for (uint32_t i = 0; i < CPU_MAX; i++) {
        slabStress[i] = (SlabStressCpu){0};
    }
    for (uint32_t i = 0; i < SLAB_STRESS_XCHG; i++) {
        slabExchange[i] = NULL;
    }
    smpWorkRun(smpOnlineMask(), slabStressFn, NULL);
    for (uint32_t i = 0; i < SLAB_STRESS_XCHG; i++) {
        kfree(slabExchange[i]);
        slabExchange[i] = NULL;
    }
    uint32_t bad = 0, ops = 0, irqRuns = 0, irqBad = 0;
    for (uint32_t i = 0; i < smpOnlineCount(); i++) {
        bad += slabStress[i].bad;
        ops += slabStress[i].ops;
        irqRuns += slabStress[i].irqRuns;
        irqBad += slabStress[i].irqBad;
    }
    slabShrinkAll();
    pmmDrainAllCaches();
    PmmStats after;
    pmmGetStats(&after);
    KTEST_ASSERT_EQ(bad, 0);
    KTEST_ASSERT_EQ(irqBad, 0);
    KTEST_ASSERT_EQ(ops, smpOnlineCount() * SLAB_STRESS_OPS);
    KTEST_ASSERT(irqRuns >= smpOnlineCount()); /* the handlers really ran */
    KTEST_ASSERT_EQ(after.freePages, before.freePages);
}

/* --- vmalloc/vfree on every CPU at once (D-196) --------------------------------------------------
 */

#define VM_STRESS_ROUNDS 60u

typedef struct {
    uint64_t seed;
    uint32_t bad, rounds;
} VmStressCpu;

static VmStressCpu vmStress[CPU_MAX];

static void vmStressFn(void *arg) {
    (void)arg;
    uint32_t cpu = smpThisCpu();
    VmStressCpu *s = &vmStress[cpu];
    s->seed = 0x2545F4914F6CDD1DULL * (cpu + 7);
    for (uint32_t r = 0; r < VM_STRESS_ROUNDS; r++) {
        /* 1..80 pages: some areas take two release chunks (64 pages), so a vfree waits for two
         * shootdown rounds while other CPUs are doing the same. */
        uint32_t pages = 1 + sweepRnd(&s->seed) % 80u;
        uint64_t *p = vmalloc((size_t)pages * 4096, 0);
        if (p == NULL) {
            s->bad++;
            continue;
        }
        uint64_t stamp = ((uint64_t)cpu << 48) | r;
        for (uint32_t pg = 0; pg < pages; pg++) {
            p[pg * 512] = stamp ^ pg;
            p[pg * 512 + 511] = ~(stamp ^ pg);
        }
        for (uint32_t pg = 0; pg < pages; pg++) {
            if (p[pg * 512] != (stamp ^ pg) || p[pg * 512 + 511] != ~(stamp ^ pg)) {
                s->bad++;
                break;
            }
        }
        vfree(p);
        s->rounds++;
    }
}

/* Every CPU vmallocs, fills, checks and vfrees at once: each vfree waits for a shootdown answered
 * by CPUs that are themselves inside vmalloc/vfree (and their own shootdowns). Nothing may
 * deadlock, no area may see another's data (a stale TLB entry for a reused KVA range would), and
 * the pmm ends where it started. */
KTEST(smp_vmalloc_concurrent_stress) {
    /* The first use of a fresh stretch of KVA can allocate page-table pages the vmm never frees
     * (as in vmalloc_map_free_no_leak): take that cost before the snapshot. First-fit KVA keeps
     * the areas below inside this stretch (4 CPUs x 80 pages is far less than 8 MiB). */
    void *warm = vmalloc(8 * 1024 * 1024, 0);
    KTEST_ASSERT(warm != NULL);
    vfree(warm);
    pmmDrainAllCaches();
    slabShrinkAll();
    PmmStats before;
    pmmGetStats(&before);
    for (uint32_t i = 0; i < CPU_MAX; i++) {
        vmStress[i] = (VmStressCpu){0};
    }
    smpWorkRun(smpOnlineMask(), vmStressFn, NULL);
    uint32_t bad = 0, rounds = 0;
    for (uint32_t i = 0; i < smpOnlineCount(); i++) {
        bad += vmStress[i].bad;
        rounds += vmStress[i].rounds;
    }
    slabShrinkAll();
    pmmDrainAllCaches();
    PmmStats after;
    pmmGetStats(&after);
    KTEST_ASSERT_EQ(bad, 0);
    KTEST_ASSERT_EQ(rounds, smpOnlineCount() * VM_STRESS_ROUNDS);
    KTEST_ASSERT_EQ(after.freePages, before.freePages);
}

/* --- timers are CPU-local (D-199) ----------------------------------------------------------------
 */

static TimerObj remoteTimer;
static volatile uint32_t remoteTimerFired;

static void remoteTimerCb(TimerObj *t, void *ctx) {
    (void)t;
    (void)ctx;
    remoteTimerFired = 1;
}

static void remoteTimerArmFn(void *arg) {
    (void)arg;
    timerInit(&remoteTimer, remoteTimerCb, NULL);
    (void)timerArm(&remoteTimer, timeMonotonicNs() + 60000000000ull); /* a minute: stays queued */
}

static void remoteTimerCancelFn(void *arg) {
    Status *st = arg;
    *st = timerCancel(&remoteTimer);
}

static void remoteCancelTrigger(void *arg) {
    (void)arg;
    (void)timerCancel(&remoteTimer);
}

static void remoteRearmTrigger(void *arg) {
    (void)arg;
    (void)timerArm(&remoteTimer, timeMonotonicNs() + 1000000ull);
}

/* A timer queued on cpu 1 cannot be cancelled or re-armed from cpu 0 (panicBug, caught), and the
 * refused calls leave it queued on cpu 1, where it can still be cancelled. */
KTEST(smp_timer_cross_cpu_misuse_caught) {
    if (smpOnlineCount() < 2) {
        return;
    }
    remoteTimerFired = 0;
    SmpWork w = {.fn = remoteTimerArmFn, .arg = NULL, .remaining = 1};
    smpWorkPost(1, &w);
    smpWorkWait(&w);
    KTEST_ASSERT(timerIsArmed(&remoteTimer));
    TrapCatchInfo info;
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, remoteCancelTrigger, NULL, &info));
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, remoteRearmTrigger, NULL, &info));
    KTEST_ASSERT(timerIsArmed(&remoteTimer));
    Status st = STATUS_ERR_INVALID;
    w = (SmpWork){.fn = remoteTimerCancelFn, .arg = &st, .remaining = 1};
    smpWorkPost(1, &w);
    smpWorkWait(&w);
    KTEST_ASSERT_EQ(st, STATUS_OK);
    KTEST_ASSERT(!timerIsArmed(&remoteTimer));
    KTEST_ASSERT_EQ(remoteTimerFired, 0);
}

/* --- archTrapCatch belongs to the CPU that armed it (D-190) --------------------------------------
 */

static volatile uint32_t foreignClaimed, foreignRan;

static void foreignClaimFn(void *arg) {
    (void)arg;
    /* What panicBug() on this CPU would do first: a catch armed by cpu 0 must not claim it (if it
     * did, this CPU would longjmp onto cpu 0's stack). */
    foreignClaimed = archTrapCatchSoftware(TRAP_CATCH_KERNEL_BUG, 0) ? 1u : 0u;
    foreignRan = 1;
}

static void catchBodyRunsForeign(void *arg) {
    (void)arg;
    SmpWork w = {.fn = foreignClaimFn, .arg = NULL, .remaining = 1};
    smpWorkPost(1, &w);
    smpWorkWait(&w);
}

KTEST(smp_trap_catch_ignores_other_cpus) {
    if (smpOnlineCount() < 2) {
        return;
    }
    foreignClaimed = 0;
    foreignRan = 0;
    TrapCatchInfo info;
    bool caught = archTrapCatch(TRAP_CATCH_KERNEL_BUG, catchBodyRunsForeign, NULL, &info);
    KTEST_ASSERT(!caught);
    KTEST_ASSERT_EQ(foreignRan, 1);
    KTEST_ASSERT_EQ(foreignClaimed, 0);
}

/* --- smpWorkPost() misuse (D-202) ----------------------------------------------------------------
 */

static volatile uint32_t holdRelease;

static void holdFn(void *arg) {
    (void)arg;
    while (!holdRelease) {
        archPause();
    }
}

static SmpWork postSecond;

static void postTwiceTrigger(void *arg) {
    (void)arg;
    postSecond = (SmpWork){.fn = holdFn, .arg = NULL, .remaining = 1};
    smpWorkPost(1, &postSecond);
}

static void postSelfTrigger(void *arg) {
    (void)arg;
    static SmpWork w;
    w = (SmpWork){.fn = holdFn, .arg = NULL, .remaining = 1};
    smpWorkPost(smpThisCpu(), &w);
}

static void postOfflineTrigger(void *arg) {
    (void)arg;
    static SmpWork w;
    w = (SmpWork){.fn = holdFn, .arg = NULL, .remaining = 1};
    smpWorkPost(CPU_MAX - 1, &w);
}

/* Posting to a CPU whose earlier item has not been taken yet, to the caller itself, or to an
 * offline CPU is refused (panicBug, caught), and the refused post leaves the pending one intact. */
KTEST(smp_work_post_misuse_caught) {
    TrapCatchInfo info;
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, postSelfTrigger, NULL, &info));
    if (smpOnlineCount() < CPU_MAX) {
        KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, postOfflineTrigger, NULL, &info));
    }
    if (smpOnlineCount() < 2) {
        return;
    }
    /* Keep cpu 1 busy in a first item so a second one stays pending in its slot. */
    holdRelease = 0;
    SmpWork first = {.fn = holdFn, .arg = NULL, .remaining = 1};
    smpWorkPost(1, &first);
    uint64_t deadline = timeMonotonicNs() + 2000000000ull;
    while (ATOMIC_LOAD(&cpuLocalOf(1)->work, MEM_ACQUIRE) != NULL && timeMonotonicNs() < deadline) {
        archPause(); /* wait until cpu 1 took `first` */
    }
    SmpWork pending = {.fn = holdFn, .arg = NULL, .remaining = 1};
    smpWorkPost(1, &pending);
    KTEST_ASSERT(archTrapCatch(TRAP_CATCH_KERNEL_BUG, postTwiceTrigger, NULL, &info));
    KTEST_ASSERT(ATOMIC_LOAD(&cpuLocalOf(1)->work, MEM_ACQUIRE) == &pending);
    holdRelease = 1;
    smpWorkWait(&first);
    smpWorkWait(&pending);
}

/* --- the caches really are per CPU (ROADMAP M3.5 item 5, D-199) ----------------------------------
 */

static Page *perCpuPage;
static void *perCpuObj;
static SlabCache *perCpuCache;

static void apPageRoundTripFn(void *arg) {
    (void)arg;
    if (pmmAllocPages(0, 0, &perCpuPage) == STATUS_OK) {
        pmmFreePages(perCpuPage, 0); /* now the head of cpu 1's own cache */
    } else {
        perCpuPage = NULL;
    }
}

/* A page freed on cpu 1 stays in cpu 1's cache: allocations on cpu 0 (whose cache starts empty, so
 * it refills from the buddy allocator) never get it, while a shared cache would hand it straight
 * back (LIFO). */
KTEST(smp_pmm_cache_is_per_cpu) {
    if (smpOnlineCount() < 2) {
        return;
    }
    pmmDrainAllCaches();
    perCpuPage = NULL;
    SmpWork w = {.fn = apPageRoundTripFn, .arg = NULL, .remaining = 1};
    smpWorkPost(1, &w);
    smpWorkWait(&w);
    KTEST_ASSERT(perCpuPage != NULL);
    PmmStats st;
    pmmGetStats(&st);
    KTEST_ASSERT(st.cachedPages > 0); /* cpu 1's cache, counted from cpu 0 */
    Page *got[64];
    uint32_t n = 0;
    bool sawIt = false;
    for (; n < 64; n++) {
        if (pmmAllocPages(0, 0, &got[n]) != STATUS_OK) {
            break;
        }
        sawIt = sawIt || got[n] == perCpuPage;
    }
    for (uint32_t i = 0; i < n; i++) {
        pmmFreePages(got[i], 0);
    }
    pmmDrainAllCaches();
    pmmGetStats(&st);
    KTEST_ASSERT_EQ(n, 64);
    KTEST_ASSERT(!sawIt);
    KTEST_ASSERT_EQ(st.cachedPages, 0); /* pmmDrainAllCaches() reached cpu 1's cache too */
}

static void apObjRoundTripFn(void *arg) {
    (void)arg;
    perCpuObj = slabAlloc(perCpuCache, 0);
    if (perCpuObj != NULL) {
        slabFree(perCpuCache, perCpuObj); /* now on top of cpu 1's magazine */
    }
}

/* The same for the slab magazines: an object freed on cpu 1 is never handed out on cpu 0. */
KTEST(smp_slab_magazine_is_per_cpu) {
    if (smpOnlineCount() < 2) {
        return;
    }
    KTEST_ASSERT(slabCacheCreate("sweep-percpu", 128, 0, NULL, NULL, &perCpuCache) == STATUS_OK);
    perCpuObj = NULL;
    SmpWork w = {.fn = apObjRoundTripFn, .arg = NULL, .remaining = 1};
    smpWorkPost(1, &w);
    smpWorkWait(&w);
    KTEST_ASSERT(perCpuObj != NULL);
    void *got[64];
    uint32_t n = 0;
    bool sawIt = false;
    for (; n < 64; n++) {
        got[n] = slabAlloc(perCpuCache, 0);
        if (got[n] == NULL) {
            break;
        }
        sawIt = sawIt || got[n] == perCpuObj;
    }
    for (uint32_t i = 0; i < n; i++) {
        slabFree(perCpuCache, got[i]);
    }
    slabCacheDestroy(perCpuCache);
    perCpuCache = NULL;
    KTEST_ASSERT_EQ(n, 64);
    KTEST_ASSERT(!sawIt);
}
