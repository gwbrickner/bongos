/* Host tests for boot/common/bootcfg.c's minimal boot.cfg parser (ARCHITECTURE §5.2/§23). */
#include "bootcfg.h"
#include "framework/test.h"

#include <string.h>

TEST(bootCfgParsesKernelAndCmdline) {
    const char *text = "kernel = /bong/kernel.elf\ncmdline = ktest=all\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_TRUE(cfg.hasKernel);
    ASSERT_STREQ(cfg.kernel, "/bong/kernel.elf");
    ASSERT_TRUE(cfg.hasCmdline);
    ASSERT_STREQ(cfg.cmdline, "ktest=all");
}

TEST(bootCfgSkipsCommentsBlankAndSectionLines) {
    const char *text = "# comment\n\n[entry]\nkernel = /bong/kernel.elf\n   \ncmdline=\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_TRUE(cfg.hasKernel);
    ASSERT_STREQ(cfg.kernel, "/bong/kernel.elf");
    ASSERT_TRUE(cfg.hasCmdline);
    ASSERT_STREQ(cfg.cmdline, "");
}

TEST(bootCfgFirstKeyWins) {
    const char *text = "kernel = /a\nkernel = /b\ncmdline=first\ncmdline=second\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_STREQ(cfg.kernel, "/a");
    ASSERT_STREQ(cfg.cmdline, "first");
}

TEST(bootCfgIgnoresUnknownKeys) {
    const char *text = "resolution = auto\nkernel = /bong/kernel.elf\ntimeout=5\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_TRUE(cfg.hasKernel);
    ASSERT_TRUE(!cfg.hasCmdline);
}

TEST(bootCfgRejectsKernelNotStartingWithSlash) {
    const char *text = "kernel = bong/kernel.elf\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgRejectsKernelTooLong) {
    char text[600];
    strcpy(text, "kernel = /");
    size_t pos = strlen(text);
    for (int i = 0; i < 260; i++) {
        text[pos++] = 'a';
    }
    text[pos++] = '\n';
    text[pos] = '\0';

    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgRejectsNonAsciiKernel) {
    const char text[] = "kernel = /bong/k\xC3\xA9rnel.elf\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, sizeof(text) - 1, &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgHandlesCrlfLineEndings) {
    const char *text = "kernel = /bong/kernel.elf\r\ncmdline = a b c\r\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_STREQ(cfg.kernel, "/bong/kernel.elf");
    ASSERT_STREQ(cfg.cmdline, "a b c");
}

TEST(bootCfgTruncatesOverlongCmdline) {
    static char text[BOOTINFO_CMDLINE_MAX + 128];
    strcpy(text, "cmdline = ");
    size_t pos = strlen(text);
    for (uint32_t i = 0; i < BOOTINFO_CMDLINE_MAX + 10; i++) {
        text[pos++] = 'x';
    }
    text[pos++] = '\n';
    text[pos] = '\0';

    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_TRUE(cfg.hasCmdline);
    ASSERT_EQ(strlen(cfg.cmdline), (uint64_t)(BOOTINFO_CMDLINE_MAX - 1));
}
