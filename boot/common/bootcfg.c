/* See bootcfg.h. No libc string functions (D-065): freestanding, hand-rolled line/trim/compare
 * helpers over the raw buffer. All arithmetic stays in uint32_t (D-065: no `long`, no 64-bit
 * `/`/`%`) -- safe because every offset here is bounded by BOOT_CFG_FILE_MAX, checked up front. */
#include "include/bootcfg.h"

#include "include/bootmem.h"

#define CFG_ERR(out, ln, reason)                                                                   \
    do {                                                                                           \
        (out)->errorLine = (ln);                                                                   \
        (out)->errorReason = (reason);                                                             \
        return BOOT_ERR_CFG;                                                                       \
    } while (0)

static bool cfgIsSpace(char c) {
    return c == ' ' || c == '\t';
}

/* Trims leading/trailing spaces and tabs from [*start, *end) in place (by moving the pointers,
 * not the bytes). */
static void cfgTrim(const char **start, const char **end) {
    while (*start < *end && cfgIsSpace(**start)) {
        (*start)++;
    }
    while (*end > *start && cfgIsSpace(*(*end - 1))) {
        (*end)--;
    }
}

static bool cfgStreqBounded(const char *start, const char *end, const char *literal) {
    uint64_t len = (uint64_t)(end - start);
    uint64_t i = 0;
    for (; i < len; i++) {
        if (literal[i] == '\0' || start[i] != literal[i]) {
            return false;
        }
    }
    return literal[i] == '\0';
}

static bool cfgMemEq(const char *a, const char *b, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

/* `[a-z0-9_.]`, ARCHITECTURE §5.2/D-067. */
static bool cfgIsKeyByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

/* Printable ASCII, minus the bracket characters (which would make a section header ambiguous). */
static bool cfgIsNameByte(char c) {
    unsigned char u = (unsigned char)c;
    return u >= 0x20 && u <= 0x7E && c != '[' && c != ']';
}

/* `kernel =`/`initrd =` value rule: '/'-prefixed, 1..255 bytes, 0x21..0x7E only (no spaces or
 * control characters -- tighter than M1.3's "not > 0x7F" check). */
static bool cfgValidPathValue(const char *s, uint32_t len) {
    if (len == 0 || len > BOOT_CFG_PATH_MAX - 1 || s[0] != '/') {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

/* Decimal digits only, leading zeros allowed; rejects before any intermediate value could exceed
 * what a later range check cares about (D-067: reject once v > 99999 before the next digit). */
static bool cfgParseUint(const char *s, uint32_t len, uint32_t *out) {
    if (len == 0) {
        return false;
    }
    uint32_t v = 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = s[i];
        if (c < '0' || c > '9') {
            return false;
        }
        if (v > 99999u) {
            return false;
        }
        v = v * 10u + (uint32_t)(c - '0');
    }
    *out = v;
    return true;
}

/* `auto`, or `<W>x<H>`/`<W>X<H>` with W and H each in 1..BOOT_CFG_DIM_MAX. */
static bool cfgParseResolution(const char *s, uint32_t len, uint32_t *w, uint32_t *h) {
    if (cfgStreqBounded(s, s + len, "auto")) {
        *w = 0;
        *h = 0;
        return true;
    }
    uint32_t xi = len;
    for (uint32_t i = 0; i < len; i++) {
        if (s[i] == 'x' || s[i] == 'X') {
            xi = i;
            break;
        }
    }
    if (xi == len || xi == 0 || xi == len - 1) {
        return false;
    }
    uint32_t wv, hv;
    if (!cfgParseUint(s, xi, &wv) || !cfgParseUint(s + xi + 1, len - xi - 1, &hv)) {
        return false;
    }
    if (wv < 1 || wv > BOOT_CFG_DIM_MAX || hv < 1 || hv > BOOT_CFG_DIM_MAX) {
        return false;
    }
    *w = wv;
    *h = hv;
    return true;
}

BootStatus bootCfgParse(const char *text, uint64_t textLen, BootCfg *out) {
    if (out == NULL) {
        return BOOT_ERR_CFG;
    }
    bootMemset(out, 0, sizeof(*out));
    if (text == NULL) {
        CFG_ERR(out, 0, "no boot.cfg text");
    }
    if (textLen > BOOT_CFG_FILE_MAX) {
        CFG_ERR(out, 0, "boot.cfg exceeds BOOT_CFG_FILE_MAX");
    }

    /* A leading UTF-8 BOM (EF BB BF), for a boot.cfg edited on the ESP with a Windows text
     * editor. */
    if (textLen >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text += 3;
        textLen -= 3;
    }

    /* A NUL byte anywhere is rejected up front, with its line number, rather than let it
     * silently truncate a value or a comparison further down. */
    for (uint64_t i = 0; i < textLen; i++) {
        if (text[i] == '\0') {
            uint32_t line = 1;
            for (uint64_t j = 0; j < i; j++) {
                if (text[j] == '\n') {
                    line++;
                }
            }
            CFG_ERR(out, line, "NUL byte in boot.cfg");
        }
    }

    BootCfgScope *curScope = &out->global;
    bool sawTimeout = false, sawDefault = false;
    bool defaultIsIndex = false;
    uint32_t defaultIndexVal = 0;
    BootCfgSpan defaultNameSpan = {0, 0};
    uint32_t defaultLine = 0;

    const char *p = text;
    const char *fileEnd = text + textLen;
    uint32_t lineNo = 0;
    while (p < fileEnd) {
        lineNo++;
        const char *lineStart = p;
        const char *nl = lineStart;
        while (nl < fileEnd && *nl != '\n') {
            nl++;
        }
        const char *lineEnd = nl; /* excludes the '\n' itself */
        p = (nl < fileEnd) ? nl + 1 : fileEnd;
        if (lineEnd > lineStart && lineEnd[-1] == '\r') {
            lineEnd--;
        }

        const char *start = lineStart;
        const char *end = lineEnd;
        cfgTrim(&start, &end);
        if (start == end || *start == '#') {
            continue;
        }

        if (*start == '[') {
            if (end[-1] != ']') {
                CFG_ERR(out, lineNo, "unterminated section header");
            }
            const char *innerStart = start + 1;
            const char *innerEnd = end - 1;
            cfgTrim(&innerStart, &innerEnd);
            uint32_t innerLen = (uint32_t)(innerEnd - innerStart);
            if (innerLen < 1 || innerLen > BOOT_CFG_NAME_MAX) {
                CFG_ERR(out, lineNo, "section name must be 1-63 characters");
            }
            for (uint32_t i = 0; i < innerLen; i++) {
                if (!cfgIsNameByte(innerStart[i])) {
                    CFG_ERR(out, lineNo, "invalid character in section name");
                }
            }
            for (uint32_t i = 0; i < out->entryCount; i++) {
                const BootCfgSpan *nm = &out->entries[i].name;
                if (nm->len == innerLen && cfgMemEq(text + nm->off, innerStart, innerLen)) {
                    CFG_ERR(out, lineNo, "duplicate entry name");
                }
            }
            if (out->entryCount >= BOOT_CFG_MAX_ENTRIES) {
                CFG_ERR(out, lineNo, "too many [entry] sections (max 9)");
            }
            BootCfgEntryDef *e = &out->entries[out->entryCount];
            e->name.off = (uint32_t)(innerStart - text);
            e->name.len = innerLen;
            e->line = lineNo;
            out->entryCount++;
            curScope = &e->keys;
            continue;
        }

        const char *eq = start;
        while (eq < end && *eq != '=') {
            eq++;
        }
        if (eq == end) {
            CFG_ERR(out, lineNo, "expected key = value");
        }
        const char *keyStart = start;
        const char *keyEnd = eq;
        cfgTrim(&keyStart, &keyEnd);
        const char *valueStart = eq + 1;
        const char *valueEnd = end;
        cfgTrim(&valueStart, &valueEnd);
        uint32_t keyLen = (uint32_t)(keyEnd - keyStart);
        if (keyLen < 1 || keyLen > 31) {
            CFG_ERR(out, lineNo, "malformed key");
        }
        for (uint32_t i = 0; i < keyLen; i++) {
            if (!cfgIsKeyByte(keyStart[i])) {
                CFG_ERR(out, lineNo, "malformed key");
            }
        }
        uint32_t valLen = (uint32_t)(valueEnd - valueStart);
        uint32_t valOff = (uint32_t)(valueStart - text);

        if (cfgStreqBounded(keyStart, keyEnd, "kernel")) {
            if (!(curScope->setMask & BOOT_CFG_HAS_KERNEL)) {
                if (!cfgValidPathValue(valueStart, valLen)) {
                    CFG_ERR(out, lineNo, "invalid kernel path");
                }
                curScope->kernel.off = valOff;
                curScope->kernel.len = valLen;
                curScope->setMask |= BOOT_CFG_HAS_KERNEL;
            }
        } else if (cfgStreqBounded(keyStart, keyEnd, "initrd")) {
            if (!(curScope->setMask & BOOT_CFG_HAS_INITRD)) {
                if (valLen > 0 && !cfgValidPathValue(valueStart, valLen)) {
                    CFG_ERR(out, lineNo, "invalid initrd path");
                }
                curScope->initrd.off = valOff;
                curScope->initrd.len = valLen;
                curScope->setMask |= BOOT_CFG_HAS_INITRD;
            }
        } else if (cfgStreqBounded(keyStart, keyEnd, "cmdline")) {
            if (!(curScope->setMask & BOOT_CFG_HAS_CMDLINE)) {
                curScope->cmdline.off = valOff;
                curScope->cmdline.len = valLen;
                curScope->setMask |= BOOT_CFG_HAS_CMDLINE;
            }
        } else if (cfgStreqBounded(keyStart, keyEnd, "resolution")) {
            if (!(curScope->setMask & BOOT_CFG_HAS_RESOLUTION)) {
                uint32_t w, h;
                if (!cfgParseResolution(valueStart, valLen, &w, &h)) {
                    CFG_ERR(out, lineNo, "invalid resolution");
                }
                curScope->resWidth = w;
                curScope->resHeight = h;
                curScope->setMask |= BOOT_CFG_HAS_RESOLUTION;
            }
        } else if (cfgStreqBounded(keyStart, keyEnd, "kaslr")) {
            if (!(curScope->setMask & BOOT_CFG_HAS_KASLR)) {
                bool on;
                if (cfgStreqBounded(valueStart, valueEnd, "on")) {
                    on = true;
                } else if (cfgStreqBounded(valueStart, valueEnd, "off")) {
                    on = false;
                } else {
                    CFG_ERR(out, lineNo, "invalid kaslr value (want on/off)");
                }
                curScope->kaslr = on ? 1 : 0;
                curScope->setMask |= BOOT_CFG_HAS_KASLR;
            }
        } else if (cfgStreqBounded(keyStart, keyEnd, "timeout")) {
            if (curScope != &out->global) {
                CFG_ERR(out, lineNo, "timeout is only valid before the first [entry]");
            }
            if (!sawTimeout) {
                uint32_t t;
                if (cfgStreqBounded(valueStart, valueEnd, "forever")) {
                    t = BOOT_CFG_TIMEOUT_FOREVER;
                } else if (!cfgParseUint(valueStart, valLen, &t) || t > BOOT_CFG_TIMEOUT_MAX) {
                    CFG_ERR(out, lineNo, "invalid timeout (want 0-3600 or forever)");
                }
                out->timeoutSec = t;
                sawTimeout = true;
            }
        } else if (cfgStreqBounded(keyStart, keyEnd, "default")) {
            if (curScope != &out->global) {
                CFG_ERR(out, lineNo, "default is only valid before the first [entry]");
            }
            if (!sawDefault) {
                bool allDigits = valLen > 0;
                for (uint32_t i = 0; i < valLen && allDigits; i++) {
                    if (valueStart[i] < '0' || valueStart[i] > '9') {
                        allDigits = false;
                    }
                }
                if (allDigits) {
                    uint32_t idx;
                    if (!cfgParseUint(valueStart, valLen, &idx) || idx < 1) {
                        CFG_ERR(out, lineNo, "invalid default index");
                    }
                    defaultIsIndex = true;
                    defaultIndexVal = idx;
                } else {
                    defaultIsIndex = false;
                    defaultNameSpan.off = valOff;
                    defaultNameSpan.len = valLen;
                }
                defaultLine = lineNo;
                sawDefault = true;
            }
        } else {
            out->unknownKeyCount++;
            if (out->firstUnknownKeyLine == 0) {
                out->firstUnknownKeyLine = lineNo;
            }
        }
    }

    if (out->entryCount == 0) {
        /* No [Name] sections: one implicit entry taking every value from the global scope, so an
         * M1.3-style kernel=/cmdline=-only file still boots unchanged. entries[0] is already
         * zeroed by the bootMemset() above. */
        out->entryCount = 1;
    }

    if (!sawDefault) {
        out->defaultIndex = 0;
    } else if (defaultIsIndex) {
        if (defaultIndexVal < 1 || defaultIndexVal > out->entryCount) {
            CFG_ERR(out, defaultLine, "default index out of range");
        }
        out->defaultIndex = defaultIndexVal - 1;
    } else {
        bool found = false;
        for (uint32_t i = 0; i < out->entryCount; i++) {
            const BootCfgSpan *nm = &out->entries[i].name;
            if (nm->len == defaultNameSpan.len &&
                cfgMemEq(text + nm->off, text + defaultNameSpan.off, defaultNameSpan.len)) {
                out->defaultIndex = i;
                found = true;
                break;
            }
        }
        if (!found) {
            CFG_ERR(out, defaultLine, "default entry name not found");
        }
    }

    return BOOT_OK;
}

/* Copies span [span->off, span->off+span->len) into `dst` (capacity `dstCap`, including the
 * NUL), truncating rather than overflowing. Returns the number of bytes actually copied
 * (excluding the NUL). */
static uint32_t cfgCopySpan(const char *text, const BootCfgSpan *span, char *dst, uint32_t dstCap) {
    uint32_t len = span->len;
    if (len > dstCap - 1) {
        len = dstCap - 1;
    }
    bootMemcpy(dst, text + span->off, len);
    dst[len] = '\0';
    return len;
}

BootStatus bootCfgResolveEntry(const char *text, uint64_t textLen, const BootCfg *cfg,
                               uint32_t index, BootCfgEntry *out) {
    if (text == NULL || cfg == NULL || out == NULL || index >= cfg->entryCount) {
        return BOOT_ERR_CFG;
    }
    /* Every span in `cfg` is an offset from bootCfgParse()'s BOM-skipped view of the text, not
     * from `text` itself -- replicate that same skip here so the two functions agree on what
     * offset 0 means, rather than requiring the caller to remember and redo it. */
    if (textLen >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text += 3;
        textLen -= 3;
    }
    (void)textLen;
    bootMemset(out, 0, sizeof(*out));
    const BootCfgEntryDef *e = &cfg->entries[index];

    if (e->name.len > 0) {
        cfgCopySpan(text, &e->name, out->name, sizeof(out->name));
    }

    if (e->keys.setMask & BOOT_CFG_HAS_KERNEL) {
        cfgCopySpan(text, &e->keys.kernel, out->kernel, sizeof(out->kernel));
    } else if (cfg->global.setMask & BOOT_CFG_HAS_KERNEL) {
        cfgCopySpan(text, &cfg->global.kernel, out->kernel, sizeof(out->kernel));
    } else {
        static const char defaultKernel[] = "/bong/kernel.elf";
        bootMemcpy(out->kernel, defaultKernel, sizeof(defaultKernel));
    }

    if (e->keys.setMask & BOOT_CFG_HAS_INITRD) {
        cfgCopySpan(text, &e->keys.initrd, out->initrd, sizeof(out->initrd));
    } else if (cfg->global.setMask & BOOT_CFG_HAS_INITRD) {
        cfgCopySpan(text, &cfg->global.initrd, out->initrd, sizeof(out->initrd));
    } /* else "" (none), already zeroed */

    const BootCfgSpan *cmdlineSpan = NULL;
    if (e->keys.setMask & BOOT_CFG_HAS_CMDLINE) {
        cmdlineSpan = &e->keys.cmdline;
    } else if (cfg->global.setMask & BOOT_CFG_HAS_CMDLINE) {
        cmdlineSpan = &cfg->global.cmdline;
    }
    if (cmdlineSpan != NULL) {
        uint32_t copied = cfgCopySpan(text, cmdlineSpan, out->cmdline, sizeof(out->cmdline));
        out->cmdlineTruncated = copied < cmdlineSpan->len;
    }

    if (e->keys.setMask & BOOT_CFG_HAS_RESOLUTION) {
        out->resWidth = e->keys.resWidth;
        out->resHeight = e->keys.resHeight;
    } else if (cfg->global.setMask & BOOT_CFG_HAS_RESOLUTION) {
        out->resWidth = cfg->global.resWidth;
        out->resHeight = cfg->global.resHeight;
    } /* else 0,0 = auto, already zeroed */

    if (e->keys.setMask & BOOT_CFG_HAS_KASLR) {
        out->kaslr = e->keys.kaslr != 0;
    } else if (cfg->global.setMask & BOOT_CFG_HAS_KASLR) {
        out->kaslr = cfg->global.kaslr != 0;
    } else {
        out->kaslr = true; /* built-in default: on */
    }

    return BOOT_OK;
}
