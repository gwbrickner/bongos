/* See compress.h. CRC-32 (the reflected 0xEDB88320 polynomial) with a constant 16-entry nibble
 * table: no lazy initialisation, so it is safe from any thread with no synchronisation. The
 * standard published algorithm, same result as tools/mkimage/crc32.c, in zlib's running-value
 * convention. */
#include "compress/compress.h"

static const uint32_t CRC_NIBBLE[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
                                        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
                                        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
                                        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};

uint32_t compressCrc32(uint32_t crc, const void *p, size_t n) {
    const uint8_t *bytes = p;
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= bytes[i];
        crc = CRC_NIBBLE[crc & 15u] ^ (crc >> 4);
        crc = CRC_NIBBLE[crc & 15u] ^ (crc >> 4);
    }
    return crc ^ 0xFFFFFFFFu;
}
