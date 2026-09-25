/* ktests for the framebuffer console (ARCHITECTURE §19/§23, ROADMAP M1.4). */
#include "branding.h"
#include "cmdline.h"
#include "format.h"
#include "kernel-boot.h"
#include "ktest.h"

#include <arch/cpu.h>
#include <stddef.h>

#include "drivers/fbcon/fbcon.h"
#include "drivers/serial/uart16550.h"

KTEST(fbcon_geometry) {
    const BootInfo *bi = kernelBootInfo();
    if (bi->fb.phys == 0) {
        return; /* no GOP framebuffer under this configuration; nothing to check */
    }
    if (cmdlineHasToken(kernelCmdline(), "fbcon=off")) {
        KTEST_ASSERT(!fbconActive());
        return;
    }
    KTEST_ASSERT(fbconActive());

    /* D-069's exact formula: scale = clamp(min(width/1280, height/720), 1, 4); cols/rows from
     * that, each capped at 480/270. */
    uint32_t width = bi->fb.width, height = bi->fb.height;
    uint32_t wScale = width / 1280u;
    uint32_t hScale = height / 720u;
    uint32_t expectedScale = (wScale < hScale) ? wScale : hScale;
    if (expectedScale < 1) {
        expectedScale = 1;
    }
    if (expectedScale > 4) {
        expectedScale = 4;
    }
    KTEST_ASSERT_EQ(fbconScale(), expectedScale);

    uint32_t expectedCols = width / (8u * expectedScale);
    if (expectedCols > 480u) {
        expectedCols = 480u;
    }
    uint32_t expectedRows = height / (16u * expectedScale);
    if (expectedRows > 270u) {
        expectedRows = 270u;
    }
    KTEST_ASSERT_EQ(fbconCols(), expectedCols);
    KTEST_ASSERT_EQ(fbconRows(), expectedRows);

    KTEST_ASSERT(bi->fb.pitch >= bi->fb.width * 4);
    KTEST_ASSERT_EQ(bi->fb.bpp, 32);
}

/* Renders a deterministic test card (not the literal boot log, which varies with OVMF's memory
 * map and BRANDING_VERSION) for the GUI screenshot harness (D-070): a scroll exercise, the
 * product name, every fg and every bg color, and every printable glyph. When `ktest.shots=1` is
 * on the cmdline, acks the render over serial and blocks (bounded only by the harness's own
 * timeout) until the harness has taken its screendump and replied -- otherwise it just renders
 * and returns immediately. */
KTEST(fbcon_screenshot) {
    if (!fbconActive()) {
        bool wantedShot = cmdlineHasToken(kernelCmdline(), "ktest.shots=1");
        KTEST_ASSERT(!wantedShot);
        return;
    }

    fbconClear();
    uint32_t rows = fbconRows();
    char line[64];
    for (uint32_t i = 0; i < rows + 5; i++) {
        fbconSetColor(7, 0);
        int len = ksnprintf(line, sizeof(line), "scroll %u\n", i);
        size_t n = (len > 0) ? ((size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1) : 0;
        fbconWrite(line, n);
    }

    static const char hexDigits[] = "0123456789ABCDEF";

    fbconSetColor(15, 0);
    fbconWrite(BRANDING_NAME, sizeof(BRANDING_NAME) - 1);
    fbconWrite("\n", 1);

    for (uint32_t c = 0; c < 16; c++) {
        fbconSetColor((uint8_t)c, 0);
        fbconWrite(&hexDigits[c], 1);
    }
    fbconWrite("\n", 1);

    for (uint32_t c = 0; c < 16; c++) {
        fbconSetColor(15, (uint8_t)c);
        fbconWrite(&hexDigits[c], 1);
    }
    fbconSetColor(7, 0);
    fbconWrite("\n", 1);

    for (int row = 0; row < 3; row++) {
        for (int i = 0; i < 32; i++) {
            int code = 0x20 + row * 32 + i;
            if (code > 0x7E) {
                break;
            }
            char ch = (char)code;
            fbconWrite(&ch, 1);
        }
        fbconWrite("\n", 1);
    }

    if (cmdlineHasToken(kernelCmdline(), "ktest.shots=1")) {
        /* serialInit() deliberately doesn't clear the FIFOs (klogInit()'s own comment), so a
         * stale byte from before this point could otherwise pre-satisfy the wait below. */
        serialDrainRx();
        serialWriteString("SCREENSHOT kernel\n");
        uint8_t b = 0;
        for (;;) {
            if (serialTryReadByte(&b)) {
                if (b == '\n') {
                    break;
                }
            } else {
                archPause();
            }
        }
    }
}
