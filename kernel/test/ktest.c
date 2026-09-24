/* See ktest.h. Implements ARCHITECTURE §23's wire protocol and exit-code mapping. */
#include "ktest.h"

#include "cmdline.h"
#include "format.h"
#include "klog.h"

#include <arch/qemu.h>
#include <stdarg.h>
#include <stdbool.h>

struct KtestCtx {
    const KtestCase *test;
    bool failed;
};

/* Defined by kernel.ld: the `.ktests` section holds one `const KtestCase *` per KTEST(), in link
 * order. */
extern const KtestCase *const ktestsStart[];
extern const KtestCase *const ktestsEnd[];

#define KTEST_MAX_PATTERNS 32

static bool ktestModeActive = false;
static const KtestCase *ktestRunning = NULL;

bool ktestIsActive(void) {
    return ktestModeActive;
}

const char *ktestCurrentName(void) {
    return ktestRunning != NULL ? ktestRunning->name : NULL;
}

void ktestFail(KtestCtx *ktestCtx, const char *file, uint32_t line, const char *fmt, ...) {
    ktestCtx->failed = true;

    char message[200];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    char wireLine[320];
    ksnprintf(wireLine, sizeof(wireLine), "KTEST FAIL %s: %s:%u: %s\n", ktestCtx->test->name, file,
              line, message);
    klogRaw(wireLine);
}

static uint32_t ktestTotalCount(void) {
    return (uint32_t)(ktestsEnd - ktestsStart);
}

void ktestRunFromCmdline(const char *cmdline) {
    char value[256];
    if (!cmdlineFindKtest(cmdline, value, sizeof(value))) {
        return;
    }
    ktestModeActive = true;

    bool isAll = cmdlineStrEq(value, "all");
    char *patterns[KTEST_MAX_PATTERNS];
    uint32_t patternCount = 0;
    bool patternsDropped = false;
    if (!isAll) {
        char *p = value;
        patterns[patternCount++] = p;
        /* Keep walking (and NUL-splitting at every comma) even past KTEST_MAX_PATTERNS, so a
         * pattern beyond the cap is cleanly dropped rather than silently swallowed -- with commas
         * intact -- into the last slot we did keep. */
        while (*p != '\0') {
            if (*p == ',') {
                *p = '\0';
                p++;
                if (patternCount < KTEST_MAX_PATTERNS) {
                    patterns[patternCount++] = p;
                } else {
                    patternsDropped = true;
                }
            } else {
                p++;
            }
        }
    }

    uint32_t totalTests = ktestTotalCount();
    uint32_t realPassed = 0;
    uint32_t realFailed = 0;
    uint32_t patternMisses = 0;
    if (patternsDropped) {
        char msg[80];
        ksnprintf(msg, sizeof(msg),
                  "KTEST FAIL ktest: more than %u ktest= patterns given; extra patterns dropped\n",
                  (unsigned)KTEST_MAX_PATTERNS);
        klogRaw(msg);
        patternMisses++;
    }

    /* Report unmatched patterns before running anything, per ARCHITECTURE §23. */
    if (isAll) {
        if (totalTests == 0) {
            klogRaw("KTEST FAIL ktest: pattern \"all\" matched no tests\n");
            patternMisses++;
        }
    } else {
        for (uint32_t pi = 0; pi < patternCount; pi++) {
            uint32_t matchCount = 0;
            for (uint32_t i = 0; i < totalTests; i++) {
                if (cmdlineGlobMatch(patterns[pi], ktestsStart[i]->name)) {
                    matchCount++;
                }
            }
            if (matchCount == 0) {
                char msg[288];
                ksnprintf(msg, sizeof(msg), "KTEST FAIL ktest: pattern \"%s\" matched no tests\n",
                          patterns[pi]);
                klogRaw(msg);
                patternMisses++;
            }
        }
    }

    for (uint32_t i = 0; i < totalTests; i++) {
        const KtestCase *tc = ktestsStart[i];
        bool matches = isAll;
        if (!matches) {
            for (uint32_t pi = 0; pi < patternCount; pi++) {
                if (cmdlineGlobMatch(patterns[pi], tc->name)) {
                    matches = true;
                    break;
                }
            }
        }
        if (!matches) {
            continue;
        }

        char startLine[160];
        ksnprintf(startLine, sizeof(startLine), "KTEST START %s\n", tc->name);
        klogRaw(startLine);

        ktestRunning = tc;
        KtestCtx ctx = {tc, false};
        tc->fn(&ctx);
        ktestRunning = NULL;

        if (ctx.failed) {
            realFailed++;
        } else {
            char passLine[160];
            ksnprintf(passLine, sizeof(passLine), "KTEST PASS %s\n", tc->name);
            klogRaw(passLine);
            realPassed++;
        }
    }

    char doneLine[96];
    ksnprintf(doneLine, sizeof(doneLine), "KTEST DONE passed=%u failed=%u\n", realPassed,
              realFailed + patternMisses);
    klogRaw(doneLine);

    bool allPass = (realPassed >= 1) && (realFailed == 0) && (patternMisses == 0);
    archDebugExit(allPass ? 0x10 : 0x11);
}
