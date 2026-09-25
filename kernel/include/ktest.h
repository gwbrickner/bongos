/* The in-kernel test framework (ARCHITECTURE §23). A KTEST() registers a test case by placing a
 * pointer to it in the `.ktests` linker section (kernel.ld); ktestRunFromCmdline() walks that
 * section when the command line has `ktest=`. */
#ifndef KERNEL_KTEST_H
#define KERNEL_KTEST_H

#include <arch/jmp.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct KtestCtx KtestCtx; /* opaque; defined in kernel/test/ktest.c */
typedef void (*KtestFn)(KtestCtx *ktestCtx);

typedef struct KtestCase {
    const char *name;
    KtestFn fn;
    const char *file;
    uint32_t line;
    uint32_t reserved;
} KtestCase;

/* Defines and registers a test case named `testName` (a bare identifier, not a string). The body
 * follows the macro like a function body and receives `ktestCtx` implicitly (used by
 * KTEST_ASSERT*). Registration works by pointer, not by embedding the struct:
 * `ktestCase_##testName` itself is a plain (non-static) `const KtestCase`, with external linkage
 * and a real address, and the `.ktests` section holds a `static const KtestCase *const` pointing at
 * it. Each translation unit's section entry is therefore just one pointer-sized, pointer-aligned
 * slot -- entries from different object files never get padding between them, which is what keeps
 * the linker-array walk
 * (`ktestsStart`/`ktestsEnd`, D-063) safe -- see kernel/test/bootinfo_test.c for example usage. */
#define KTEST(testName)                                                                            \
    static void ktestFn_##testName(KtestCtx *ktestCtx);                                            \
    const KtestCase ktestCase_##testName = {#testName, ktestFn_##testName, __FILE__, __LINE__, 0}; \
    __attribute__((used,                                                                           \
                   section(".ktests"))) static const KtestCase *const ktestEntry_##testName =      \
        &ktestCase_##testName;                                                                     \
    static void ktestFn_##testName(KtestCtx *ktestCtx)

/* Records a failure against the currently running test and returns from the calling function
 * (via the KTEST_ASSERT* macros below); does not stop the rest of the ktest run. Prints
 * "KTEST FAIL <name>: <file>:<line>: <message>" on serial, per the wire protocol (ARCHITECTURE
 * §23). No locks, boot-time/ktest-only. */
void ktestFail(KtestCtx *ktestCtx, const char *file, uint32_t line, const char *fmt, ...);

#define KTEST_ASSERT(cond)                                                                         \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ktestFail(ktestCtx, __FILE__, __LINE__, "%s", #cond);                                  \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define KTEST_ASSERT_EQ(a, b)                                                                      \
    do {                                                                                           \
        uint64_t ktA_ = (uint64_t)(a);                                                             \
        uint64_t ktB_ = (uint64_t)(b);                                                             \
        if (ktA_ != ktB_) {                                                                        \
            ktestFail(ktestCtx, __FILE__, __LINE__, "%s == %s (0x%llx != 0x%llx)", #a, #b, ktA_,   \
                      ktB_);                                                                       \
            return;                                                                                \
        }                                                                                          \
    } while (0)

/* Runs the tests selected by the `ktest=` token on `cmdline` (ARCHITECTURE §23's protocol: START/
 * PASS/FAIL lines per test, then one DONE line), and calls archDebugExit() -- never returning --
 * if `ktest=` was present. Returns normally (running nothing) if it wasn't. No locks, boot-time
 * only; called once, after klogInit(). */
void ktestRunFromCmdline(const char *cmdline);

/* True once ktestRunFromCmdline() has determined `ktest=` was present (even before any test has
 * actually run) -- panic() uses this to decide whether a panic should report a KTEST FAIL and
 * exit rather than just halt. No locks, boot-time only. */
bool ktestIsActive(void);

/* The name of the test currently executing, or NULL if none is (either ktest mode is off, or
 * we're between tests / doing pre-run pattern checks). No locks, boot-time only. */
const char *ktestCurrentName(void);

/* Expected-panic recovery (D-076). Arms a match on `prefix` against the next panic()/panicTrap()
 * message; a matching panic makes panic.c jump straight back into the ktest via
 * archJmpSave()/archJmpRestore() (kernel/include/arch/jmp.h). Two calls, both required, in this
 * exact shape, directly in the KTEST() body (never through a wrapper function -- same
 * restriction as setjmp, and for the same reason: a wrapper's own "ret" would read a
 * return-address stack slot that a sibling call in between could clobber, since the recovery
 * jump only guarantees the *jump itself* lands correctly, not what any intervening function's
 * own epilogue later reads off the stack):
 *
 *   ktestArmExpectedPanic(ktestCtx, "expected panic message prefix");
 *   if (archJmpSave(ktestPanicJmpBuf()) == 0) {
 *       triggerThePanic();
 *       KTEST_ASSERT(false); // unreachable
 *   } else {
 *       // recovered: verify whatever state the panic should have left behind
 *   }
 *
 * If the test function returns without the expected panic ever firing, ktestRunFromCmdline()
 * fails it ("expected panic did not occur") and disarms the match. No locks, ktest-only (a no-op
 * outside a running ktest). */
void ktestArmExpectedPanic(KtestCtx *ktestCtx, const char *prefix);

/* The single shared jump buffer archJmpSave()/archJmpRestore() use for expected-panic recovery.
 * Never call archJmpSave() through anything but this buffer, and never save more than one
 * expected panic at a time (ktests run one at a time, so this is never contended). */
ArchJmpBuf *ktestPanicJmpBuf(void);

/* Called by panic.c's shared panic implementation before printing anything: true if `message`
 * matches an armed ktestExpectPanic() prefix (and disarms it). panic.c must follow a `true`
 * result with ktestPanicRecover() and never fall through to the normal fatal path. Not for
 * ktests to call directly. */
bool ktestPanicExpected(const char *message);

/* Jumps back to the matching ktestExpectPanic() call site (making it return 1). Never returns.
 * Only valid to call immediately after ktestPanicExpected() returns true. Not for ktests to call
 * directly. */
_Noreturn void ktestPanicRecover(void);

#endif
