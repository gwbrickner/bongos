/* See klog.h. */
#include "klog.h"

#include "branding.h"
#include "format.h"
#include "panic.h"
#include "preempt.h"
#include "spinlock.h"

#include <arch/cpu.h>
#include <stdarg.h>
#include <stddef.h>

#include "drivers/fbcon/fbcon.h"
#include "drivers/serial/uart16550.h"

/* klogLock serializes the sinks (UART, fbcon cursor/scroll state), which are not reentrant (D-173),
 * and is a LEAF: nothing called under it (serialWriteString, fbcon*) may take a Spinlock. It is
 * safe from an IRQ handler (irqsave). Once a panic is in progress it is bypassed, because the
 * panicking context may itself hold it (a UBSan trip or #PF inside fbconWrite, or an NMI): the
 * panic path only disables IRQs, as before M3.4 (D-188). */
static Spinlock klogLock = SPINLOCK_INIT("klog");

/* Begins/ends the sink section. `*locked` records whether klogLock was taken (false only during a
 * panic, or when this CPU is already inside the section: an exception that logs nested in it).
 * Returns the saved RFLAGS for klogOutputEnd(). */
static uint64_t klogOutputBegin(bool *locked) {
    if (panicInProgress()) {
        *locked = false;
        return archIrqSave();
    }
    CpuSync *s = cpuSync();
    if (s->klogHeld != 0) {
        /* This CPU is already inside the sink section with IRQs off, so only an exception or NMI
         * can be here: #BP (which logs and resumes) or the report of a caught fault. Spinning on
         * our own lock would hang; instead nest unlocked, as klog did before M3.4. */
        *locked = false;
        return archIrqSave();
    }
    uint64_t flags = spinLockIrqSave(&klogLock);
    s->klogHeld = 1;
    *locked = true;
    return flags;
}

static void klogOutputEnd(bool locked, uint64_t irqFlags) {
    if (locked) {
        cpuSync()->klogHeld = 0;
        spinUnlockIrqRestore(&klogLock, irqFlags);
    } else {
        archIrqRestore(irqFlags);
    }
}

static const char *klogLevelName(KlogLevel level) {
    switch (level) {
        case KLOG_ERROR:
            return "error";
        case KLOG_WARN:
            return "warn";
        case KLOG_INFO:
            return "info";
        case KLOG_DEBUG:
            return "debug";
        case KLOG_TRACE:
            return "trace";
        default:
            return "?";
    }
}

/* ANSI palette indices (D-069): error red, warn yellow, info/debug/trace the same as klogRaw's
 * plain color, dimmed for debug/trace. */
static uint8_t klogLevelColor(KlogLevel level) {
    switch (level) {
        case KLOG_ERROR:
            return 9;
        case KLOG_WARN:
            return 11;
        case KLOG_INFO:
            return 7;
        case KLOG_DEBUG:
        case KLOG_TRACE:
            return 8;
        default:
            return 7;
    }
}

/* ksnprintf() returns the length it *would* have written (snprintf semantics, can exceed `size`
 * on truncation); fbconWrite() needs the length actually sitting in the buffer, which is always
 * < size since ksnprintf NUL-terminates whenever size > 0. */
static size_t klogWrittenLen(int snprintfResult, size_t size) {
    if (snprintfResult < 0 || size == 0) {
        return 0;
    }
    return ((size_t)snprintfResult < size) ? (size_t)snprintfResult : size - 1;
}

void klogInit(void) {
    serialWriteString("\r\n");
    klogWrite(KLOG_INFO, "kernel", "%s %s (%s)", BRANDING_NAME, BRANDING_VERSION,
              BRANDING_CODENAME);
}

void klogWrite(KlogLevel level, const char *tag, const char *fmt, ...) {
    char message[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    char line[320];
    int written =
        ksnprintf(line, sizeof(line), "[%s] %s: %s\n", klogLevelName(level), tag, message);
    /* D-173/D-188: the sinks are not reentrant and an IRQ handler may log, so the whole output
     * section runs under klogLock with IRQs off. */
    bool locked;
    uint64_t irqFlags = klogOutputBegin(&locked);
    serialWriteString(line);
    if (fbconActive()) {
        fbconSetColor(klogLevelColor(level), 0);
        fbconWrite(line, klogWrittenLen(written, sizeof(line)));
    }
    klogOutputEnd(locked, irqFlags);
}

void klogRaw(const char *s) {
    bool locked;
    uint64_t irqFlags = klogOutputBegin(&locked); /* D-173/D-188: see klogWrite() */
    serialWriteString(s);
    if (fbconActive()) {
        size_t n = 0;
        while (s[n] != '\0') {
            n++;
        }
        fbconSetColor(7, 0);
        fbconWrite(s, n);
    }
    klogOutputEnd(locked, irqFlags);
}

void klogTestRunInSection(void (*fn)(void *), void *arg) {
    bool locked;
    uint64_t irqFlags = klogOutputBegin(&locked);
    fn(arg);
    klogOutputEnd(locked, irqFlags);
}
