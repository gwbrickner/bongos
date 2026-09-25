/* boot.cfg parser (ARCHITECTURE §5.2, D-067). Grammar: a line-oriented `key = value` file (at
 * most BOOT_CFG_FILE_MAX bytes, UTF-8, an optional leading BOM, LF or CRLF line endings, `#`
 * whole-line comments, blank lines skipped) with up to BOOT_CFG_MAX_ENTRIES `[Name]` sections
 * that make up the boot menu. Keys before the first section form the *global* scope; each
 * section is its own scope. `kernel`/`initrd`/`cmdline`/`resolution`/`kaslr` are valid in either
 * scope, with entry-then-global-then-built-in inheritance; `timeout`/`default` are global-only.
 * A file with no `[Name]` sections parses as one implicit entry that takes every value from the
 * global scope, so an M1.3-style `kernel =`/`cmdline =` file still boots unchanged.
 *
 * Two-phase API, D-067: `bootCfgParse` builds a `BootCfg` of zero-copy spans into the caller's
 * text buffer (which must stay alive and unmodified until resolution is done) plus the resolved
 * `default =` index; `bootCfgResolveEntry` then copies one entry's effective (inherited) values
 * into a `BootCfgEntry`. Keeping `BootCfg` span-based rather than copying every entry's cmdline
 * up front is what keeps it small enough for M2.5's sub-1 MiB BIOS stage2. No libc string
 * functions (D-065): freestanding, hand-rolled line/trim/compare helpers over the raw buffer. */
#ifndef BOOT_COMMON_BOOTCFG_H
#define BOOT_COMMON_BOOTCFG_H

#include <stdbool.h>
#include <stdint.h>

#include "boot-status.h"
#include "bootinfo.h"

#define BOOT_CFG_FILE_MAX        65536u /* bytes, D-067 */
#define BOOT_CFG_MAX_ENTRIES     9u     /* so serial digit keys 1-9 can select directly */
#define BOOT_CFG_NAME_MAX        63u    /* entry display name, excluding the NUL */
#define BOOT_CFG_PATH_MAX        256u   /* kernel/initrd path, including the NUL */
#define BOOT_CFG_KERNEL_PATH_MAX BOOT_CFG_PATH_MAX /* pre-M1.4 name, kept as an alias */
#define BOOT_CFG_TIMEOUT_MAX     3600u
#define BOOT_CFG_TIMEOUT_FOREVER 0xFFFFFFFFu
#define BOOT_CFG_DIM_MAX         16384u

#define BOOT_CFG_HAS_KERNEL     (1u << 0)
#define BOOT_CFG_HAS_INITRD     (1u << 1)
#define BOOT_CFG_HAS_CMDLINE    (1u << 2)
#define BOOT_CFG_HAS_RESOLUTION (1u << 3)
#define BOOT_CFG_HAS_KASLR      (1u << 4)

/* A [off, off+len) span into the text buffer bootCfgParse() was given; never resolved into a
 * pointer until bootCfgResolveEntry() copies it out. */
typedef struct {
    uint32_t off, len;
} BootCfgSpan;

typedef struct {
    uint32_t setMask; /* BOOT_CFG_HAS_* bits: which keys this scope explicitly set */
    BootCfgSpan kernel, initrd, cmdline;
    uint32_t resWidth, resHeight; /* 0,0 = auto */
    uint8_t kaslr;                /* 1 = on, 0 = off; meaningful only if setMask has the bit */
} BootCfgScope;

typedef struct {
    BootCfgSpan name; /* len 0 only for the implicit (zero-section) entry */
    uint32_t line;    /* 1-based `[Name]` line; 0 for the implicit entry */
    BootCfgScope keys;
} BootCfgEntryDef;

typedef struct {
    BootCfgScope global;
    BootCfgEntryDef entries[BOOT_CFG_MAX_ENTRIES];
    uint32_t entryCount;   /* >= 1 whenever bootCfgParse() returns BOOT_OK */
    uint32_t timeoutSec;   /* 0..BOOT_CFG_TIMEOUT_MAX, or BOOT_CFG_TIMEOUT_FOREVER */
    uint32_t defaultIndex; /* 0-based index into entries[], already resolved */
    uint32_t unknownKeyCount, firstUnknownKeyLine; /* 0 if none seen */
    uint32_t errorLine;                            /* set on error; 0 means a whole-file error */
    const char *errorReason; /* static string, non-NULL exactly when the return isn't BOOT_OK */
} BootCfg;

typedef struct {
    char name[BOOT_CFG_NAME_MAX + 1]; /* "" for the implicit entry */
    char kernel[BOOT_CFG_PATH_MAX];
    char initrd[BOOT_CFG_PATH_MAX]; /* "" = none */
    char cmdline[BOOTINFO_CMDLINE_MAX];
    uint32_t resWidth, resHeight; /* 0,0 = auto */
    bool kaslr;
    bool cmdlineTruncated; /* the effective cmdline was longer than BOOTINFO_CMDLINE_MAX and got cut
                            */
} BootCfgEntry;

/* Parses `text` (`textLen` bytes, not necessarily NUL-terminated -- a raw file read) into `out`.
 * On BOOT_OK, `out->entryCount` is at least 1 and `out->defaultIndex` is a valid index into
 * `out->entries`. On BOOT_ERR_CFG, `out->errorLine` (0 for a whole-file problem, e.g. the file
 * exceeds BOOT_CFG_FILE_MAX) and `out->errorReason` describe why; every other field is
 * unspecified. `text` must stay alive and unmodified until every bootCfgResolveEntry() call
 * naming this `out` is done, since `out` stores spans into it, not copies. No locks, boot-time or
 * host-test only; pure. */
BootStatus bootCfgParse(const char *text, uint64_t textLen, BootCfg *out);

/* Resolves entry `index` (< cfg->entryCount) of a BOOT_OK `cfg` -- the same `text`/`textLen`
 * bootCfgParse() was called with -- into `out`, applying entry-then-global-then-built-in
 * inheritance per key. Fails only on a bad argument (NULL, or index >= cfg->entryCount). No
 * locks, boot-time or host-test only; pure. */
BootStatus bootCfgResolveEntry(const char *text, uint64_t textLen, const BootCfg *cfg,
                               uint32_t index, BootCfgEntry *out);

#endif
