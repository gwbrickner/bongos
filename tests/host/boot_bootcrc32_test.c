/* Host tests for boot/common/bootcrc32.c (D-105). */
#include "bootcrc32.h"
#include "framework/test.h"

#include <string.h>

TEST(bootCrc32MatchesKnownCheckVector) {
    /* The standard CRC-32 check value for the ASCII string "123456789". */
    const char *s = "123456789";
    ASSERT_EQ(bootCrc32(s, strlen(s)), 0xCBF43926u);
}

TEST(bootCrc32EmptyInputIsZero) {
    ASSERT_EQ(bootCrc32("", 0), 0u);
}

TEST(bootCrc32StreamingMatchesOneShot) {
    const char *s = "the quick brown fox jumps over the lazy dog";
    size_t len = strlen(s);
    uint32_t oneShot = bootCrc32(s, len);

    uint32_t crc = BOOT_CRC32_INIT;
    crc = bootCrc32Update(crc, s, 7);
    crc = bootCrc32Update(crc, s + 7, len - 7);
    ASSERT_EQ(bootCrc32Finish(crc), oneShot);
}
