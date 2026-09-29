/* CRC-32 (ISO 3309 / the "zlib" polynomial 0xEDB88320, reflected, init/final XOR 0xFFFFFFFF) --
 * a standard, published algorithm, not third-party code. Used by bootgpt.c to validate a GPT
 * header/partition-entry array (UEFI Spec §5.3.2, D-105). Streaming (bootCrc32Update), unlike
 * tools/mkimage/crc32.c's whole-buffer crc32Compute(): bootgpt.c only has room for one sector of
 * scratch at a time, not the whole (up to 16 KiB) partition-entry array. Different name on
 * purpose -- both this file and tools/mkimage/crc32.c end up on tests/host's include path. */
#ifndef BOOT_COMMON_BOOTCRC32_H
#define BOOT_COMMON_BOOTCRC32_H

#include <stddef.h>
#include <stdint.h>

/* The running CRC state to pass as `crc` to the first bootCrc32Update() call. */
#define BOOT_CRC32_INIT 0xFFFFFFFFu

/* Folds `len` bytes of `data` into the running CRC state `crc` (BOOT_CRC32_INIT for the first
 * call in a sequence, or a previous call's return value to continue). No locks, boot-time or
 * host-test only; pure. */
uint32_t bootCrc32Update(uint32_t crc, const void *data, size_t len);

/* Un-inverts a running CRC state into the standard published CRC-32 check value. Call once after
 * the last bootCrc32Update() in a sequence. */
static inline uint32_t bootCrc32Finish(uint32_t crc) {
    return crc ^ 0xFFFFFFFFu;
}

/* One-shot convenience for a buffer that's entirely in memory already:
 * bootCrc32Finish(bootCrc32Update(BOOT_CRC32_INIT, data, len)). */
uint32_t bootCrc32(const void *data, size_t len);

#endif
