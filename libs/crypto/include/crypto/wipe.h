/* cryptoWipe: zeroes secret material in a way the compiler may not optimize away (ARCHITECTURE
 * §17: "Zero secrets after use"). EXPERIMENTAL, unaudited crypto. */
#ifndef CRYPTO_WIPE_H
#define CRYPTO_WIPE_H

#include <stddef.h>

/* Overwrites `len` bytes at `p` with zero. Every store is volatile, so the compiler must perform
 * it even when the buffer is dead afterwards (a plain memset before return/scope end may be
 * deleted as a dead store). Cost depends only on `len`, never on the contents. `p` may be NULL
 * only when `len` is 0. No locks, may not sleep, IRQ-safe, cannot fail. */
void cryptoWipe(void *p, size_t len);

#endif
