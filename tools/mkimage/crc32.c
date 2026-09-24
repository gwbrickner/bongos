#include "crc32.h"

static uint32_t crc32Table[256];
static int crc32TableReady = 0;

static void crc32BuildTable(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; bit++) {
            c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
        }
        crc32Table[i] = c;
    }
    crc32TableReady = 1;
}

uint32_t crc32Compute(const void *data, size_t length) {
    if (!crc32TableReady) {
        crc32BuildTable();
    }
    const uint8_t *bytes = data;
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; i++) {
        crc = crc32Table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}
