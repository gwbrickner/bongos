/* See loader-kaslr.h. */
#include "loader-kaslr.h"

#include "bootkaslr.h"
#include "loader-serial.h"

static void logBase(uint64_t base) {
    loaderSerialWriteString("; base=0x");
    loaderSerialWriteHex64(base);
    loaderSerialWriteString("\n");
}

BootStatus loaderKaslrApply(const ElfImage *img, const uint8_t *file, uint64_t fileSize,
                            uint8_t *dest, const uint8_t seed[64], bool kaslrOn,
                            uint64_t *outSlide) {
    *outSlide = 0;
    if (!kaslrOn) {
        loaderSerialWriteString("loader: kaslr: off (boot.cfg)");
        logBase(img->linkBase);
        return BOOT_OK;
    }

    uint64_t slide = 0;
    ElfRelocStats stats;
    BootStatus bst = bootKaslrPickSlide(seed, img->span, &slide);
    if (bst == BOOT_OK) {
        bst = elfRelocate(img, file, fileSize, dest, slide, &stats);
    }
    if (bst == BOOT_OK) {
        loaderSerialWriteString("loader: kaslr: slide=0x");
        loaderSerialWriteHex64(slide);
        loaderSerialWriteString(" base=0x");
        loaderSerialWriteHex64(img->linkBase + slide);
        loaderSerialWriteString(" relocs=");
        loaderSerialWriteUint(stats.applied);
        loaderSerialWriteString("\n");
        *outSlide = slide;
        return BOOT_OK;
    }

    loaderSerialWriteString("loader: kaslr: disabled: ");
    loaderSerialWriteString(bootStatusString(bst));
    logBase(img->linkBase);
    /* elfRelocate() may have failed in pass 2, after part of the image was already slid: throw
     * that image away and start over from the file. */
    return elfLoad(img, file, dest);
}
