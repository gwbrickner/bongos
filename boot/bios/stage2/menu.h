/* The interactive BIOS boot menu (D-110): drives boot/common/bootmenu.c's pure state machine
 * with rmIdle()-yielded polling of serial (bootkey.c) and local (INT 16h) key input plus a BDA
 * tick-counter-based 1-second countdown, drawing via boot/common/fbtext.c and mirroring to
 * serial -- the BIOS counterpart of boot/uefi/menu.c. */
#ifndef BOOT_BIOS_STAGE2_MENU_H
#define BOOT_BIOS_STAGE2_MENU_H

#include "bootcfg.h"
#include "fbtext.h"

#include <stdint.h>

/* Runs the menu to completion and returns the chosen entry's 0-based index: `cfg`'s default
 * entry once the countdown expires, whatever arrow keys + Enter left selected, or whatever digit
 * 1-9 was pressed. Only call this when `cfg->timeoutSec > 0` -- the caller boots
 * `cfg->defaultIndex` directly without ever drawing anything otherwise. `text`/`textLen` must be
 * the same boot.cfg buffer `cfg` was parsed from. `fx` may be NULL (serial-only, D-071). Never
 * fails: an unrecognized key is treated as BOOT_KEY_OTHER and the loop keeps going. */
uint32_t loaderMenuRun(const char *text, uint64_t textLen, const BootCfg *cfg, BootFbText *fx);

#endif
