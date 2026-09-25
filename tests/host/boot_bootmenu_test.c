/* Host tests for boot/common/bootmenu.c's pure boot-menu state machine (ARCHITECTURE §5.5,
 * D-068). */
#include "bootmenu.h"
#include "framework/test.h"

#include <string.h>

static void makeCfg(BootCfg *cfg, const char *text) {
    memset(cfg, 0, sizeof(*cfg));
    ASSERT_EQ(bootCfgParse(text, strlen(text), cfg), BOOT_OK);
}

TEST(bootMenuInitStartsAtDefaultWithCountdownRunning) {
    BootCfg cfg;
    makeCfg(&cfg, "timeout = 5\ndefault = 2\n[a]\n[b]\n[c]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    ASSERT_EQ(st.entryCount, (uint32_t)3);
    ASSERT_EQ(st.selected, (uint32_t)1);
    ASSERT_EQ(st.remainingSec, (uint32_t)5);
    ASSERT_TRUE(st.countdownActive);
}

TEST(bootMenuInitZeroTimeoutStartsInactive) {
    BootCfg cfg;
    makeCfg(&cfg, "[a]\n[b]\n"); /* no timeout= -> 0 */
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    ASSERT_TRUE(!st.countdownActive);
}

TEST(bootMenuUpDownMoveSelectionAndStopCountdown) {
    BootCfg cfg;
    makeCfg(&cfg, "timeout = 5\n[a]\n[b]\n[c]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);

    BootKey down = {BOOT_KEY_DOWN, 0};
    ASSERT_EQ(bootMenuKey(&st, down), (BootMenuAction)BOOT_MENU_REDRAW_ROWS);
    ASSERT_EQ(st.selected, (uint32_t)1);
    ASSERT_EQ(st.oldSelected, (uint32_t)0);
    ASSERT_TRUE(!st.countdownActive);

    BootKey up = {BOOT_KEY_UP, 0};
    ASSERT_EQ(bootMenuKey(&st, up), (BootMenuAction)BOOT_MENU_REDRAW_ROWS);
    ASSERT_EQ(st.selected, (uint32_t)0);
}

TEST(bootMenuUpDownClampsAtEnds) {
    BootCfg cfg;
    makeCfg(&cfg, "[a]\n[b]\n"); /* timeout 0: countdown already inactive */
    BootMenuState st;
    bootMenuInit(&st, &cfg);

    BootKey up = {BOOT_KEY_UP, 0};
    ASSERT_EQ(bootMenuKey(&st, up),
              (BootMenuAction)BOOT_MENU_NONE); /* already at 0, and countdown inactive */
    ASSERT_EQ(st.selected, (uint32_t)0);

    BootKey down = {BOOT_KEY_DOWN, 0};
    ASSERT_EQ(bootMenuKey(&st, down), (BootMenuAction)BOOT_MENU_REDRAW_ROWS);
    ASSERT_EQ(st.selected, (uint32_t)1);
    ASSERT_EQ(bootMenuKey(&st, down),
              (BootMenuAction)BOOT_MENU_NONE); /* already at the last entry */
    ASSERT_EQ(st.selected, (uint32_t)1);
}

TEST(bootMenuUpAtZeroWithActiveCountdownStopsIt) {
    BootCfg cfg;
    makeCfg(&cfg, "timeout = 5\n[a]\n[b]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    BootKey up = {BOOT_KEY_UP, 0};
    ASSERT_EQ(bootMenuKey(&st, up), (BootMenuAction)BOOT_MENU_REDRAW_COUNTDOWN);
    ASSERT_TRUE(!st.countdownActive);
    ASSERT_EQ(st.selected, (uint32_t)0);
}

TEST(bootMenuEnterBoots) {
    BootCfg cfg;
    makeCfg(&cfg, "[a]\n[b]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    BootKey enter = {BOOT_KEY_ENTER, 0};
    ASSERT_EQ(bootMenuKey(&st, enter), (BootMenuAction)BOOT_MENU_BOOT);
    ASSERT_EQ(st.selected, (uint32_t)0);
}

TEST(bootMenuDigitSelectsAndBoots) {
    BootCfg cfg;
    makeCfg(&cfg, "[a]\n[b]\n[c]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    BootKey digit3 = {BOOT_KEY_DIGIT, 3};
    ASSERT_EQ(bootMenuKey(&st, digit3), (BootMenuAction)BOOT_MENU_BOOT);
    ASSERT_EQ(st.selected, (uint32_t)2);
}

TEST(bootMenuDigitOutOfRangeIsNoOp) {
    BootCfg cfg;
    makeCfg(&cfg, "[a]\n[b]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    BootKey digit9 = {BOOT_KEY_DIGIT, 9};
    ASSERT_EQ(bootMenuKey(&st, digit9), (BootMenuAction)BOOT_MENU_NONE);
    ASSERT_EQ(st.selected, (uint32_t)0);
}

TEST(bootMenuOtherKeyStopsCountdownOnlyOnce) {
    BootCfg cfg;
    makeCfg(&cfg, "timeout = 5\n[a]\n[b]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    BootKey other = {BOOT_KEY_OTHER, 0};
    ASSERT_EQ(bootMenuKey(&st, other), (BootMenuAction)BOOT_MENU_REDRAW_COUNTDOWN);
    ASSERT_EQ(bootMenuKey(&st, other), (BootMenuAction)BOOT_MENU_NONE); /* already stopped */
}

TEST(bootMenuTickCountsDownAndBootsAtZero) {
    BootCfg cfg;
    makeCfg(&cfg, "timeout = 2\n[a]\n[b]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    ASSERT_EQ(bootMenuTick(&st), (BootMenuAction)BOOT_MENU_REDRAW_COUNTDOWN);
    ASSERT_EQ(st.remainingSec, (uint32_t)1);
    ASSERT_EQ(bootMenuTick(&st), (BootMenuAction)BOOT_MENU_BOOT);
    ASSERT_EQ(st.remainingSec, (uint32_t)0);
    ASSERT_TRUE(!st.countdownActive);
}

TEST(bootMenuTickIsNoOpWhenInactiveOrForever) {
    BootCfg cfg;
    makeCfg(&cfg, "timeout = forever\n[a]\n[b]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    ASSERT_TRUE(st.countdownActive); /* forever still "runs" (no menu-abandon by idle timeout) */
    ASSERT_EQ(bootMenuTick(&st),
              (BootMenuAction)BOOT_MENU_NONE); /* but never actually counts down */
    ASSERT_EQ(st.remainingSec, BOOT_CFG_TIMEOUT_FOREVER);
}

TEST(bootMenuTickAfterKeyStoppedIsNoOp) {
    BootCfg cfg;
    makeCfg(&cfg, "timeout = 5\n[a]\n[b]\n");
    BootMenuState st;
    bootMenuInit(&st, &cfg);
    BootKey other = {BOOT_KEY_OTHER, 0};
    bootMenuKey(&st, other);
    ASSERT_EQ(bootMenuTick(&st), (BootMenuAction)BOOT_MENU_NONE);
}
