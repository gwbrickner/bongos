/* CRC-32 (ISO 3309 / the "zlib" polynomial 0xEDB88320, reflected, init/final XOR 0xFFFFFFFF).
 * GPT headers and partition-entry arrays are checksummed with this algorithm (UEFI Spec §5.3.2);
 * it's a standard, published algorithm, not third-party code. */
#ifndef MKIMAGE_CRC32_H
#define MKIMAGE_CRC32_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32Compute(const void *data, size_t length);

#endif
