/* boot.cfg parser (ARCHITECTURE §5.2), M1.3's minimal subset: `kernel = /path` and
 * `cmdline = ...` only. Forward-compatible with M1.4's fuller parser (`[entry]` sections,
 * `resolution`, `kaslr`, `timeout`, `default`): those lines are recognized and silently ignored
 * here rather than rejected, so a boot.cfg written for M1.4 still parses today. */
#ifndef BOOT_COMMON_BOOTCFG_H
#define BOOT_COMMON_BOOTCFG_H

#include <stdbool.h>
#include <stdint.h>

#include "boot-status.h"
#include "bootinfo.h"

#define BOOT_CFG_KERNEL_PATH_MAX 256 /* 255 chars + NUL, ARCHITECTURE §5.2 */

typedef struct {
    char kernel[BOOT_CFG_KERNEL_PATH_MAX];
    char cmdline[BOOTINFO_CMDLINE_MAX];
    bool hasKernel;
    bool hasCmdline;
} BootCfg;

/* Parses `text` (`textLen` bytes, not necessarily NUL-terminated -- a raw file read). Fills
 * `out->kernel`/`out->cmdline` from the first `kernel =`/`cmdline =` lines seen (later ones of
 * the same key are ignored); every other key, `[...]` line, comment, and blank line is skipped.
 * `out->hasKernel`/`hasCmdline` distinguish "not present" (the caller applies ARCHITECTURE §5.5's
 * default, kernel=/bong/kernel.elf, empty cmdline) from an explicit empty value. Fails with
 * BOOT_ERR_CFG only if a `kernel =` line's value doesn't start with '/', exceeds 255 characters,
 * or isn't 7-bit ASCII (ARCHITECTURE §5.2); a `cmdline =` value longer than the buffer is
 * truncated (the caller logs that, matching the whole-BootInfo cmdline truncation warning in
 * ARCHITECTURE §5.5 step 11), not rejected. No locks, boot-time or host-test only; pure. */
BootStatus bootCfgParse(const char *text, uint64_t textLen, BootCfg *out);

#endif
