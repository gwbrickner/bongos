/* See menu.h. Firmware-specific glue only (D-110): rmIdle() so BIOS's own timer/keyboard IRQs get
 * real-mode time to run, bootkey.c for the serial ANSI arrow-key stream (the test harness drives
 * the menu over COM1, and SeaBIOS itself never reads the serial port), INT 16h for local
 * keyboard input, and the BDA tick counter for the countdown -- driving the same shared
 * boot/common/bootmenu.c state machine and boot/common/hw/menu-ui.c drawing/logging UEFI's own
 * menu.c uses, so both firmwares produce pixel-identical menus and identical log lines. */
#include "menu.h"

#include "bootkey.h"
#include "bootmem.h"
#include "loader-menu-ui.h"
#include "loader-serial.h"
#include "rm.h"

#include <stdbool.h>

/* The classic PC/XT BIOS tick rate is ~18.2065 Hz; BIOS_TICKS_PER_SECOND is its floor, so this
 * countdown runs very slightly slow rather than fast -- matching D-110's "close enough" contract
 * for a menu countdown, not a precision timer. */
#define BIOS_TICKS_PER_SECOND 18u
#define BDA_TICK_COUNTER_ADDR 0x046Cu /* BDA "clock ticks since midnight" (u32) */

static uint32_t readBdaTicks(void) {
    return *(volatile const uint32_t *)bootPhysToPtr(BDA_TICK_COUNTER_ADDR);
}

static BootKey classifyBiosScanKey(uint8_t scanCode, uint8_t asciiChar) {
    BootKey key = {BOOT_KEY_OTHER, 0};
    if (scanCode == 0x48u) { /* extended scan code: up arrow */
        key.kind = BOOT_KEY_UP;
    } else if (scanCode == 0x50u) { /* down arrow */
        key.kind = BOOT_KEY_DOWN;
    } else if (asciiChar == 0x0Du || asciiChar == 0x0Au) { /* CR or LF */
        key.kind = BOOT_KEY_ENTER;
    } else if (asciiChar >= '1' && asciiChar <= '9') {
        key.kind = BOOT_KEY_DIGIT;
        key.digit = (uint32_t)(asciiChar - '0');
    }
    return key;
}

/* INT 16h AH=01h: check for a pending keystroke without consuming it. True (with *outScan/
 * *outAscii filled) iff one is pending -- the BIOS reports that via ZF=0, which the thunk exposes
 * as bit 6 of the returned eflags. */
static bool biosKeyReady(uint8_t *outScan, uint8_t *outAscii) {
    RmRegs r;
    bootMemset(&r, 0, sizeof(r));
    r.eax = 0x0100u;
    rmInt(0x16, &r);
    if ((r.eflags & (1u << 6)) != 0) {
        return false;
    }
    *outAscii = (uint8_t)(r.eax & 0xFFu);
    *outScan = (uint8_t)((r.eax >> 8) & 0xFFu);
    return true;
}

/* INT 16h AH=00h: consumes the keystroke a prior biosKeyReady() confirmed is pending. */
static void biosKeyRead(uint8_t *outScan, uint8_t *outAscii) {
    RmRegs r;
    bootMemset(&r, 0, sizeof(r));
    r.eax = 0x0000u;
    rmInt(0x16, &r);
    *outAscii = (uint8_t)(r.eax & 0xFFu);
    *outScan = (uint8_t)((r.eax >> 8) & 0xFFu);
}

uint32_t loaderMenuRun(const char *text, uint64_t textLen, const BootCfg *cfg, BootFbText *fx) {
    static MenuUi ui; /* static, like handoff.c's other big buffers: not this loader's tiny stack */
    menuUiInit(&ui, text, textLen, cfg);

    BootMenuState state;
    bootMenuInit(&state, cfg);

    /* Drain any input queued before this point -- both COM1 (the GUI test harness can queue a
     * byte the instant it sees any earlier loader output) and the BIOS's own INT 16h keyboard
     * buffer (a stray keystroke held over from POST) -- *before* drawing/printing the "menu
     * ready" sync marker below, matching UEFI menu.c's ConIn->Reset() ordering (D-110). Draining
     * after the marker would risk either silently swallowing a fast `send` that races the drain,
     * or letting a stale byte register as BOOT_KEY_OTHER and stop the countdown / trigger an
     * unintended immediate boot. */
    {
        uint8_t drainByte;
        while (loaderSerialReadByte(&drainByte)) {
            /* discard */
        }
        uint8_t drainScan, drainAscii;
        while (biosKeyReady(&drainScan, &drainAscii)) {
            biosKeyRead(&drainScan, &drainAscii);
        }
    }

    menuUiDrawMenu(&ui, fx, &state, cfg->entryCount);
    menuUiSerialPrintMenu(&ui, cfg->entryCount, cfg->defaultIndex);
    /* The harness's sync marker (D-070) for both screendump timing and "it's now safe to send a
     * key" -- printed only after drawing, matching UEFI menu.c's own ordering. */
    loaderSerialWriteString("loader: menu ready\n");

    BootKeyParser serialParser;
    bootKeyParserInit(&serialParser);

    bool useTimer = state.timeoutSec != BOOT_CFG_TIMEOUT_FOREVER;
    uint32_t lastTicks = readBdaTicks();
    uint32_t nextTickTarget = lastTicks + BIOS_TICKS_PER_SECOND;

    for (;;) {
        rmIdle();

        BootMenuAction action = BOOT_MENU_NONE;
        uint32_t oldSelected = state.selected;
        bool gotKey = false;
        BootKey key = {BOOT_KEY_OTHER, 0};

        uint8_t serialByte;
        while (loaderSerialReadByte(&serialByte)) {
            if (bootKeyParserFeed(&serialParser, serialByte, &key)) {
                gotKey = true;
                break;
            }
        }

        if (!gotKey) {
            uint8_t scan, ascii;
            if (biosKeyReady(&scan, &ascii)) {
                biosKeyRead(&scan, &ascii);
                key = classifyBiosScanKey(scan, ascii);
                gotKey = true;
            }
        }

        if (gotKey) {
            action = bootMenuKey(&state, key);
            if (action == BOOT_MENU_REDRAW_ROWS) {
                menuUiLogSelected(state.selected);
            }
        } else if (useTimer) {
            uint32_t ticks = readBdaTicks();
            if (ticks < lastTicks) {
                /* Midnight wraparound (the BDA counter resets to 0 at 24h): resync rather than
                 * computing a bogus huge "elapsed" delta from an apparent decrease. */
                nextTickTarget = ticks + BIOS_TICKS_PER_SECOND;
            } else if (ticks >= nextTickTarget) {
                nextTickTarget += BIOS_TICKS_PER_SECOND;
                action = bootMenuTick(&state);
                if (action == BOOT_MENU_BOOT) {
                    menuUiLogTimeoutBoot();
                }
            }
            lastTicks = ticks;
        }

        switch (action) {
            case BOOT_MENU_REDRAW_ROWS:
                menuUiDrawRow(&ui, fx, menuUiEntryRow(oldSelected), oldSelected, false);
                menuUiDrawRow(&ui, fx, menuUiEntryRow(state.selected), state.selected, true);
                menuUiDrawCountdown(&ui, fx, menuUiCountdownRow(cfg->entryCount), &state);
                break;
            case BOOT_MENU_REDRAW_COUNTDOWN:
                menuUiDrawCountdown(&ui, fx, menuUiCountdownRow(cfg->entryCount), &state);
                break;
            case BOOT_MENU_BOOT:
                goto booted;
            case BOOT_MENU_NONE:
            default:
                break;
        }
    }
booted:
    menuUiLogBootingEntry(&ui, state.selected);
    menuUiDrawBooting(fx);

    return state.selected;
}
