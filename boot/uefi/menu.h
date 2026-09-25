/* The interactive UEFI boot menu (ARCHITECTURE §5.2/§5.5, D-068): drives boot/common/bootmenu.c's
 * pure state machine with real ConIn keys and a 1-second EFI timer, drawing via
 * boot/common/fbtext.c and mirroring to serial. */
#ifndef LOADER_MENU_H
#define LOADER_MENU_H

#include "bootcfg.h"
#include "fbtext.h"
#include "include/efi/efi.h"

/* Runs the menu to completion and returns the chosen entry's 0-based index: `cfg`'s default
 * entry once the countdown expires, whatever arrow keys + Enter left selected, or whatever digit
 * 1-9 was pressed. Only call this when `cfg->timeoutSec > 0` -- the caller boots
 * `cfg->defaultIndex` directly without ever drawing anything otherwise. `text`/`textLen` must be
 * the same boot.cfg buffer `cfg` was parsed from (each entry's display name is resolved from it
 * here). `fx` must already be `fbTextInit`'d. Never fails: a `ReadKeyStroke`/`WaitForEvent` error
 * is treated as an ignorable key and the loop keeps going. */
uint32_t loaderMenuRun(EFI_SYSTEM_TABLE *st, const char *text, uint64_t textLen, const BootCfg *cfg,
                       BootFbText *fx);

#endif
