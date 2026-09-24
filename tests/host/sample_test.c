/* Sample test proving the host-test runner works (M1.1's "Done when" check). Real subsystem
 * tests (allocators, bongfs, crypto vectors, parsers) land alongside their code in later
 * milestones. */
#include "framework/test.h"

TEST(sampleArithmeticHolds) {
    ASSERT_EQ(2 + 2, 4);
    ASSERT_TRUE(7 > 3);
}

TEST(sampleStringsMatch) {
    ASSERT_STREQ("bongOS", "bongOS");
}
