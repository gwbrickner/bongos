/* See bootcrc32.h. Bitwise (no 256-entry table): this runs a handful of times per boot over at
 * most a few sectors, not a hot path, and skipping the table keeps this file's .data/.bss empty
 * (relevant for stage2's tight low-memory/on-disk-size budget, D-101). */
#include "include/bootcrc32.h"

uint32_t bootCrc32Update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc;
}

uint32_t bootCrc32(const void *data, size_t len) {
    return bootCrc32Finish(bootCrc32Update(BOOT_CRC32_INIT, data, len));
}
