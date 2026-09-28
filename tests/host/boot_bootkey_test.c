/* Host tests for boot/common/bootkey.c: the BIOS menu's serial arrow-key parser (D-110). */
#include "bootkey.h"
#include "framework/test.h"

TEST(bootKeyDigitsAndEnter) {
    BootKeyParser p;
    bootKeyParserInit(&p);
    BootKey k;
    ASSERT_TRUE(bootKeyParserFeed(&p, '3', &k));
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_DIGIT);
    ASSERT_EQ(k.digit, 3u);

    ASSERT_TRUE(bootKeyParserFeed(&p, '\r', &k));
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_ENTER);
    ASSERT_TRUE(bootKeyParserFeed(&p, '\n', &k));
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_ENTER);
}

TEST(bootKeyArrowsViaBracket) {
    BootKeyParser p;
    bootKeyParserInit(&p);
    BootKey k;
    ASSERT_TRUE(!bootKeyParserFeed(&p, 0x1B, &k)); /* ESC: not complete yet */
    ASSERT_TRUE(!bootKeyParserFeed(&p, '[', &k));
    ASSERT_TRUE(bootKeyParserFeed(&p, 'A', &k));
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_UP);

    ASSERT_TRUE(!bootKeyParserFeed(&p, 0x1B, &k));
    ASSERT_TRUE(!bootKeyParserFeed(&p, '[', &k));
    ASSERT_TRUE(bootKeyParserFeed(&p, 'B', &k));
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_DOWN);
}

TEST(bootKeyArrowsViaApplicationCursorMode) {
    BootKeyParser p;
    bootKeyParserInit(&p);
    BootKey k;
    ASSERT_TRUE(!bootKeyParserFeed(&p, 0x1B, &k));
    ASSERT_TRUE(!bootKeyParserFeed(&p, 'O', &k));
    ASSERT_TRUE(bootKeyParserFeed(&p, 'A', &k));
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_UP);
}

TEST(bootKeyEscFollowedByJunkIsOtherAndConsumesOneByte) {
    BootKeyParser p;
    bootKeyParserInit(&p);
    BootKey k;
    ASSERT_TRUE(!bootKeyParserFeed(&p, 0x1B, &k));
    ASSERT_TRUE(bootKeyParserFeed(&p, 'Z', &k)); /* not '[' or 'O': OTHER, consumed */
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_OTHER);
    /* Parser is back to idle -- the very next byte starts fresh, not treated as part of the
     * escape sequence. */
    ASSERT_TRUE(bootKeyParserFeed(&p, '5', &k));
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_DIGIT);
    ASSERT_EQ(k.digit, 5u);
}

TEST(bootKeyCsiFollowedByJunkIsOther) {
    BootKeyParser p;
    bootKeyParserInit(&p);
    BootKey k;
    ASSERT_TRUE(!bootKeyParserFeed(&p, 0x1B, &k));
    ASSERT_TRUE(!bootKeyParserFeed(&p, '[', &k));
    ASSERT_TRUE(bootKeyParserFeed(&p, 'Z', &k));
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_OTHER);
}

TEST(bootKeyPlainByteIsOther) {
    BootKeyParser p;
    bootKeyParserInit(&p);
    BootKey k;
    ASSERT_TRUE(bootKeyParserFeed(&p, 'x', &k));
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_OTHER);
    ASSERT_TRUE(bootKeyParserFeed(&p, '0', &k)); /* '0' is not in 1-9: OTHER */
    ASSERT_EQ((int)k.kind, (int)BOOT_KEY_OTHER);
}
