/* A tiny freestanding printf-family (ARCHITECTURE §4: only freestanding headers, no libc). Used
 * by klog, panic, and the ktest reporter -- everything that needs to build a line of text before
 * writing it to serial. */
#ifndef KERNEL_FORMAT_H
#define KERNEL_FORMAT_H

#include <stdarg.h>
#include <stddef.h>

/* Supports %%, %c, %s, %d, %u, %x, %X, and %p, each with an optional '0' zero-pad flag, a decimal
 * width, and an optional "l"/"ll" length modifier (%ld/%lu/%lx are 64-bit on this LP64-like
 * kernel target; %lld/%llu/%llx are always 64-bit). An unrecognized conversion is emitted
 * literally (e.g. "%q" -> "%q"), so a typo in a format string doesn't corrupt the rest of the
 * line. Always NUL-terminates when size > 0. Returns the number of characters that would have
 * been written excluding the NUL (as snprintf does), so a caller can detect truncation by
 * comparing the result against `size`. No locks, may not sleep, IRQ-safe; pure (touches only
 * `buf`). */
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* varargs wrapper around kvsnprintf. Same contract. */
int ksnprintf(char *buf, size_t size, const char *fmt, ...);

#endif
