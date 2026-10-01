/* Differential host test for libs/gfx's GIF decoder (M12.7 bug-sweeper, D-163/D-164): random
 * multi-frame files (every LZW minimum code size 2..11, KwKwK codes, clears, early and missing
 * end codes, codes past a full table, frames partly or wholly off the canvas at coordinates near
 * 65535, interlace, transparency, every disposal 0..7) are played through the streaming API and
 * compared, frame by frame, with a second, deliberately different model written here: its LZW
 * decoder reads the raw code bytes (before sub-block framing) and keeps every table string
 * explicitly; its compositor works in 64-bit coordinates, lists interlaced rows pass by pass, and
 * keeps disposal 3 as a copy of the whole canvas. Rewind is checked by replaying. */
#include "framework/test.h"
#include "gfx/gfx-image.h"
#include "gfx/gfx.h"
#include "gfx_decode_testutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *b;
    size_t n, cap;
} DBuf;

static void dPut(DBuf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->b = realloc(b->b, b->cap);
    }
    memcpy(b->b + b->n, p, n);
    b->n += n;
}

static void dByte(DBuf *b, uint32_t v) {
    uint8_t c = (uint8_t)v;
    dPut(b, &c, 1);
}

static void dLe16(DBuf *b, uint32_t v) {
    dByte(b, v & 0xFF);
    dByte(b, v >> 8);
}

/* ---- the model ------------------------------------------------------------------------- */

typedef struct {
    int64_t x, y, w, h;
    bool interlace;
    uint32_t pal[256];
    uint32_t palCount;
    bool hasTransp;
    uint32_t transIdx;
    uint32_t disposal;
    uint32_t delay;
    uint32_t minCode;
    uint8_t *raw; /* LZW code bytes, before sub-block framing */
    size_t rawLen;
} MFrame;

/* Decodes `f`'s raw codes into at most w*h indices; returns false on an LZW error. */
static bool modelLzw(const MFrame *f, uint16_t *out, size_t *nOut) {
    size_t need = (size_t)(f->w * f->h), n = 0;
    uint32_t m = f->minCode, clear = 1u << m, eoi = clear + 1;
    uint32_t width = m + 1, next = clear + 2;
    static uint16_t *str[4096];
    static uint32_t len[4096];
    int64_t prev = -1;
    size_t bitPos = 0, totalBits = f->rawLen * 8;
    bool ok = true;
    for (uint32_t i = 0; i < 4096; i++) {
        str[i] = NULL;
        len[i] = 0;
    }
    while (n < need) {
        if (bitPos + width > totalBits) {
            break;
        }
        uint32_t code = 0;
        for (uint32_t k = 0; k < width; k++, bitPos++) {
            code |= (uint32_t)((f->raw[bitPos / 8] >> (bitPos % 8)) & 1u) << k;
        }
        if (code == clear) {
            for (uint32_t i = clear + 2; i < 4096; i++) {
                free(str[i]);
                str[i] = NULL;
            }
            width = m + 1;
            next = clear + 2;
            prev = -1;
            continue;
        }
        if (code == eoi) {
            break;
        }
        uint16_t tmp[4097];
        uint32_t tl;
        if (code < clear) {
            tmp[0] = (uint16_t)code;
            tl = 1;
        } else if (prev >= 0 && code >= clear + 2 && code < next) {
            memcpy(tmp, str[code], len[code] * 2);
            tl = len[code];
        } else if (prev >= 0 && code == next) {
            uint32_t pl = prev < (int64_t)clear ? 1 : len[prev];
            if (prev < (int64_t)clear) {
                tmp[0] = (uint16_t)prev;
            } else {
                memcpy(tmp, str[prev], pl * 2);
            }
            tmp[pl] = tmp[0];
            tl = pl + 1;
        } else {
            ok = false;
            break;
        }
        for (uint32_t i = 0; i < tl && n < need; i++) {
            out[n++] = tmp[i];
        }
        if (prev >= 0 && next < 4096) {
            uint32_t pl = prev < (int64_t)clear ? 1 : len[prev];
            uint16_t *s = malloc((pl + 1) * 2);
            if (prev < (int64_t)clear) {
                s[0] = (uint16_t)prev;
            } else {
                memcpy(s, str[prev], pl * 2);
            }
            s[pl] = tmp[0];
            str[next] = s;
            len[next] = pl + 1;
            next++;
            if (next == (1u << width) && width < 12) {
                width++;
            }
        }
        prev = code;
    }
    for (uint32_t i = 0; i < 4096; i++) {
        free(str[i]);
        str[i] = NULL;
    }
    *nOut = n;
    return ok;
}

/* Row order of a frame of height h. */
static void modelRows(const MFrame *f, int64_t *rows) {
    size_t k = 0;
    if (!f->interlace) {
        for (int64_t r = 0; r < f->h; r++) {
            rows[k++] = r;
        }
        return;
    }
    static const int64_t start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
    for (int p = 0; p < 4; p++) {
        for (int64_t r = start[p]; r < f->h; r += step[p]) {
            rows[k++] = r;
        }
    }
}

/* ---- the random file ------------------------------------------------------------------- */

typedef struct {
    uint32_t w, h;
    uint32_t gct[256];
    uint32_t gctCount;
    uint32_t nFrames;
    MFrame fr[6];
    int32_t loop;
} MFile;

static uint32_t rnd(uint32_t *s, uint32_t n) {
    return decRng(s) % n;
}

/* Random codes for frame `f`: mostly valid, sometimes an error, sometimes no end code. */
static void genCodes(uint32_t *s, MFrame *f) {
    uint32_t m = f->minCode, clear = 1u << m, eoi = clear + 1;
    uint32_t width = m + 1, next = clear + 2;
    bool havePrev = false;
    /* "big" frames use few clears and mostly literals, so the table fills (deferred clear) */
    bool big = f->w * f->h >= 5000;
    uint32_t nCodes = 1 + rnd(s, big ? 7000 : rnd(s, 8) == 0 ? 6000 : 400);
    uint32_t clearPer1000 = big ? 0 : 5, kwkwkPer1000 = big ? 10 : 250;
    uint64_t acc = 0;
    uint32_t accBits = 0;
    DBuf out = {0};
    uint32_t litMax = clear;
    if (rnd(s, 3) == 0) {
        litMax = 1 + rnd(s, clear); /* small alphabets make long strings */
    }
    if (rnd(s, 2) == 0) {
        acc = clear;
        accBits = width; /* a leading clear */
    }
    for (uint32_t i = 0; i < nCodes; i++) {
        uint32_t code;
        uint32_t r = rnd(s, 1000);
        bool adds = false;
        if (!havePrev) {
            code = r < 3 ? clear + 2 + rnd(s, 3) : rnd(s, litMax); /* 0.3%: an invalid first */
            adds = false;
            havePrev = true;
        } else if (r < clearPer1000) {
            code = clear;
        } else if (r < 7 && !big) {
            code = eoi;
        } else if (r < 9 && !big) {
            code = next + 1 + rnd(s, 3); /* too big: INVALID unless the frame is full */
            if (code >= (1u << width)) {
                code = (1u << width) - 1;
            }
            adds = true;
        } else if (r < 9 + kwkwkPer1000 + (next >= 4096 ? 100u : 0u)) {
            /* KwKwK; once the table is full, the last entries (4095 is the easiest to lose) */
            code = next < 4096 ? next : 4095 - rnd(s, 3);
            adds = true;
        } else if (r < (big ? 900u : 500u)) {
            code = rnd(s, litMax);
            adds = true;
        } else {
            code = next > clear + 2 ? clear + 2 + rnd(s, next - clear - 2) : rnd(s, litMax);
            adds = true;
        }
        if (code == clear) {
            havePrev = false;
        }
        acc |= (uint64_t)code << accBits;
        accBits += width;
        while (accBits >= 8) {
            dByte(&out, (uint32_t)(acc & 0xFF));
            acc >>= 8;
            accBits -= 8;
        }
        if (code == clear) {
            width = m + 1;
            next = clear + 2;
            continue;
        }
        if (adds && next < 4096) {
            next++;
            if (next == (1u << width) && width < 12) {
                width++;
            }
        }
    }
    if (rnd(s, 4) != 0) {
        acc |= (uint64_t)eoi << accBits;
        accBits += width;
    }
    while (accBits > 0) {
        dByte(&out, (uint32_t)(acc & 0xFF));
        acc >>= 8;
        accBits = accBits > 8 ? accBits - 8 : 0;
    }
    if (rnd(s, 10) == 0 && out.n > 0) {
        out.n -= rnd(s, out.n); /* truncated data */
    }
    f->raw = out.b;
    f->rawLen = out.n;
}

static int64_t randCoord(uint32_t *s, uint32_t canvas) {
    uint32_t r = rnd(s, 10);
    if (r == 0) {
        return 65535 - rnd(s, 4);
    }
    if (r == 1) {
        return canvas + rnd(s, 3);
    }
    return rnd(s, canvas + 1);
}

static void genFile(uint32_t *s, MFile *mf, DBuf *b) {
    memset(mf, 0, sizeof(*mf));
    mf->w = 1 + rnd(s, 24);
    mf->h = 1 + rnd(s, 24);
    mf->loop = -1;
    bool hasGct = rnd(s, 4) != 0;
    uint32_t gctBits = rnd(s, 8);
    dPut(b, rnd(s, 2) ? "GIF89a" : "GIF87a", 6);
    dLe16(b, mf->w);
    dLe16(b, mf->h);
    dByte(b, (hasGct ? 0x80u : 0u) | gctBits);
    dByte(b, rnd(s, 256)); /* background: ignored */
    dByte(b, 0);
    if (hasGct) {
        mf->gctCount = 2u << gctBits;
        for (uint32_t i = 0; i < mf->gctCount; i++) {
            uint32_t c = decRng(s) & 0xFFFFFF;
            dByte(b, c >> 16);
            dByte(b, c >> 8);
            dByte(b, c);
            mf->gct[i] = 0xFF000000u | c;
        }
    }
    mf->nFrames = 1 + rnd(s, 6);
    for (uint32_t k = 0; k < mf->nFrames; k++) {
        MFrame *f = &mf->fr[k];
        if (rnd(s, 3) == 0) { /* a comment extension */
            dByte(b, 0x21);
            dByte(b, 0xFE);
            dByte(b, 3);
            dPut(b, "abc", 3);
            dByte(b, 0);
        }
        if (mf->loop < 0 && rnd(s, 4) == 0) {
            uint32_t lc = rnd(s, 65536);
            dByte(b, 0x21);
            dByte(b, 0xFF);
            dByte(b, 11);
            dPut(b, "NETSCAPE2.0", 11);
            dByte(b, 3);
            dByte(b, 1);
            dLe16(b, lc);
            dByte(b, 0);
            mf->loop = (int32_t)lc;
        }
        if (rnd(s, 5) != 0) {
            uint32_t packed = (rnd(s, 8) << 2) | rnd(s, 2);
            f->disposal = (packed >> 2) & 7;
            f->hasTransp = (packed & 1) != 0;
            f->delay = rnd(s, 65536);
            f->transIdx = rnd(s, 256);
            dByte(b, 0x21);
            dByte(b, 0xF9);
            dByte(b, 4);
            dByte(b, packed);
            dLe16(b, f->delay);
            dByte(b, f->transIdx);
            dByte(b, 0);
        }
        f->x = randCoord(s, mf->w);
        f->y = randCoord(s, mf->h);
        f->w = rnd(s, 12) == 0 ? 0 : 1 + rnd(s, mf->w + 4);
        f->h = rnd(s, 12) == 0 ? 0 : 1 + rnd(s, mf->h + 4);
        if (rnd(s, 10) == 0) { /* big enough to fill the LZW table; mostly off the canvas */
            f->w = 100 + rnd(s, 150);
            f->h = 60 + rnd(s, 190);
        }
        f->interlace = rnd(s, 3) == 0;
        bool hasLct = rnd(s, 3) == 0;
        uint32_t lctBits = rnd(s, 8);
        dByte(b, 0x2C);
        dLe16(b, (uint32_t)f->x);
        dLe16(b, (uint32_t)f->y);
        dLe16(b, (uint32_t)f->w);
        dLe16(b, (uint32_t)f->h);
        dByte(b, (hasLct ? 0x80u : 0u) | (f->interlace ? 0x40u : 0u) | lctBits);
        if (hasLct) {
            f->palCount = 2u << lctBits;
            for (uint32_t i = 0; i < f->palCount; i++) {
                uint32_t c = decRng(s) & 0xFFFFFF;
                dByte(b, c >> 16);
                dByte(b, c >> 8);
                dByte(b, c);
                f->pal[i] = 0xFF000000u | c;
            }
        } else {
            f->palCount = mf->gctCount;
            memcpy(f->pal, mf->gct, sizeof(f->pal));
        }
        f->minCode = 2 + rnd(s, 10);
        dByte(b, f->minCode);
        genCodes(s, f);
        size_t p = 0;
        while (p < f->rawLen) {
            size_t chunk = 1 + rnd(s, 255);
            if (chunk > f->rawLen - p) {
                chunk = f->rawLen - p;
            }
            dByte(b, (uint32_t)chunk);
            dPut(b, f->raw + p, chunk);
            p += chunk;
        }
        dByte(b, 0);
    }
    dByte(b, 0x3B);
}

/* Plays the model and the decoder side by side; false (with a message) on any difference. */
static bool playCompare(GfxGif *g, const MFile *mf, uint32_t seed) {
    size_t W = mf->w, H = mf->h;
    uint32_t *canvas = calloc(W * H, 4), *saved = calloc(W * H, 4);
    bool ok = true, dead = false;
    int64_t prevX0 = 0, prevY0 = 0, prevX1 = 0, prevY1 = 0;
    uint32_t prevDisp = 0;
    bool havePrev = false;
    for (uint32_t k = 0; k < mf->nFrames && ok && !dead; k++) {
        const MFrame *f = &mf->fr[k];
        if (havePrev) {
            for (int64_t y = prevY0; y < prevY1; y++) {
                for (int64_t x = prevX0; x < prevX1; x++) {
                    if (prevDisp == 2) {
                        canvas[y * W + x] = 0;
                    } else if (prevDisp == 3) {
                        canvas[y * W + x] = saved[y * W + x];
                    }
                }
            }
        }
        if (f->disposal == 3) {
            memcpy(saved, canvas, W * H * 4);
        }
        int64_t x0 = f->x < (int64_t)W ? f->x : (int64_t)W,
                y0 = f->y < (int64_t)H ? f->y : (int64_t)H;
        int64_t x1 = f->x + f->w < (int64_t)W ? f->x + f->w : (int64_t)W;
        int64_t y1 = f->y + f->h < (int64_t)H ? f->y + f->h : (int64_t)H;
        if (x1 <= x0 || y1 <= y0) {
            x0 = y0 = x1 = y1 = 0;
        }
        bool lzwOk = true;
        if (f->w != 0 && f->h != 0) {
            uint16_t *idx = malloc((size_t)(f->w * f->h) * 2);
            int64_t *rows = malloc((size_t)f->h * sizeof(int64_t));
            size_t n;
            lzwOk = modelLzw(f, idx, &n);
            modelRows(f, rows);
            for (size_t i = 0; i < n; i++) {
                int64_t col = (int64_t)i % f->w, row = rows[(int64_t)i / f->w];
                int64_t cx = f->x + col, cy = f->y + row;
                if (cx >= (int64_t)W || cy >= (int64_t)H) {
                    continue;
                }
                if (f->hasTransp && idx[i] == f->transIdx) {
                    continue;
                }
                canvas[cy * W + cx] = idx[i] < f->palCount ? f->pal[idx[i]] : 0xFF000000u;
            }
            free(idx);
            free(rows);
        }
        GfxGifFrame fr;
        Status st = gfxGifNextFrame(g, &fr);
        if (!lzwOk) {
            if (st != STATUS_ERR_INVALID || fr.canvas.pixels != NULL) {
                fprintf(stderr, "  seed %u frame %u: want INVALID, got %d\n", seed, k, (int)st);
                ok = false;
            }
            GfxGifFrame fr2;
            if (gfxGifNextFrame(g, &fr2) != STATUS_ERR_INVALID) { /* sticky */
                fprintf(stderr, "  seed %u: error not sticky\n", seed);
                ok = false;
            }
            dead = true;
            break;
        }
        if (st != STATUS_OK) {
            fprintf(stderr, "  seed %u frame %u: status %d\n", seed, k, (int)st);
            ok = false;
            break;
        }
        if (fr.index != k || fr.disposal != f->disposal || fr.delayMs != f->delay * 10 ||
            fr.rect.x0 != x0 || fr.rect.y0 != y0 || fr.rect.x1 != x1 || fr.rect.y1 != y1 ||
            fr.canvas.width != (int32_t)W || fr.canvas.height != (int32_t)H) {
            fprintf(stderr,
                    "  seed %u frame %u: fields differ (rect %d,%d,%d,%d want %lld,%lld,"
                    "%lld,%lld)\n",
                    seed, k, fr.rect.x0, fr.rect.y0, fr.rect.x1, fr.rect.y1, (long long)x0,
                    (long long)y0, (long long)x1, (long long)y1);
            ok = false;
            break;
        }
        for (size_t i = 0; i < W * H; i++) {
            if (fr.canvas.pixels[i] != canvas[i]) {
                fprintf(stderr, "  seed %u frame %u: pixel %zu got %08X want %08X\n", seed, k, i,
                        fr.canvas.pixels[i], canvas[i]);
                ok = false;
                break;
            }
        }
        havePrev = true;
        prevDisp = f->disposal;
        prevX0 = x0;
        prevY0 = y0;
        prevX1 = x1;
        prevY1 = y1;
    }
    if (ok && !dead) {
        GfxGifFrame fr;
        if (gfxGifNextFrame(g, &fr) != STATUS_ERR_NOT_FOUND || fr.canvas.pixels != NULL) {
            fprintf(stderr, "  seed %u: no NOT_FOUND after the last frame\n", seed);
            ok = false;
        }
    }
    free(canvas);
    free(saved);
    return ok;
}

TEST(gfxGifDifferentialRandomFiles) {
    int played = 0, errors = 0;
    for (uint32_t seed = 1; seed <= 3000; seed++) {
        uint32_t s = seed * 2654435761u;
        MFile mf;
        DBuf b = {0};
        genFile(&s, &mf, &b);
        DecCountAlloc ca;
        GfxAllocator al;
        decAllocInit(&ca, &al, -1);
        GfxGif *g = NULL;
        GfxDecodeLimits lim = {256, 256, 70000, 1u << 24};
        Status st = gfxGifOpen(b.b, b.n, &lim, &al, &g);
        bool ok = st == STATUS_OK;
        if (!ok) {
            fprintf(stderr, "  seed %u: open %d\n", seed, (int)st);
        }
        if (ok) {
            GfxGifInfo info = gfxGifGetInfo(g);
            ok = info.width == mf.w && info.height == mf.h && info.frameCount == mf.nFrames &&
                 info.loopCount == mf.loop;
            if (!ok) {
                fprintf(stderr, "  seed %u: info differs\n", seed);
            }
        }
        for (int pass = 0; pass < 2 && ok; pass++) { /* the second pass checks Rewind */
            ok = playCompare(g, &mf, seed);
            gfxGifRewind(g);
        }
        played++;
        for (uint32_t k = 0; k < mf.nFrames; k++) {
            free(mf.fr[k].raw);
        }
        gfxGifClose(g);
        free(b.b);
        if (ok && ca.live != 0) {
            fprintf(stderr, "  seed %u: %d allocations live after Close\n", seed, ca.live);
            ok = false;
        }
        if (!ok) {
            errors++;
            if (errors > 5) {
                break;
            }
        }
    }
    ASSERT_EQ(errors, 0);
    ASSERT_EQ(played, 3000);
}
