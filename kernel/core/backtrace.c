/* See backtrace.h. */
#include "backtrace.h"

#include "format.h"
#include "klog.h"
#include "sections.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACKTRACE_MAX_FRAMES  32
#define BACKTRACE_STACK_COUNT 4

typedef struct {
    uint64_t bottom, top;
} StackRange;

/* Boundaries of every stack a frame pointer can legitimately point into (ARCHITECTURE §7.1). */
static void knownStacks(StackRange out[BACKTRACE_STACK_COUNT]) {
    out[0].bottom = (uint64_t)(uintptr_t)kernelBootStackBottom;
    out[0].top = (uint64_t)(uintptr_t)kernelBootStackTop;
    out[1].bottom = (uint64_t)(uintptr_t)kernelIst1Bottom;
    out[1].top = (uint64_t)(uintptr_t)kernelIst1Top;
    out[2].bottom = (uint64_t)(uintptr_t)kernelIst2Bottom;
    out[2].top = (uint64_t)(uintptr_t)kernelIst2Top;
    out[3].bottom = (uint64_t)(uintptr_t)kernelIst3Bottom;
    out[3].top = (uint64_t)(uintptr_t)kernelIst3Top;
}

/* -1 if `fp` isn't inside any known stack with room for both the saved-fp and return-address
 * words (16 bytes), or isn't 8-aligned (every legitimate frame pointer is). */
static int stackIndexOf(uint64_t fp, const StackRange ranges[BACKTRACE_STACK_COUNT]) {
    if ((fp & 7) != 0) {
        return -1;
    }
    for (int i = 0; i < BACKTRACE_STACK_COUNT; i++) {
        if (fp >= ranges[i].bottom && fp <= ranges[i].top - 16) {
            return i;
        }
    }
    return -1;
}

static void printFrame(int index, uint64_t addr) {
    char line[64];
    ksnprintf(line, sizeof(line), "  #%d 0x%016llx\n", index, (unsigned long long)addr);
    klogRaw(line);
}

void backtracePrint(uint64_t pc, uint64_t fp) {
    StackRange ranges[BACKTRACE_STACK_COUNT];
    knownStacks(ranges);

    int frame = 0;
    if (pc != 0) {
        printFrame(0, pc);
        frame = 1;
    }

    int prevStack = -1;
    uint64_t prevFp = 0;
    for (; frame < BACKTRACE_MAX_FRAMES && fp != 0; frame++) {
        int stack = stackIndexOf(fp, ranges);
        if (stack < 0) {
            break;
        }
        if (prevStack == stack && fp <= prevFp) {
            break; /* same-stack chain must strictly increase, or it's corrupted/looping */
        }

        const uint64_t *frameWords = (const uint64_t *)(uintptr_t)fp;
        uint64_t savedFp = frameWords[0];
        uint64_t returnAddr = frameWords[1];
        printFrame(frame, returnAddr);

        prevStack = stack;
        prevFp = fp;
        fp = savedFp;
    }
}
