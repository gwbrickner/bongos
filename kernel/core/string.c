/* Freestanding memcpy/memset/memmove/memcmp: clang emits implicit calls to these (struct
 * assignment, array zero-init, aggregate comparison) even under -ffreestanding, so something must
 * define them or the kernel fails to link. These are the one place in the kernel that keeps their
 * plain libc names rather than a subsystem prefix (ARCHITECTURE §4): the compiler generates calls
 * to exactly these symbols, not to a bongOS-prefixed equivalent. */
#include <stddef.h>
#include <stdint.h>

/* No locks, IRQ-safe, may not sleep; pure (touches only its arguments). */
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

/* No locks, IRQ-safe, may not sleep; pure. */
void *memset(void *dst, int value, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (uint8_t)value;
    }
    return dst;
}

/* No locks, IRQ-safe, may not sleep; pure. Copies backward when the ranges overlap and `dst` is
 * ahead of `src`, forward otherwise -- the two orderings a byte-at-a-time memmove must choose
 * between to behave like a real move rather than corrupting overlapping data. */
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

/* No locks, IRQ-safe, may not sleep; pure. */
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}
