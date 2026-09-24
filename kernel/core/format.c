/* See format.h. */
#include "format.h"

#include <stdbool.h>
#include <stdint.h>

static size_t appendChar(char *buf, size_t size, size_t pos, char c) {
    if (pos < size) {
        buf[pos] = c;
    }
    return pos + 1;
}

static size_t appendStr(char *buf, size_t size, size_t pos, const char *s) {
    for (; *s != '\0'; s++) {
        pos = appendChar(buf, size, pos, *s);
    }
    return pos;
}

static size_t appendUint(char *buf, size_t size, size_t pos, uint64_t value, int base, int upper,
                         int width, int zeroPad) {
    char digits[32];
    int n = 0;
    if (value == 0) {
        digits[n++] = '0';
    }
    while (value > 0) {
        int d = (int)(value % (uint64_t)base);
        value /= (uint64_t)base;
        digits[n++] = (char)((d < 10) ? ('0' + d) : ((upper ? 'A' : 'a') + (d - 10)));
    }
    int padCount = (width > n) ? (width - n) : 0;
    char padChar = zeroPad ? '0' : ' ';
    for (int i = 0; i < padCount; i++) {
        pos = appendChar(buf, size, pos, padChar);
    }
    for (int i = n - 1; i >= 0; i--) {
        pos = appendChar(buf, size, pos, digits[i]);
    }
    return pos;
}

static size_t appendInt(char *buf, size_t size, size_t pos, int64_t value, int width, int zeroPad) {
    uint64_t magnitude;
    if (value < 0) {
        pos = appendChar(buf, size, pos, '-');
        magnitude = (uint64_t)(-(value + 1)) + 1; /* avoids overflow negating INT64_MIN */
        if (width > 0) {
            width--;
        }
    } else {
        magnitude = (uint64_t)value;
    }
    return appendUint(buf, size, pos, magnitude, 10, 0, width, zeroPad);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    const char *p = fmt;
    for (; *p != '\0'; p++) {
        if (*p != '%') {
            pos = appendChar(buf, size, pos, *p);
            continue;
        }
        p++;
        if (*p == '\0') {
            break;
        }
        if (*p == '%') {
            pos = appendChar(buf, size, pos, '%');
            continue;
        }

        int zeroPad = 0;
        if (*p == '0') {
            zeroPad = 1;
            p++;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        int isLongLong = 0;
        if (*p == 'l') {
            p++;
            if (*p == 'l') {
                p++;
            }
            isLongLong = 1; /* %l and %ll are both treated as 64-bit on this target */
        }

        if (*p == '\0') {
            /* An incomplete specifier ("%l", "%0", "%5", ...) walked p onto the format string's
             * own NUL. Stop here instead of falling into the switch's default case, which would
             * append *p (the NUL itself) and then the loop's p++ would read one byte past the
             * string's end. */
            break;
        }

        switch (*p) {
            case 'c':
                pos = appendChar(buf, size, pos, (char)va_arg(ap, int));
                break;
            case 's': {
                const char *s = va_arg(ap, const char *);
                pos = appendStr(buf, size, pos, s != NULL ? s : "(null)");
                break;
            }
            case 'd': {
                int64_t v = isLongLong ? va_arg(ap, long long) : va_arg(ap, int);
                pos = appendInt(buf, size, pos, v, width, zeroPad);
                break;
            }
            case 'u': {
                uint64_t v = isLongLong ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
                pos = appendUint(buf, size, pos, v, 10, 0, width, zeroPad);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = isLongLong ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
                pos = appendUint(buf, size, pos, v, 16, *p == 'X', width, zeroPad);
                break;
            }
            case 'p': {
                void *ptr = va_arg(ap, void *);
                pos = appendStr(buf, size, pos, "0x");
                pos = appendUint(buf, size, pos, (uint64_t)(uintptr_t)ptr, 16, 0, 16, 1);
                break;
            }
            default:
                pos = appendChar(buf, size, pos, '%');
                pos = appendChar(buf, size, pos, *p);
                break;
        }
    }
    if (size > 0) {
        bool truncated = pos >= size;
        buf[truncated ? size - 1 : pos] = '\0';
        /* A truncated line silently drops its trailing '\n' (the byte that would hold it gets
         * overwritten by the NUL above), which visually joins the next line onto it in the serial
         * log -- easy to misread as one garbled line rather than two separate ones. If the format
         * string itself ends in '\n' (true for every wire-protocol line this kernel emits), force
         * it back in so a truncated line is still terminated. */
        if (truncated && size >= 2 && p != fmt && p[-1] == '\n') {
            buf[size - 2] = '\n';
            buf[size - 1] = '\0';
        }
    }
    return (int)pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
