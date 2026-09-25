/* Host tests for boot/common/memmap.c: the EFI-type mapping table and the rank-sweep normalizer
 * (ARCHITECTURE §5.5/§23, D-060). */
#include "framework/test.h"
#include "memmap.h"

TEST(memMapEfiTypeMapsKnownTypes) {
    ASSERT_EQ(memMapEfiTypeToBootMem(0, 0), (uint32_t)BOOT_MEM_RESERVED); /* Reserved */
    ASSERT_EQ(memMapEfiTypeToBootMem(1, 0), (uint32_t)BOOT_MEM_USABLE);   /* LoaderCode */
    ASSERT_EQ(memMapEfiTypeToBootMem(2, 0), (uint32_t)BOOT_MEM_USABLE);   /* LoaderData */
    ASSERT_EQ(memMapEfiTypeToBootMem(3, 0), (uint32_t)BOOT_MEM_USABLE);   /* BootServicesCode */
    ASSERT_EQ(memMapEfiTypeToBootMem(4, 0), (uint32_t)BOOT_MEM_USABLE);   /* BootServicesData */
    ASSERT_EQ(memMapEfiTypeToBootMem(5, 0), (uint32_t)BOOT_MEM_RESERVED); /* RuntimeServicesCode */
    ASSERT_EQ(memMapEfiTypeToBootMem(6, 0), (uint32_t)BOOT_MEM_RESERVED); /* RuntimeServicesData */
    ASSERT_EQ(memMapEfiTypeToBootMem(7, 0), (uint32_t)BOOT_MEM_USABLE);   /* Conventional */
    ASSERT_EQ(memMapEfiTypeToBootMem(7, MEM_MAP_EFI_ATTRIBUTE_SP), (uint32_t)BOOT_MEM_RESERVED);
    ASSERT_EQ(memMapEfiTypeToBootMem(8, 0), (uint32_t)BOOT_MEM_BAD); /* Unusable */
    ASSERT_EQ(memMapEfiTypeToBootMem(9, 0), (uint32_t)BOOT_MEM_ACPI_RECLAIM);
    ASSERT_EQ(memMapEfiTypeToBootMem(10, 0), (uint32_t)BOOT_MEM_ACPI_NVS);
    ASSERT_EQ(memMapEfiTypeToBootMem(11, 0), (uint32_t)BOOT_MEM_RESERVED); /* MMIO */
    ASSERT_EQ(memMapEfiTypeToBootMem(99, 0), (uint32_t)BOOT_MEM_RESERVED); /* unknown/OEM */
}

TEST(memMapNormalizeSortsAndCoalesces) {
    MemMapInput in[3] = {
        {0x2000, 0x1000, BOOT_MEM_USABLE},
        {0x0, 0x1000, BOOT_MEM_USABLE},
        {0x1000, 0x1000, BOOT_MEM_USABLE},
    };
    BootMemRegion out[8];
    uint32_t nOut = 0;
    uint64_t scratch[16];
    ASSERT_EQ(memMapNormalize(in, 3, out, 8, &nOut, scratch), BOOT_OK);
    ASSERT_EQ(nOut, 1u);
    ASSERT_EQ(out[0].base, 0x0ULL);
    ASSERT_EQ(out[0].length, 0x3000ULL);
    ASSERT_EQ(out[0].type, (uint32_t)BOOT_MEM_USABLE);
}

TEST(memMapNormalizePicksHighestRankOnOverlap) {
    MemMapInput in[2] = {
        {0x0, 0x2000, BOOT_MEM_USABLE},
        {0x1000, 0x1000, BOOT_MEM_KERNEL},
    };
    BootMemRegion out[8];
    uint32_t nOut = 0;
    uint64_t scratch[16];
    ASSERT_EQ(memMapNormalize(in, 2, out, 8, &nOut, scratch), BOOT_OK);
    ASSERT_EQ(nOut, 2u);
    ASSERT_EQ(out[0].base, 0x0ULL);
    ASSERT_EQ(out[0].length, 0x1000ULL);
    ASSERT_EQ(out[0].type, (uint32_t)BOOT_MEM_USABLE);
    ASSERT_EQ(out[1].base, 0x1000ULL);
    ASSERT_EQ(out[1].length, 0x1000ULL);
    ASSERT_EQ(out[1].type, (uint32_t)BOOT_MEM_KERNEL);
}

TEST(memMapNormalizeLeavesHolesUnfilled) {
    MemMapInput in[2] = {
        {0x0, 0x1000, BOOT_MEM_USABLE},
        {0x3000, 0x1000, BOOT_MEM_USABLE},
    };
    BootMemRegion out[8];
    uint32_t nOut = 0;
    uint64_t scratch[16];
    ASSERT_EQ(memMapNormalize(in, 2, out, 8, &nOut, scratch), BOOT_OK);
    ASSERT_EQ(nOut, 2u); /* the [0x1000, 0x3000) hole is never invented as RESERVED */
    ASSERT_EQ(out[0].base, 0x0ULL);
    ASSERT_EQ(out[1].base, 0x3000ULL);
}

TEST(memMapNormalizeRoundsUsableInward) {
    MemMapInput in[1] = {{0x100, 0x2000, BOOT_MEM_USABLE}};
    BootMemRegion out[4];
    uint32_t nOut = 0;
    uint64_t scratch[8];
    ASSERT_EQ(memMapNormalize(in, 1, out, 4, &nOut, scratch), BOOT_OK);
    ASSERT_EQ(nOut, 1u);
    ASSERT_EQ(out[0].base, 0x1000ULL);
    ASSERT_EQ(out[0].length, 0x1000ULL);
}

TEST(memMapNormalizeRoundsOtherTypesOutward) {
    MemMapInput in[1] = {{0x100, 0x1000, BOOT_MEM_RESERVED}};
    BootMemRegion out[4];
    uint32_t nOut = 0;
    uint64_t scratch[8];
    ASSERT_EQ(memMapNormalize(in, 1, out, 4, &nOut, scratch), BOOT_OK);
    ASSERT_EQ(nOut, 1u);
    ASSERT_EQ(out[0].base, 0x0ULL);
    ASSERT_EQ(out[0].length, 0x2000ULL);
}

TEST(memMapNormalizeReturnsCapacityError) {
    MemMapInput in[3] = {
        {0x0, 0x1000, BOOT_MEM_USABLE},
        {0x2000, 0x1000, BOOT_MEM_USABLE},
        {0x4000, 0x1000, BOOT_MEM_USABLE},
    };
    BootMemRegion out[2];
    uint32_t nOut = 0;
    uint64_t scratch[16];
    ASSERT_EQ(memMapNormalize(in, 3, out, 2, &nOut, scratch), BOOT_ERR_MEMMAP_CAPACITY);
}

TEST(memMapCheckOverlayAcceptsContainedRange) {
    MemMapInput efi[1] = {{0x10000, 0x10000, 2}}; /* EfiLoaderData */
    ASSERT_EQ(memMapCheckOverlay(0x11000, 0x1000, efi, 1), BOOT_OK);
}

TEST(memMapCheckOverlayRejectsUncontainedRange) {
    MemMapInput efi[1] = {{0x10000, 0x1000, 2}};
    ASSERT_EQ(memMapCheckOverlay(0x11000, 0x1000, efi, 1), BOOT_ERR_MEMMAP_OVERLAY);
}

TEST(memMapCheckOverlayRejectsWrongEfiType) {
    MemMapInput efi[1] = {{0x10000, 0x10000, 7}}; /* Conventional, not Loader{Code,Data} */
    ASSERT_EQ(memMapCheckOverlay(0x11000, 0x1000, efi, 1), BOOT_ERR_MEMMAP_OVERLAY);
}

TEST(memMapCheckOverlayAllowsZeroLength) {
    ASSERT_EQ(memMapCheckOverlay(0x11000, 0, NULL, 0), BOOT_OK);
}
