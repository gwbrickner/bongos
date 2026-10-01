/* cryptoWipe (ARCHITECTURE §17). EXPERIMENTAL crypto. */
#include "crypto/wipe.h"

#include <stdint.h>

/* Technique: a byte loop of volatile stores in an out-of-line function. C17 5.1.2.3 makes every
 * volatile access an observable side effect, so the compiler may neither delete nor merge them,
 * whatever it can prove about the buffer's later use. ARCHITECTURE §4 keeps assembly in
 * kernel/arch/ and boot/ only, so the usual `asm volatile("" ::: "memory")` compiler barrier is
 * deliberately not used here (nor a memset call, which the compiler is free to elide); the
 * volatile stores alone give the guarantee. `noinline` keeps the call opaque to the caller's
 * dead-store analysis as a second line of defence. Contract: see crypto/wipe.h. */
__attribute__((noinline)) void cryptoWipe(void *p, size_t len) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0; i < len; i++) {
        v[i] = 0;
    }
}
