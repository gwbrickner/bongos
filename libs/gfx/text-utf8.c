/* UTF-8 decoding with maximal-subpart U+FFFD substitution (M12.3, D-156; spec
 * docs/specs/gfx-text.md). Every read is bounded by `len`. */
#include "gfx/gfx-text.h"

#define REPLACEMENT 0xFFFDu

/* Contract: pure; len == 0 returns GFX_UTF8_END with *consumed = 0. */
uint32_t gfxUtf8Decode(const uint8_t *s, size_t len, size_t *consumed) {
    if (len == 0) {
        *consumed = 0;
        return GFX_UTF8_END;
    }
    const uint32_t b0 = s[0];
    if (b0 < 0x80) {
        *consumed = 1;
        return b0;
    }
    /* trailing bytes still needed, the allowed range of the first one, and the lead's payload */
    uint32_t need;
    uint32_t lo = 0x80, hi = 0xBF;
    uint32_t cp;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        need = 1;
        cp = b0 & 0x1F;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2;
        cp = b0 & 0x0F;
        if (b0 == 0xE0) {
            lo = 0xA0;
        } else if (b0 == 0xED) {
            hi = 0x9F;
        }
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3;
        cp = b0 & 0x07;
        if (b0 == 0xF0) {
            lo = 0x90;
        } else if (b0 == 0xF4) {
            hi = 0x8F;
        }
    } else {
        *consumed = 1;
        return REPLACEMENT;
    }
    size_t used = 1;
    for (uint32_t k = 0; k < need; k++) {
        if (used >= len || s[used] < lo || s[used] > hi) {
            *consumed = used;
            return REPLACEMENT;
        }
        cp = (cp << 6) | (s[used] & 0x3Fu);
        used++;
        lo = 0x80;
        hi = 0xBF;
    }
    *consumed = used;
    return cp;
}

/* Contract: pure. */
size_t gfxUtf8Encode(uint32_t cp, uint8_t out[4]) {
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        cp = REPLACEMENT;
    }
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (uint8_t)(0xC0 | (cp >> 6));
        out[1] = (uint8_t)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (uint8_t)(0xE0 | (cp >> 12));
        out[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (uint8_t)(0xF0 | (cp >> 18));
    out[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (uint8_t)(0x80 | (cp & 0x3F));
    return 4;
}

/* Contract: pure; NULL `nReplaced` is allowed. A genuine U+FFFD in the input is not counted. */
size_t gfxUtf8Count(const uint8_t *s, size_t len, size_t *nReplaced) {
    size_t n = 0, bad = 0, pos = 0;
    while (pos < len) {
        size_t used;
        const uint32_t cp = gfxUtf8Decode(s + pos, len - pos, &used);
        if (cp == REPLACEMENT &&
            !(used == 3 && s[pos] == 0xEF && s[pos + 1] == 0xBF && s[pos + 2] == 0xBD)) {
            bad++;
        }
        pos += used;
        n++;
    }
    if (nReplaced != NULL) {
        *nReplaced = bad;
    }
    return n;
}

void gfxUtf8IterInit(GfxUtf8Iter *it, const uint8_t *s, size_t len) {
    it->s = s;
    it->len = len;
    it->pos = 0;
}

/* Contract: pure apart from *it; false at the end (cp and offset untouched). */
bool gfxUtf8Next(GfxUtf8Iter *it, uint32_t *cp, size_t *offset) {
    if (it->pos >= it->len) {
        return false;
    }
    size_t used;
    const uint32_t c = gfxUtf8Decode(it->s + it->pos, it->len - it->pos, &used);
    *cp = c;
    *offset = it->pos;
    it->pos += used;
    return true;
}
