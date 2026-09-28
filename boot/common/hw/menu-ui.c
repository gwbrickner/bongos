/* See loader-menu-ui.h. Extracted from boot/uefi/menu.c (D-110) so BIOS stage2 draws and logs
 * the identical menu instead of a second hand-tuned copy. */
#include "loader-menu-ui.h"

#include "branding.h"
#include "loader-serial.h"

#include <stddef.h>

#define MENU_TITLE_ROW       1
#define MENU_FIRST_ENTRY_ROW 3

void menuUiInit(MenuUi *ui, const char *text, uint64_t textLen, const BootCfg *cfg) {
    for (uint32_t i = 0; i < cfg->entryCount; i++) {
        bootCfgResolveEntry(text, textLen, cfg, i, &ui->entries[i]);
    }
}

const char *menuUiEntryName(const MenuUi *ui, uint32_t index) {
    return ui->entries[index].name[0] != '\0' ? ui->entries[index].name : BRANDING_NAME;
}

uint32_t menuUiEntryRow(uint32_t index) {
    return MENU_FIRST_ENTRY_ROW + index;
}

uint32_t menuUiFooterRow(uint32_t entryCount) {
    return MENU_FIRST_ENTRY_ROW + entryCount + 1;
}

uint32_t menuUiCountdownRow(uint32_t entryCount) {
    return MENU_FIRST_ENTRY_ROW + entryCount + 2;
}

/* "N. <name>", highlighted (bg 4/fg 15) when selected, plain (bg 0/fg 7) otherwise -- fills the
 * row's background first so the highlight bar extends past the name itself. */
void menuUiDrawRow(const MenuUi *ui, BootFbText *fx, uint32_t row, uint32_t index, bool selected) {
    if (fx == NULL) {
        return;
    }
    char line[BOOT_CFG_NAME_MAX + 8];
    uint32_t pos = 0;
    line[pos++] = (char)('0' + (index + 1)); /* index+1 is 1-9: BOOT_CFG_MAX_ENTRIES is 9 */
    line[pos++] = '.';
    line[pos++] = ' ';
    const char *name = menuUiEntryName(ui, index);
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

void menuUiDrawCountdown(const MenuUi *ui, BootFbText *fx, uint32_t row, const BootMenuState *state) {
    if (fx == NULL) {
        return;
    }
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
    const char *name = menuUiEntryName(ui, state->selected);
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

void menuUiDrawMenu(const MenuUi *ui, BootFbText *fx, const BootMenuState *state, uint32_t entryCount) {
    fbTextClear(fx, 0);
    fbTextPutString(fx, MENU_TITLE_ROW, 2, BRANDING_NAME " boot menu", 15, 0);
    for (uint32_t i = 0; i < entryCount; i++) {
        menuUiDrawRow(ui, fx, menuUiEntryRow(i), i, i == state->selected);
    }
    fbTextPutString(fx, menuUiFooterRow(entryCount), 2,
                    "Up/Down to select, Enter to boot, 1-9 to boot an entry", 8, 0);
    menuUiDrawCountdown(ui, fx, menuUiCountdownRow(entryCount), state);
}

void menuUiDrawBooting(BootFbText *fx) {
    fbTextClear(fx, 0);
    fbTextPutString(fx, 0, 2, "Booting...", 15, 0);
}

void menuUiSerialPrintMenu(const MenuUi *ui, uint32_t entryCount, uint32_t defaultIndex) {
    loaderSerialWriteString("loader: boot menu:\n");
    for (uint32_t i = 0; i < entryCount; i++) {
        loaderSerialWriteString(i == defaultIndex ? "  * " : "    ");
        loaderSerialWriteUint(i + 1);
        loaderSerialWriteString(". ");
        loaderSerialWriteString(menuUiEntryName(ui, i));
        loaderSerialWriteString("\n");
    }
}

void menuUiLogSelected(uint32_t selectedIndex) {
    loaderSerialWriteString("loader: selected ");
    loaderSerialWriteUint(selectedIndex + 1);
    loaderSerialWriteString("\n");
}

void menuUiLogTimeoutBoot(void) {
    loaderSerialWriteString("loader: timeout, booting default\n");
}

void menuUiLogBootingEntry(const MenuUi *ui, uint32_t selectedIndex) {
    loaderSerialWriteString("loader: booting entry ");
    loaderSerialWriteUint(selectedIndex + 1);
    loaderSerialWriteString(" (");
    loaderSerialWriteString(menuUiEntryName(ui, selectedIndex));
    loaderSerialWriteString(")\n");
}
