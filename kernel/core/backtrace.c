/* See backtrace.h. */
#include "backtrace.h"

#include "format.h"
#include "klog.h"
#include "ksym.h"
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

/* `isReturnAddr`: a return address points *after* the `call`, which for a noreturn call (e.g.
 * panic() or __stack_chk_fail()) can be the first byte of the *next* function -- so frame >= 1
 * looks up `addr - 1` instead of `addr` itself, to symbolize against the calling function, not
 * whatever happens to follow it. The printed offset is still computed from the real `addr`, not
 * the lookup address, so it reads as "this many bytes past the call/branch", not "one byte less".
 * Frame #0 (the trap path's own faulting RIP) is the exact instruction address, so it's looked up
 * as-is (isReturnAddr = false). */
static void printFrame(int index, uint64_t addr, bool isReturnAddr) {
    uint64_t lookupAddr = (isReturnAddr && addr > 0) ? addr - 1 : addr;
    char name[64];
    uint64_t symAddr = 0;
    size_t blobSize = (size_t)(ksymsEnd - ksymsStart);
    Status st = ksymDecodeLookup(ksymsStart, blobSize, lookupAddr, name, sizeof(name), &symAddr);

    char line[128];
    if (st == STATUS_OK) {
        ksnprintf(line, sizeof(line), "  #%d 0x%016llx %s+0x%llx\n", index,
                  (unsigned long long)addr, name, (unsigned long long)(addr - symAddr));
    } else {
        ksnprintf(line, sizeof(line), "  #%d 0x%016llx ?\n", index, (unsigned long long)addr);
    }
    klogRaw(line);
}

/* Shared frame-pointer walk: calls `cb(ctx, frameIndex, returnAddr)` for each frame found from
 * `fp` onward, starting the frame count at `startIndex`. Both backtracePrint() and
 * backtraceCapture() are built on this, so the stack-bounds-checking/anti-loop logic lives in
 * exactly one place. */
typedef void (*FrameCallback)(void *ctx, int index, uint64_t returnAddr);

static void walkFrames(uint64_t fp, int startIndex, FrameCallback cb, void *ctx) {
    StackRange ranges[BACKTRACE_STACK_COUNT];
    knownStacks(ranges);

    int prevStack = -1;
    uint64_t prevFp = 0;
    for (int frame = startIndex; frame < BACKTRACE_MAX_FRAMES && fp != 0; frame++) {
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
        cb(ctx, frame, returnAddr);

        prevStack = stack;
        prevFp = fp;
        fp = savedFp;
    }
}

static void printCallback(void *ctx, int index, uint64_t returnAddr) {
    (void)ctx;
    printFrame(index, returnAddr, true);
}

void backtracePrint(uint64_t pc, uint64_t fp) {
    int start = 0;
    if (pc != 0) {
        printFrame(0, pc, false);
        start = 1;
    }
    walkFrames(fp, start, printCallback, NULL);
}

typedef struct {
    uint64_t *out;
    size_t max;
    size_t count;
} CaptureCtx;

static void captureCallback(void *ctx, int index, uint64_t returnAddr) {
    (void)index;
    CaptureCtx *c = (CaptureCtx *)ctx;
    if (c->count < c->max) {
        c->out[c->count++] = returnAddr;
    }
}

size_t backtraceCapture(uint64_t fp, uint64_t *out, size_t max) {
    CaptureCtx ctx = {out, max, 0};
    walkFrames(fp, 0, captureCallback, &ctx);
    return ctx.count;
}
