/* See bootkaslr.h. */
#include "include/bootkaslr.h"

#include "include/bootinfo.h"
#include "include/bootmem.h"

_Static_assert(BOOT_KASLR_WINDOW == BOOTINFO_KERNEL_WINDOW_END - BOOTINFO_KERNEL_WINDOW_BASE,
               "BOOT_KASLR_WINDOW must match the kernel window (ARCHITECTURE §6.1)");

/* The splitmix64 finalizer: a bijection on 64 bits with good avalanche. Add/xor/shift/multiply
 * only: no division, so it costs nothing extra on i386. */
static uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint64_t leU64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

BootStatus bootKaslrPickSlide(const uint8_t seed[64], uint64_t span, uint64_t *outSlide) {
    if (seed == NULL || outSlide == NULL) {
        return BOOT_ERR_ELF_HEADER;
    }
    if (span == 0 || span > BOOT_KASLR_WINDOW) {
        return BOOT_ERR_ELF_RANGE; /* also keeps alignUp below from overflowing */
    }
    uint64_t need = bootAlignUp(span, BOOT_KASLR_ALIGN);
    if (need > BOOT_KASLR_WINDOW) {
        return BOOT_ERR_ELF_RANGE;
    }
    uint64_t slots = ((BOOT_KASLR_WINDOW - need) >> 21) + 1; /* 1..256 */

    uint64_t h = 0x4B41534C52534C44ULL; /* domain separator: ASCII "KASLRSLD" */
    for (uint32_t i = 0; i < BOOT_KASLR_SEED_BYTES / 8; i++) {
        h = mix64(h ^ leU64(seed + 8u * i));
    }
    /* Multiply-shift range reduction: slot = floor(r * slots / 2^32) with r a uniform 32-bit
     * value, so no modulo (and its bias is below 2^-24 for slots <= 256). */
    uint64_t slot = ((uint64_t)(uint32_t)(h >> 32) * slots) >> 32;
    *outSlide = slot << 21;
    return BOOT_OK;
}
