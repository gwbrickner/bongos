/* See menu.h. Firmware-specific glue only (ConIn key polling, the EFI timer, the outer wait
 * loop) -- the drawing/logging/state machine are shared code (D-110, D-068):
 * boot/common/hw/menu-ui.c and boot/common/bootmenu.c. */
#include "menu.h"

#include "loader-menu-ui.h"
#include "loader-serial.h"

#define MENU_TIMER_PERIOD_100NS 10000000ULL /* 1 second, EFI's 100ns timer units */

static BootKey classifyKey(const EFI_INPUT_KEY *k) {
    BootKey key = {BOOT_KEY_OTHER, 0};
    if (k->ScanCode == 0x01) { /* SCAN_UP */
        key.kind = BOOT_KEY_UP;
    } else if (k->ScanCode == 0x02) { /* SCAN_DOWN */
        key.kind = BOOT_KEY_DOWN;
    } else if (k->UnicodeChar == 0x0D || k->UnicodeChar == 0x0A) { /* CR or LF */
        key.kind = BOOT_KEY_ENTER;
    } else if (k->UnicodeChar >= '1' && k->UnicodeChar <= '9') {
        key.kind = BOOT_KEY_DIGIT;
        key.digit = (uint32_t)(k->UnicodeChar - '0');
    }
    return key;
}

uint32_t loaderMenuRun(EFI_SYSTEM_TABLE *st, const char *text, uint64_t textLen, const BootCfg *cfg,
                       BootFbText *fx) {
    static MenuUi ui; /* static, like handoff.c's other big buffers: not this loader's tiny stack */
    menuUiInit(&ui, text, textLen, cfg);

    BootMenuState state;
    bootMenuInit(&state, cfg);

    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conIn = st->ConIn;
    /* Reset *before* printing the "menu ready" sync marker (found the hard way: a GUI test
     * script's `send` can follow right on the heels of its `expect "loader: menu ready"` with no
     * intervening delay, and EFI_SIMPLE_TEXT_INPUT_PROTOCOL.Reset()'s "reset the input device
     * hardware" can discard any keystroke that arrived between resume and this call -- if Reset
     * ran after the marker, a fast-enough `send` would be silently swallowed here rather than
     * seen by the WaitForKey loop below). */
    conIn->Reset(conIn, FALSE);

    menuUiDrawMenu(&ui, fx, &state, cfg->entryCount);
    menuUiSerialPrintMenu(&ui, cfg->entryCount, cfg->defaultIndex);
    /* Printed only after drawing and after Reset: this is the harness's sync marker (D-070) for
     * both screendump timing and "it's now safe to send a key". */
    loaderSerialWriteString("loader: menu ready\n");

    bool useTimer = state.timeoutSec != BOOT_CFG_TIMEOUT_FOREVER;
    EFI_EVENT timerEvent = NULL;
    if (useTimer) {
        bs->CreateEvent(EVT_TIMER, 0, NULL, NULL, &timerEvent);
        bs->SetTimer(timerEvent, TimerPeriodic, MENU_TIMER_PERIOD_100NS);
    }

    for (;;) {
        EFI_EVENT waitList[2];
        UINTN numEvents = 0;
        waitList[numEvents++] = conIn->WaitForKey;
        if (useTimer) {
            waitList[numEvents++] = timerEvent;
        }
        UINTN index = 0;
        if (EFI_ERROR(bs->WaitForEvent(numEvents, waitList, &index))) {
            continue;
        }

        BootMenuAction action = BOOT_MENU_NONE;
        uint32_t oldSelected = state.selected;
        if (index == 0) {
            EFI_INPUT_KEY key;
            if (EFI_ERROR(conIn->ReadKeyStroke(conIn, &key))) {
                continue;
            }
            action = bootMenuKey(&state, classifyKey(&key));
            if (action == BOOT_MENU_REDRAW_ROWS) {
                menuUiLogSelected(state.selected);
            }
        } else {
            action = bootMenuTick(&state);
            if (action == BOOT_MENU_BOOT) {
                menuUiLogTimeoutBoot();
            }
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

    if (useTimer) {
        bs->SetTimer(timerEvent, TimerCancel, 0);
        bs->CloseEvent(timerEvent);
    }

    menuUiLogBootingEntry(&ui, state.selected);
    menuUiDrawBooting(fx);

    return state.selected;
}
