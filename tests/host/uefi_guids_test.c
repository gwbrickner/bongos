/* Host tests for boot/uefi/guids.c: checks every GUID constant against its known-correct bytes
 * from the UEFI/ACPI spec text. This is the regression guard for exactly the class of bug the
 * `reviewer` subagent caught in M1.2's first round: EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID's last
 * byte was transcribed as 0x3e instead of the spec's 0x3b, and nothing in M1.2 calls that
 * protocol yet, so the boot harness had no way to notice -- only a byte-for-byte check does. */
#include "framework/test.h"
#include "include/efi/guids.h"

static void assertGuidEquals(EFI_GUID guid, uint32_t data1, uint16_t data2, uint16_t data3,
                             uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4, uint8_t d5,
                             uint8_t d6, uint8_t d7) {
    ASSERT_EQ(guid.Data1, data1);
    ASSERT_EQ(guid.Data2, data2);
    ASSERT_EQ(guid.Data3, data3);
    ASSERT_EQ(guid.Data4[0], d0);
    ASSERT_EQ(guid.Data4[1], d1);
    ASSERT_EQ(guid.Data4[2], d2);
    ASSERT_EQ(guid.Data4[3], d3);
    ASSERT_EQ(guid.Data4[4], d4);
    ASSERT_EQ(guid.Data4[5], d5);
    ASSERT_EQ(guid.Data4[6], d6);
    ASSERT_EQ(guid.Data4[7], d7);
}

TEST(loadedImageProtocolGuidMatchesSpec) {
    /* 5B1B31A1-9562-11D2-8E3F-00A0C969723B */
    assertGuidEquals(gEfiLoadedImageProtocolGuid, 0x5b1b31a1, 0x9562, 0x11d2, 0x8e, 0x3f, 0x00,
                     0xa0, 0xc9, 0x69, 0x72, 0x3b);
}

TEST(simpleFileSystemProtocolGuidMatchesSpec) {
    /* 964E5B22-6459-11D2-8E39-00A0C969723B */
    assertGuidEquals(gEfiSimpleFileSystemProtocolGuid, 0x964e5b22, 0x6459, 0x11d2, 0x8e, 0x39, 0x00,
                     0xa0, 0xc9, 0x69, 0x72, 0x3b);
}

TEST(fileInfoGuidMatchesSpec) {
    /* 09576E92-6D3F-11D2-8E39-00A0C969723B */
    assertGuidEquals(gEfiFileInfoGuid, 0x09576e92, 0x6d3f, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9,
                     0x69, 0x72, 0x3b);
}

TEST(graphicsOutputProtocolGuidMatchesSpec) {
    /* 9042A9DE-23DC-4A38-96FB-7ADED080516A */
    assertGuidEquals(gEfiGraphicsOutputProtocolGuid, 0x9042a9de, 0x23dc, 0x4a38, 0x96, 0xfb, 0x7a,
                     0xde, 0xd0, 0x80, 0x51, 0x6a);
}

TEST(rngProtocolGuidMatchesSpec) {
    /* 3152BCA5-EADE-433D-862E-C01CDC291F44 */
    assertGuidEquals(gEfiRngProtocolGuid, 0x3152bca5, 0xeade, 0x433d, 0x86, 0x2e, 0xc0, 0x1c, 0xdc,
                     0x29, 0x1f, 0x44);
}

TEST(acpi20TableGuidMatchesSpec) {
    /* 8868E871-E4F1-11D3-BC22-0080C73C8881 */
    assertGuidEquals(gEfiAcpi20TableGuid, 0x8868e871, 0xe4f1, 0x11d3, 0xbc, 0x22, 0x00, 0x80, 0xc7,
                     0x3c, 0x88, 0x81);
}

TEST(acpi10TableGuidMatchesSpec) {
    /* EB9D2D30-2D88-11D3-9A16-0090273FC14D */
    assertGuidEquals(gEfiAcpi10TableGuid, 0xeb9d2d30, 0x2d88, 0x11d3, 0x9a, 0x16, 0x00, 0x90, 0x27,
                     0x3f, 0xc1, 0x4d);
}
