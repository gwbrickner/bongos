/* Sample tests proving the host-test runner works (M1.1's "Done when" check) and that
 * branding.h actually gets generated and included. Real subsystem tests (allocators, bongfs,
 * crypto vectors, parsers) land alongside their code in later milestones. */
#include "branding.h"
#include "framework/test.h"

#include <string.h>

TEST(sampleArithmeticHolds) {
    ASSERT_EQ(2 + 2, 4);
    ASSERT_TRUE(7 > 3);
}

TEST(sampleStringsMatch) {
    ASSERT_STREQ("left", "left");
}

TEST(brandingHeaderIsGenerated) {
    ASSERT_TRUE(strlen(BRANDING_NAME) > 0);
    ASSERT_TRUE(strlen(BRANDING_VERSION) > 0);
    ASSERT_TRUE(strlen(BRANDING_CODENAME) > 0);
}
