/* See klog.h. */
#include "klog.h"

#include "branding.h"
#include "format.h"

#include <stdarg.h>

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
    ksnprintf(line, sizeof(line), "[%s] %s: %s\n", klogLevelName(level), tag, message);
    serialWriteString(line);
}

void klogRaw(const char *s) {
    serialWriteString(s);
}
