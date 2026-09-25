/* See klog.h. */
#include "klog.h"

#include "branding.h"
#include "format.h"

#include <stdarg.h>
#include <stddef.h>

#include "drivers/fbcon/fbcon.h"
#include "drivers/serial/uart16550.h"

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
    serialWriteString(line);
    if (fbconActive()) {
        fbconSetColor(klogLevelColor(level), 0);
        fbconWrite(line, klogWrittenLen(written, sizeof(line)));
    }
}

void klogRaw(const char *s) {
    serialWriteString(s);
    if (fbconActive()) {
        size_t n = 0;
        while (s[n] != '\0') {
            n++;
        }
        fbconSetColor(7, 0);
        fbconWrite(s, n);
    }
}
