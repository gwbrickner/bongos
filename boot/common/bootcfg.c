/* See bootcfg.h. No libc string functions (D-065): freestanding, hand-rolled line/trim/compare
 * helpers over the raw buffer. */
#include "include/bootcfg.h"

#include "include/bootmem.h"

static int cfgIsSpace(char c) {
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

static int cfgStreqBounded(const char *start, const char *end, const char *literal) {
    uint64_t len = (uint64_t)(end - start);
    uint64_t i = 0;
    for (; i < len; i++) {
        if (literal[i] == '\0' || start[i] != literal[i]) {
            return 0;
        }
    }
    return literal[i] == '\0';
}

/* Copies [start, end) into `dst` (capacity `dstCap`, including the NUL), truncating rather than
 * overflowing. Returns the number of bytes actually copied (excluding the NUL). */
static uint64_t cfgCopyBounded(const char *start, const char *end, char *dst, uint64_t dstCap) {
    uint64_t len = (uint64_t)(end - start);
    if (len > dstCap - 1) {
        len = dstCap - 1;
    }
    bootMemcpy(dst, start, len);
    dst[len] = '\0';
    return len;
}

static BootStatus cfgHandleKernelLine(const char *valueStart, const char *valueEnd, BootCfg *out) {
    uint64_t len = (uint64_t)(valueEnd - valueStart);
    if (len == 0 || valueStart[0] != '/' || len > (uint64_t)(BOOT_CFG_KERNEL_PATH_MAX - 1)) {
        return BOOT_ERR_CFG;
    }
    for (uint64_t i = 0; i < len; i++) {
        if ((unsigned char)valueStart[i] > 0x7F) {
            return BOOT_ERR_CFG;
        }
    }
    cfgCopyBounded(valueStart, valueEnd, out->kernel, sizeof(out->kernel));
    out->hasKernel = true;
    return BOOT_OK;
}

BootStatus bootCfgParse(const char *text, uint64_t textLen, BootCfg *out) {
    if (text == NULL || out == NULL) {
        return BOOT_ERR_CFG;
    }
    out->kernel[0] = '\0';
    out->cmdline[0] = '\0';
    out->hasKernel = false;
    out->hasCmdline = false;

    const char *p = text;
    const char *fileEnd = text + textLen;
    while (p < fileEnd) {
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

        if (start == end || *start == '#' || *start == '[') {
            continue;
        }

        const char *eq = start;
        while (eq < end && *eq != '=') {
            eq++;
        }
        if (eq == end) {
            continue; /* no '=': not a recognized key line, ignore (forward-compatible) */
        }

        const char *keyStart = start;
        const char *keyEnd = eq;
        cfgTrim(&keyStart, &keyEnd);
        const char *valueStart = eq + 1;
        const char *valueEnd = end;
        cfgTrim(&valueStart, &valueEnd);

        if (!out->hasKernel && cfgStreqBounded(keyStart, keyEnd, "kernel")) {
            BootStatus st = cfgHandleKernelLine(valueStart, valueEnd, out);
            if (st != BOOT_OK) {
                return st;
            }
        } else if (!out->hasCmdline && cfgStreqBounded(keyStart, keyEnd, "cmdline")) {
            cfgCopyBounded(valueStart, valueEnd, out->cmdline, sizeof(out->cmdline));
            out->hasCmdline = true;
        }
        /* Every other key, and a repeated kernel/cmdline, is ignored (first wins; M1.4 adds
         * resolution/kaslr/timeout/default/[entry] handling). */
    }
    return BOOT_OK;
}
