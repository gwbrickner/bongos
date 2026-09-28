/* See bootacpi.h. */
#include "include/bootacpi.h"

#include "include/bootmem.h"

#include <stdbool.h>

#define RSDP_SCAN_STEP  16u
#define RSDP_V1_SIZE    20u
#define RSDP_SIG_OFFSET 0u
#define RSDP_SIG_LEN    8u
#define RSDP_REV_OFFSET 15u
#define RSDP_LEN_OFFSET 20u

static bool matchesSignature(const uint8_t *p) {
    static const uint8_t sig[RSDP_SIG_LEN] = {'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    for (uint32_t i = 0; i < RSDP_SIG_LEN; i++) {
        if (p[RSDP_SIG_OFFSET + i] != sig[i]) {
            return false;
        }
    }
    return true;
}

static uint8_t sumBytes(const uint8_t *p, uint32_t n) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum;
}

uint64_t bootAcpiScanForRsdp(const uint8_t *buf, size_t len, uint64_t base) {
    for (size_t off = 0; off + RSDP_V1_SIZE <= len; off += RSDP_SCAN_STEP) {
        const uint8_t *p = buf + off;
        if (!matchesSignature(p)) {
            continue;
        }
        if (sumBytes(p, RSDP_V1_SIZE) != 0) {
            continue;
        }
        uint8_t revision = p[RSDP_REV_OFFSET];
        if (revision >= 2) {
            uint32_t length;
            bootMemcpy(&length, p + RSDP_LEN_OFFSET, sizeof(length));
            if (length < 36 || (uint64_t)length > (uint64_t)(len - off)) {
                continue;
            }
            if (sumBytes(p, length) != 0) {
                continue;
            }
        }
        return base + off;
    }
    return 0;
}
