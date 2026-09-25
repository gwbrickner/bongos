/* Host tests for boot/common/bootcfg.c's boot.cfg parser (ARCHITECTURE §5.2, D-067, §23).
 *
 * The API changed from M1.3's single-struct BootCfg (with .kernel/.cmdline fields directly) to a
 * two-phase parse-then-resolve design (BootCfg holds spans + the parsed [entry] sections;
 * bootCfgResolveEntry() copies one entry's effective, inherited values into a BootCfgEntry) so
 * that [entry] sections, inheritance, and default= resolution have somewhere to live. No
 * assertion here is weaker than M1.3's -- every case that checked a field on BootCfg now parses
 * then resolves index 0 (or the resolved cfg.defaultIndex) and checks the same field on the
 * resulting BootCfgEntry. */
#include "bootcfg.h"
#include "framework/test.h"

#include <string.h>

/* void, not BootCfgEntry: ASSERT_* expands to a bare `return;`, which only compiles (cleanly,
 * under -Werror) inside a void function. A failed parse/resolve leaves `*out` zeroed (from
 * bootCfgParse's/bootCfgResolveEntry's own bootMemset) rather than uninitialized. */
static void resolveInto(const char *text, uint32_t index, BootCfgEntry *out) {
    memset(out, 0, sizeof(*out));
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(bootCfgResolveEntry(text, strlen(text), &cfg, index, out), BOOT_OK);
}

TEST(bootCfgParsesKernelAndCmdline) {
    const char *text = "kernel = /bong/kernel.elf\ncmdline = ktest=all\n";
    BootCfgEntry entry;
    resolveInto(text, 0, &entry);
    ASSERT_STREQ(entry.kernel, "/bong/kernel.elf");
    ASSERT_STREQ(entry.cmdline, "ktest=all");
    ASSERT_TRUE(!entry.cmdlineTruncated);
}

TEST(bootCfgSkipsCommentsAndBlankLines) {
    const char *text = "# comment\n\nkernel = /bong/kernel.elf\n   \ncmdline=\n";
    BootCfgEntry entry;
    resolveInto(text, 0, &entry);
    ASSERT_STREQ(entry.kernel, "/bong/kernel.elf");
    ASSERT_STREQ(entry.cmdline, "");
}

TEST(bootCfgFirstKeyWins) {
    const char *text = "kernel = /a\nkernel = /b\ncmdline=first\ncmdline=second\n";
    BootCfgEntry entry;
    resolveInto(text, 0, &entry);
    ASSERT_STREQ(entry.kernel, "/a");
    ASSERT_STREQ(entry.cmdline, "first");
}

TEST(bootCfgIgnoresUnknownKeys) {
    const char *text = "unknownkey = auto\nkernel = /bong/kernel.elf\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(cfg.unknownKeyCount, (uint32_t)1);
    ASSERT_EQ(cfg.firstUnknownKeyLine, (uint32_t)1);
    BootCfgEntry entry;
    ASSERT_EQ(bootCfgResolveEntry(text, strlen(text), &cfg, 0, &entry), BOOT_OK);
    ASSERT_STREQ(entry.kernel, "/bong/kernel.elf");
}

TEST(bootCfgRejectsKernelNotStartingWithSlash) {
    const char *text = "kernel = bong/kernel.elf\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
    ASSERT_EQ(cfg.errorLine, (uint32_t)1);
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
    BootCfgEntry entry;
    resolveInto(text, 0, &entry);
    ASSERT_STREQ(entry.kernel, "/bong/kernel.elf");
    ASSERT_STREQ(entry.cmdline, "a b c");
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

    BootCfgEntry entry;
    resolveInto(text, 0, &entry);
    ASSERT_EQ(strlen(entry.cmdline), (uint64_t)(BOOTINFO_CMDLINE_MAX - 1));
    ASSERT_TRUE(entry.cmdlineTruncated);
}

/* --- M1.4 [entry] section tests --- */

TEST(bootCfgZeroSectionsGivesOneImplicitEntry) {
    const char *text = "kernel = /bong/kernel.elf\ncmdline = ktest=all\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(cfg.entryCount, (uint32_t)1);
    ASSERT_EQ(cfg.defaultIndex, (uint32_t)0);
}

TEST(bootCfgEntriesInheritGlobalsAndCanOverride) {
    const char *text = "kernel = /bong/kernel.elf\ncmdline = loglevel=info\n\n"
                       "[bongOS]\n\n"
                       "[Safe mode]\ncmdline = cpus=1 nomodules fbcon=on\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(cfg.entryCount, (uint32_t)2);

    BootCfgEntry e0;
    ASSERT_EQ(bootCfgResolveEntry(text, strlen(text), &cfg, 0, &e0), BOOT_OK);
    ASSERT_STREQ(e0.name, "bongOS");
    ASSERT_STREQ(e0.kernel, "/bong/kernel.elf");
    ASSERT_STREQ(e0.cmdline, "loglevel=info");

    BootCfgEntry e1;
    ASSERT_EQ(bootCfgResolveEntry(text, strlen(text), &cfg, 1, &e1), BOOT_OK);
    ASSERT_STREQ(e1.name, "Safe mode");
    ASSERT_STREQ(e1.kernel, "/bong/kernel.elf");           /* inherited */
    ASSERT_STREQ(e1.cmdline, "cpus=1 nomodules fbcon=on"); /* overridden */
}

TEST(bootCfgExplicitEmptyOverridesInheritedValue) {
    const char *text = "cmdline = loglevel=info\ninitrd = /bong/initrd.img\n\n"
                       "[a]\ncmdline =\ninitrd =\n";
    BootCfgEntry entry;
    resolveInto(text, 0, &entry);
    ASSERT_STREQ(entry.cmdline, "");
    ASSERT_STREQ(entry.initrd, "");
}

TEST(bootCfgInitridDefaultsToNone) {
    const char *text = "[a]\n";
    BootCfgEntry entry;
    resolveInto(text, 0, &entry);
    ASSERT_STREQ(entry.initrd, "");
}

TEST(bootCfgDefaultByIndex) {
    const char *text = "default = 2\n[a]\n[b]\n[c]\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(cfg.defaultIndex, (uint32_t)1);
}

TEST(bootCfgDefaultByName) {
    const char *text = "default = Safe mode\n[bongOS]\n[Safe mode]\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(cfg.defaultIndex, (uint32_t)1);
}

TEST(bootCfgDefaultOutOfRangeIndexIsError) {
    const char *text = "default = 5\n[a]\n[b]\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
    ASSERT_EQ(cfg.errorLine, (uint32_t)1);
}

TEST(bootCfgDefaultUnknownNameIsError) {
    const char *text = "default = nope\n[a]\n[b]\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgDefaultWithoutKeyIsFirstEntry) {
    const char *text = "[a]\n[b]\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(cfg.defaultIndex, (uint32_t)0);
}

TEST(bootCfgTimeoutForever) {
    const char *text = "timeout = forever\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(cfg.timeoutSec, BOOT_CFG_TIMEOUT_FOREVER);
}

TEST(bootCfgTimeoutTooLargeIsError) {
    const char *text = "timeout = 3601\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgTimeoutAtMaxIsOk) {
    const char *text = "timeout = 3600\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(cfg.timeoutSec, (uint32_t)3600);
}

TEST(bootCfgTimeoutInEntryIsError) {
    const char *text = "[a]\ntimeout = 5\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
    ASSERT_EQ(cfg.errorLine, (uint32_t)2);
}

TEST(bootCfgDefaultInEntryIsError) {
    const char *text = "[a]\ndefault = 1\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgMoreThanNineEntriesIsError) {
    const char *text = "[1]\n[2]\n[3]\n[4]\n[5]\n[6]\n[7]\n[8]\n[9]\n[10]\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgNineEntriesIsOk) {
    const char *text = "[1]\n[2]\n[3]\n[4]\n[5]\n[6]\n[7]\n[8]\n[9]\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_OK);
    ASSERT_EQ(cfg.entryCount, (uint32_t)9);
}

TEST(bootCfgDuplicateEntryNameIsError) {
    const char *text = "[a]\n[a]\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgUnterminatedSectionHeaderIsError) {
    const char *text = "[a\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgLineWithoutEqualsIsError) {
    const char *text = "not a key value line\n";
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
    ASSERT_EQ(cfg.errorLine, (uint32_t)1);
}

TEST(bootCfgSkipsLeadingBom) {
    const char text[] = "\xEF\xBB\xBFkernel = /bong/kernel.elf\n";
    BootCfgEntry entry;
    resolveInto(text, 0, &entry);
    ASSERT_STREQ(entry.kernel, "/bong/kernel.elf");
}

TEST(bootCfgRejectsNulByte) {
    char text[32];
    memcpy(text, "kernel = /a\n", 12);
    text[12] = '\0'; /* a stray embedded NUL, not the end of the buffer */
    memcpy(text + 13, "cmdline = x\n", 12);
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, 25, &cfg), BOOT_ERR_CFG);
    ASSERT_EQ(cfg.errorLine, (uint32_t)2);
}

TEST(bootCfgRejectsFileTooLarge) {
    static char text[BOOT_CFG_FILE_MAX + 2];
    memset(text, '#', sizeof(text));
    text[sizeof(text) - 1] = '\n';
    BootCfg cfg;
    ASSERT_EQ(bootCfgParse(text, sizeof(text), &cfg), BOOT_ERR_CFG);
    ASSERT_EQ(cfg.errorLine, (uint32_t)0);
}

TEST(bootCfgResolutionAcceptsBothXCase) {
    const char *text1 = "resolution = 1024x768\n";
    const char *text2 = "resolution = 1024X768\n";
    BootCfgEntry e1;
    resolveInto(text1, 0, &e1);
    BootCfgEntry e2;
    resolveInto(text2, 0, &e2);
    ASSERT_EQ(e1.resWidth, (uint32_t)1024);
    ASSERT_EQ(e1.resHeight, (uint32_t)768);
    ASSERT_EQ(e2.resWidth, (uint32_t)1024);
    ASSERT_EQ(e2.resHeight, (uint32_t)768);
}

TEST(bootCfgResolutionAutoIsZeroZero) {
    BootCfgEntry entry;
    resolveInto("resolution = auto\n", 0, &entry);
    ASSERT_EQ(entry.resWidth, (uint32_t)0);
    ASSERT_EQ(entry.resHeight, (uint32_t)0);
    BootCfgEntry unset;
    resolveInto("kernel = /bong/kernel.elf\n", 0, &unset);
    ASSERT_EQ(unset.resWidth, (uint32_t)0);
    ASSERT_EQ(unset.resHeight, (uint32_t)0);
}

TEST(bootCfgResolutionRejectsMissingWidth) {
    BootCfg cfg;
    const char *text = "resolution = x768\n";
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgResolutionRejectsZeroWidth) {
    BootCfg cfg;
    const char *text = "resolution = 0x5\n";
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgResolutionRejectsTooLarge) {
    BootCfg cfg;
    const char *text = "resolution = 16385x1\n";
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgKaslrDefaultsOn) {
    BootCfgEntry entry;
    resolveInto("kernel = /bong/kernel.elf\n", 0, &entry);
    ASSERT_TRUE(entry.kaslr);
}

TEST(bootCfgKaslrOff) {
    BootCfgEntry entry;
    resolveInto("kaslr = off\n", 0, &entry);
    ASSERT_TRUE(!entry.kaslr);
}

TEST(bootCfgKaslrInvalidValueIsError) {
    BootCfg cfg;
    const char *text = "kaslr = maybe\n";
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}

TEST(bootCfgMalformedKeyIsError) {
    BootCfg cfg;
    const char *text = "Kernel = /bong/kernel.elf\n"; /* uppercase, not in [a-z0-9_.] */
    ASSERT_EQ(bootCfgParse(text, strlen(text), &cfg), BOOT_ERR_CFG);
}
