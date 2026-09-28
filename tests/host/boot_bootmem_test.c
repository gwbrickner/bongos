/* Host tests for boot/common/include/bootmem.h's bootDivMod64() (D-065/D-107: a portable
 * 64-bit-by-32-bit divide/remainder, since i386 stage2 links no compiler-rt for the compiler's
 * own 64-bit `/`/`%`). */
#include "bootmem.h"
#include "framework/test.h"

TEST(bootDivMod64SmallValues) {
    uint32_t rem = 0xFFFFFFFFu;
    ASSERT_EQ(bootDivMod64(17, 5, &rem), 3u);
    ASSERT_EQ(rem, 2u);
}

TEST(bootDivMod64ExactDivision) {
    uint32_t rem = 0xFFFFFFFFu;
    ASSERT_EQ(bootDivMod64(100, 10, &rem), 10u);
    ASSERT_EQ(rem, 0u);
}

TEST(bootDivMod64ZeroDividend) {
    uint32_t rem = 0xFFFFFFFFu;
    ASSERT_EQ(bootDivMod64(0, 7, &rem), 0u);
    ASSERT_EQ(rem, 0u);
}

TEST(bootDivMod64DivisorOne) {
    uint32_t rem = 0xFFFFFFFFu;
    uint64_t v = 0x123456789ABCDEFull;
    ASSERT_EQ(bootDivMod64(v, 1, &rem), v);
    ASSERT_EQ(rem, 0u);
}

TEST(bootDivMod64NeedsFullWidth) {
    /* A dividend that doesn't fit in 32 bits, matching bootfat.c's actual use (byte offsets/
     * cluster-run byte counts computed as uint64_t). */
    uint64_t dividend = 0x100000000ull + 12345; /* 2^32 + 12345 */
    uint32_t rem = 0xFFFFFFFFu;
    uint64_t q = bootDivMod64(dividend, 512, &rem);
    ASSERT_EQ(q, dividend / 512); /* host build: plain 64-bit division is fine for the oracle */
    ASSERT_EQ(rem, (uint32_t)(dividend % 512));
}

TEST(bootDivMod64NullRemainderIsIgnored) {
    ASSERT_EQ(bootDivMod64(99, 10, NULL), 9u);
}
