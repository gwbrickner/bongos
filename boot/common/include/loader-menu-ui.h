/* The boot menu's drawing and logging glue (ARCHITECTURE §5.2/§5.5, D-070/D-110), shared between
 * the UEFI loader (boot/uefi/menu.c) and the BIOS loader (M2.5) so both firmwares produce
 * pixel-identical menus and identical serial log lines by construction, rather than two
 * hand-tuned copies that can drift. Builds on boot/common/bootmenu.c's pure state machine and
 * boot/common/fbtext.c's text primitive; each loader's own menu.c keeps only its
 * firmware-specific key-input polling and the outer wait loop. */
#ifndef BOOT_COMMON_LOADER_MENU_UI_H
#define BOOT_COMMON_LOADER_MENU_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "bootcfg.h"
#include "bootmenu.h"
#include "fbtext.h"

typedef struct {
    BootCfgEntry entries[BOOT_CFG_MAX_ENTRIES];
} MenuUi;

/* Resolves every entry's display name once, from the same `text`/`textLen` boot.cfg buffer
 * `cfg` was parsed from, so every later draw/log call can just read a NUL-terminated name. Call
 * this once before any other menuUi* function. No locks, boot-time only; pure. */
void menuUiInit(MenuUi *ui, const char *text, uint64_t textLen, const BootCfg *cfg);

/* `entries[index].name`, or BRANDING_NAME if that entry has no explicit name (an unnamed
 * implicit single-entry boot.cfg, ARCHITECTURE §5.2). No locks, boot-time only; pure. */
const char *menuUiEntryName(const MenuUi *ui, uint32_t index);

/* Row layout: entry rows start right below the title, and the footer ("Up/Down..." help text)
 * and countdown rows sit right below the last entry row. No locks, boot-time only; pure. */
uint32_t menuUiEntryRow(uint32_t index);
uint32_t menuUiFooterRow(uint32_t entryCount);
uint32_t menuUiCountdownRow(uint32_t entryCount);

/* Draws the full menu (title, every entry row, the footer, the countdown) from scratch. A no-op
 * for any drawing this does if `fx` is NULL (ARCHITECTURE §5.2: no framebuffer, serial-only,
 * D-071) -- every fbText* call already tolerates a NULL fx. No locks, boot-time only. */
void menuUiDrawMenu(const MenuUi *ui, BootFbText *fx, const BootMenuState *state,
                    uint32_t entryCount);

/* Redraws just one entry row ("N. <name>", highlighted iff `selected`). No-op if `fx` is NULL. */
void menuUiDrawRow(const MenuUi *ui, BootFbText *fx, uint32_t row, uint32_t index, bool selected);

/* Redraws just the countdown row ("Booting <name> in Ns", or blank if inactive/FOREVER). No-op if
 * `fx` is NULL. */
void menuUiDrawCountdown(const MenuUi *ui, BootFbText *fx, uint32_t row,
                         const BootMenuState *state);

/* Clears the screen and shows "Booting..." -- the last thing drawn before the menu hands off to
 * loading the chosen entry. No-op if `fx` is NULL. */
void menuUiDrawBooting(BootFbText *fx);

/* Mirrors the whole menu to serial ("loader: boot menu:" + one numbered line per entry, `*`
 * marking the default). */
void menuUiSerialPrintMenu(const MenuUi *ui, uint32_t entryCount, uint32_t defaultIndex);

/* One-line log helpers for the three points ARCHITECTURE's boot-menu contract (and the test
 * harness's --expect-serial checks) require exact wording for: a key moved the selection, the
 * countdown expired, and the final boot choice. */
void menuUiLogSelected(uint32_t selectedIndex);
void menuUiLogTimeoutBoot(void);
void menuUiLogBootingEntry(const MenuUi *ui, uint32_t selectedIndex);

#endif
