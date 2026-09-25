/* See menu.h. */
#include "menu.h"

#include "bootmem.h"
#include "bootmenu.h"
#include "branding.h"
#include "serial.h"

#define MENU_TIMER_PERIOD_100NS 10000000ULL /* 1 second, EFI's 100ns timer units */
#define MENU_TITLE_ROW          1
#define MENU_FIRST_ENTRY_ROW    3

/* Resolved once at the top of loaderMenuRun() so every redraw/serial-print can just read a
 * NUL-terminated name -- static rather than on the stack, like handoff.c's other big buffers
 * (BOOT_CFG_MAX_ENTRIES * sizeof(BootCfgEntry) is a few KiB, more than this loader's small
 * default stack should carry). */
static BootCfgEntry menuEntries[BOOT_CFG_MAX_ENTRIES];

static const char *entryDisplayName(uint32_t index) {
    return menuEntries[index].name[0] != '\0' ? menuEntries[index].name : BRANDING_NAME;
}

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

/* "N. <name>", highlighted (bg 4/fg 15) when selected, plain (bg 0/fg 7) otherwise -- fills the
 * row's background first so the highlight bar extends past the name itself. */
static void drawRow(BootFbText *fx, uint32_t row, uint32_t index, bool selected) {
    char line[BOOT_CFG_NAME_MAX + 8];
    uint32_t pos = 0;
    line[pos++] = (char)('0' + (index + 1)); /* index+1 is 1-9: BOOT_CFG_MAX_ENTRIES is 9 */
    line[pos++] = '.';
    line[pos++] = ' ';
    const char *name = entryDisplayName(index);
    for (uint32_t i = 0; name[i] != '\0' && pos < sizeof(line) - 1; i++) {
        line[pos++] = name[i];
    }
    line[pos] = '\0';

    uint8_t fg = selected ? 15 : 7;
    uint8_t bg = selected ? 4 : 0;
    uint32_t rightCol = fx->cols > 3 ? fx->cols - 3 : fx->cols;
    fbTextFillRow(fx, row, 2, rightCol, bg);
    fbTextPutString(fx, row, 2, line, fg, bg);
}

static void writeUintInto(char *buf, uint32_t *pos, uint32_t bufCap, uint32_t v) {
    char digits[10];
    int n = 0;
    if (v == 0) {
        digits[n++] = '0';
    } else {
        while (v > 0 && n < (int)sizeof(digits)) {
            digits[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    while (n > 0 && *pos < bufCap - 1) {
        buf[(*pos)++] = digits[--n];
    }
}

static void drawCountdown(BootFbText *fx, uint32_t row, const BootMenuState *state) {
    fbTextFillRow(fx, row, 2, fx->cols, 0);
    if (!state->countdownActive || state->timeoutSec == BOOT_CFG_TIMEOUT_FOREVER) {
        return;
    }
    char line[96];
    uint32_t pos = 0;
    static const char prefix[] = "Booting ";
    for (uint32_t i = 0; prefix[i] != '\0' && pos < sizeof(line) - 1; i++) {
        line[pos++] = prefix[i];
    }
    const char *name = entryDisplayName(state->selected);
    for (uint32_t i = 0; name[i] != '\0' && pos < sizeof(line) - 16; i++) {
        line[pos++] = name[i];
    }
    static const char suffix[] = " in ";
    for (uint32_t i = 0; suffix[i] != '\0' && pos < sizeof(line) - 8; i++) {
        line[pos++] = suffix[i];
    }
    writeUintInto(line, &pos, sizeof(line), state->remainingSec);
    if (pos < sizeof(line) - 1) {
        line[pos++] = 's';
    }
    line[pos] = '\0';
    fbTextPutString(fx, row, 2, line, 8, 0);
}

static uint32_t footerRow(uint32_t entryCount) {
    return MENU_FIRST_ENTRY_ROW + entryCount + 1;
}
static uint32_t countdownRow(uint32_t entryCount) {
    return MENU_FIRST_ENTRY_ROW + entryCount + 2;
}

static void drawMenu(BootFbText *fx, const BootMenuState *state, uint32_t entryCount) {
    fbTextClear(fx, 0);
    fbTextPutString(fx, MENU_TITLE_ROW, 2, BRANDING_NAME " boot menu", 15, 0);
    for (uint32_t i = 0; i < entryCount; i++) {
        drawRow(fx, MENU_FIRST_ENTRY_ROW + i, i, i == state->selected);
    }
    fbTextPutString(fx, footerRow(entryCount), 2,
                    "Up/Down to select, Enter to boot, 1-9 to boot an entry", 8, 0);
    drawCountdown(fx, countdownRow(entryCount), state);
}

static void serialPrintMenu(uint32_t entryCount, uint32_t defaultIndex) {
    loaderSerialWriteString("loader: boot menu:\n");
    for (uint32_t i = 0; i < entryCount; i++) {
        loaderSerialWriteString(i == defaultIndex ? "  * " : "    ");
        loaderSerialWriteUint(i + 1);
        loaderSerialWriteString(". ");
        loaderSerialWriteString(entryDisplayName(i));
        loaderSerialWriteString("\n");
    }
}

uint32_t loaderMenuRun(EFI_SYSTEM_TABLE *st, const char *text, uint64_t textLen, const BootCfg *cfg,
                       BootFbText *fx) {
    for (uint32_t i = 0; i < cfg->entryCount; i++) {
        bootCfgResolveEntry(text, textLen, cfg, i, &menuEntries[i]);
    }

    BootMenuState state;
    bootMenuInit(&state, cfg);

    drawMenu(fx, &state, cfg->entryCount);
    serialPrintMenu(cfg->entryCount, cfg->defaultIndex);
    /* Printed only after drawing: this is the harness's screendump sync marker (D-070). */
    loaderSerialWriteString("loader: menu ready\n");

    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *conIn = st->ConIn;
    conIn->Reset(conIn, FALSE);

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
                loaderSerialWriteString("loader: selected ");
                loaderSerialWriteUint(state.selected + 1);
                loaderSerialWriteString("\n");
            }
        } else {
            action = bootMenuTick(&state);
            if (action == BOOT_MENU_BOOT) {
                loaderSerialWriteString("loader: timeout, booting default\n");
            }
        }

        switch (action) {
            case BOOT_MENU_REDRAW_ROWS:
                drawRow(fx, MENU_FIRST_ENTRY_ROW + oldSelected, oldSelected, false);
                drawRow(fx, MENU_FIRST_ENTRY_ROW + state.selected, state.selected, true);
                drawCountdown(fx, countdownRow(cfg->entryCount), &state);
                break;
            case BOOT_MENU_REDRAW_COUNTDOWN:
                drawCountdown(fx, countdownRow(cfg->entryCount), &state);
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

    loaderSerialWriteString("loader: booting entry ");
    loaderSerialWriteUint(state.selected + 1);
    loaderSerialWriteString(" (");
    loaderSerialWriteString(entryDisplayName(state.selected));
    loaderSerialWriteString(")\n");

    fbTextClear(fx, 0);
    fbTextPutString(fx, 0, 2, "Booting...", 15, 0);

    return state.selected;
}
