/* The boot menu's pure state machine (ARCHITECTURE §5.2/§5.5, D-068), shared between the UEFI
 * loader (boot/uefi/menu.c, which owns the GOP/ConIn firmware calls) and the BIOS loader (M2.5).
 * No I/O, no timing, no drawing: the caller feeds it key events and clock ticks and reads back
 * what changed. Host-tested (tests/host/boot_bootmenu_test.c) and driven for real by the firmware
 * glue in boot/uefi/menu.c. */
#ifndef BOOT_COMMON_BOOTMENU_H
#define BOOT_COMMON_BOOTMENU_H

#include <stdbool.h>
#include <stdint.h>

#include "bootcfg.h"

typedef enum {
    BOOT_KEY_NONE = 0,
    BOOT_KEY_UP,
    BOOT_KEY_DOWN,
    BOOT_KEY_ENTER,
    BOOT_KEY_DIGIT, /* '1'-'9'; BootMenuState.digitPressed holds which */
    BOOT_KEY_OTHER, /* any other key: stops the countdown, doesn't move the selection */
} BootKeyKind;

typedef struct {
    BootKeyKind kind;
    uint32_t digit; /* 1-9, only meaningful when kind == BOOT_KEY_DIGIT */
} BootKey;

typedef enum {
    BOOT_MENU_NONE = 0,         /* nothing changed the caller needs to act on */
    BOOT_MENU_REDRAW_ROWS,      /* the selection moved: redraw old/newSelected rows */
    BOOT_MENU_REDRAW_COUNTDOWN, /* the countdown number changed (or was blanked) */
    BOOT_MENU_BOOT,             /* selection is final: boot entries[selected] */
} BootMenuAction;

typedef struct {
    uint32_t entryCount;   /* cfg->entryCount, cached at Init so callers don't need `cfg` around */
    uint32_t selected;     /* currently highlighted entry, 0-based */
    uint32_t oldSelected;  /* valid only when the last bootMenuKey() returned REDRAW_ROWS */
    uint32_t timeoutSec;   /* BOOT_CFG_TIMEOUT_FOREVER means no countdown at all */
    uint32_t remainingSec; /* ticks down to 0; meaningless if timeoutSec is FOREVER */
    bool countdownActive;  /* false once any key besides UP/DOWN stops it, or it reaches 0 */
} BootMenuState;

/* Starts the state machine at cfg->defaultIndex selected and cfg->timeoutSec/FOREVER as the
 * countdown. No locks, boot-time only; pure. */
void bootMenuInit(BootMenuState *state, const BootCfg *cfg);

/* Feeds one key event. Returns what the caller should do:
 *  - UP/DOWN: moves `selected` (clamped, no wrap) and stops the countdown; REDRAW_ROWS if it
 *    actually moved (and stops the countdown, so REDRAW_COUNTDOWN follows if it was running),
 *    NONE if already at that end.
 *  - ENTER: BOOT (boot the current `selected`).
 *  - DIGIT n (1-9): if n <= entryCount, BOOT with `selected` set to n-1; otherwise NONE.
 *  - OTHER: stops the countdown (REDRAW_COUNTDOWN if it was running), otherwise NONE.
 * No locks, boot-time only; pure. */
BootMenuAction bootMenuKey(BootMenuState *state, BootKey key);

/* Advances the countdown by one second (the caller's timer fires once a second). Returns
 * REDRAW_COUNTDOWN if `remainingSec` changed, BOOT once it reaches 0, NONE if the countdown
 * isn't active (FOREVER, or already stopped by a key). No locks, boot-time only; pure. */
BootMenuAction bootMenuTick(BootMenuState *state);

#endif
