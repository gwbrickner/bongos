/* See bootmem.h. Plain byte loops: freestanding, no libc, no reliance on the compiler recognizing
 * the loop shape and turning it into a call to itself. */
#include "include/bootmem.h"

void bootMemcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

void bootMemset(void *dst, uint8_t value, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = value;
    }
}
