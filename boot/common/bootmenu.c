/* See bootmenu.h. */
#include "include/bootmenu.h"

#include <stddef.h>

void bootMenuInit(BootMenuState *state, const BootCfg *cfg) {
    if (state == NULL || cfg == NULL) {
        return;
    }
    state->entryCount = cfg->entryCount;
    state->selected = cfg->defaultIndex;
    state->oldSelected = cfg->defaultIndex;
    state->timeoutSec = cfg->timeoutSec;
    state->remainingSec = cfg->timeoutSec;        /* meaningless when timeoutSec is FOREVER */
    state->countdownActive = cfg->timeoutSec > 0; /* 0 means "no menu", but the caller still
                                                   * shows it if it calls bootMenuInit at all --
                                                   * timeout=0 files boot without ever reaching
                                                   * the menu, checked by the caller, not here */
}

static BootMenuAction stopCountdown(BootMenuState *state) {
    if (!state->countdownActive) {
        return BOOT_MENU_NONE;
    }
    state->countdownActive = false;
    return BOOT_MENU_REDRAW_COUNTDOWN;
}

BootMenuAction bootMenuKey(BootMenuState *state, BootKey key) {
    if (state == NULL) {
        return BOOT_MENU_NONE;
    }
    switch (key.kind) {
        case BOOT_KEY_UP: {
            /* A move always stops the countdown too, but BootMenuAction can only report one
             * thing at a time -- REDRAW_ROWS takes priority since the caller redraws the whole
             * menu (rows + countdown row) on either action in practice (boot/uefi/menu.c), and a
             * lone REDRAW_COUNTDOWN only matters when nothing else changed. */
            bool wasActive = state->countdownActive;
            state->countdownActive = false;
            if (state->selected == 0) {
                return wasActive ? BOOT_MENU_REDRAW_COUNTDOWN : BOOT_MENU_NONE;
            }
            state->oldSelected = state->selected;
            state->selected--;
            return BOOT_MENU_REDRAW_ROWS;
        }
        case BOOT_KEY_DOWN: {
            bool wasActive = state->countdownActive;
            state->countdownActive = false;
            if (state->selected + 1 >= state->entryCount) {
                return wasActive ? BOOT_MENU_REDRAW_COUNTDOWN : BOOT_MENU_NONE;
            }
            state->oldSelected = state->selected;
            state->selected++;
            return BOOT_MENU_REDRAW_ROWS;
        }
        case BOOT_KEY_ENTER:
            return BOOT_MENU_BOOT;
        case BOOT_KEY_DIGIT:
            if (key.digit >= 1 && key.digit <= state->entryCount) {
                state->oldSelected = state->selected;
                state->selected = key.digit - 1;
                return BOOT_MENU_BOOT;
            }
            return BOOT_MENU_NONE;
        case BOOT_KEY_OTHER:
            return stopCountdown(state);
        case BOOT_KEY_NONE:
        default:
            return BOOT_MENU_NONE;
    }
}

BootMenuAction bootMenuTick(BootMenuState *state) {
    if (state == NULL || !state->countdownActive || state->timeoutSec == BOOT_CFG_TIMEOUT_FOREVER) {
        return BOOT_MENU_NONE;
    }
    if (state->remainingSec == 0) {
        /* Already expired (shouldn't normally be ticked again once BOOT was returned, but stay
         * idempotent rather than underflowing remainingSec to UINT32_MAX). */
        return BOOT_MENU_NONE;
    }
    state->remainingSec--;
    if (state->remainingSec == 0) {
        state->countdownActive = false;
        return BOOT_MENU_BOOT;
    }
    return BOOT_MENU_REDRAW_COUNTDOWN;
}
