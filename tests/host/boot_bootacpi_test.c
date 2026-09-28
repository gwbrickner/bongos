/* Host tests for boot/common/bootacpi.c: the RSDP scan (ACPI "Finding the RSDP on IA-PC
 * Systems", D-107). */
#include "bootacpi.h"
#include "framework/test.h"

#include <string.h>

static void writeRsdpV1(uint8_t *p, uint8_t revision) {
    memset(p, 0, 20);
    memcpy(p, "RSD PTR ", 8);
    p[15] = revision;
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) {
        if (i != 8) {
            sum = (uint8_t)(sum + p[i]);
        }
    }
    p[8] = (uint8_t)(0 - sum); /* checksum byte: makes the 20-byte sum land on 0 */
}

TEST(bootAcpiScanFindsV1RsdpAndReturnsAbsoluteAddress) {
    uint8_t buf[64];
    memset(buf, 0xFF, sizeof(buf));
    writeRsdpV1(buf + 32, 0);
    uint64_t base = 0xE0000;
    ASSERT_EQ(bootAcpiScanForRsdp(buf, sizeof(buf), base), base + 32);
}

TEST(bootAcpiScanRejectsBadChecksum) {
    uint8_t buf[64];
    memset(buf, 0xFF, sizeof(buf));
    writeRsdpV1(buf + 32, 0);
    buf[32 + 8] ^= 0xFF; /* corrupt the checksum byte */
    ASSERT_EQ(bootAcpiScanForRsdp(buf, sizeof(buf), 0xE0000), 0ULL);
}

TEST(bootAcpiScanChecksExtendedChecksumForV2) {
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    writeRsdpV1(buf, 2); /* revision 2: also needs a valid extended region */
    uint32_t length = 36;
    memcpy(buf + 20, &length, sizeof(length));
    /* Bytes 24..35 (xsdtAddress + reserved) are already zero; compute the extended checksum over
     * the first 36 bytes, adjusting the extended-checksum byte at offset 32. */
    uint8_t sum = 0;
    for (int i = 0; i < 36; i++) {
        if (i != 32) {
            sum = (uint8_t)(sum + buf[i]);
        }
    }
    buf[32] = (uint8_t)(0 - sum);
    ASSERT_EQ(bootAcpiScanForRsdp(buf, sizeof(buf), 0x1000), 0x1000ULL);
}

TEST(bootAcpiScanRejectsV2WithBadExtendedChecksum) {
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    writeRsdpV1(buf, 2);
    uint32_t length = 36;
    memcpy(buf + 20, &length, sizeof(length));
    buf[32] = 0; /* wrong on purpose */
    ASSERT_EQ(bootAcpiScanForRsdp(buf, sizeof(buf), 0x1000), 0ULL);
}

TEST(bootAcpiScanReturnsZeroWhenNothingMatches) {
    uint8_t buf[64];
    memset(buf, 0xAA, sizeof(buf));
    ASSERT_EQ(bootAcpiScanForRsdp(buf, sizeof(buf), 0xE0000), 0ULL);
}

TEST(bootAcpiScanOnlyChecksSixteenByteAlignedOffsets) {
    uint8_t buf[64];
    memset(buf, 0xFF, sizeof(buf));
    writeRsdpV1(buf + 20, 0); /* not a multiple of 16 */
    ASSERT_EQ(bootAcpiScanForRsdp(buf, sizeof(buf), 0xE0000), 0ULL);
}
