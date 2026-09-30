/* Host tests for libs/gfx's GIF decoder and animation compositor (M12.7, D-163/D-164). The main
 * oracle is tests/data/gfx/gif-fixtures.z: GIF files plus the exact canvas after every frame,
 * produced by an independent LZW encoder and compositor (tests/data/gfx/gen_gif.py) from the pixel
 * indices it started with, never by parsing the bitstream. The rest are malformed-input,
 * structure, limit, exact-budget, truncation, mutation-fuzz and allocation-failure tests, all run
 * under ASan/UBSan/LSan with an allocator that counts what is still live. */
#include "framework/test.h"
#include "gfx/gfx-image.h"
#include "gfx/gfx.h"
#include "gfx_decode_testutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the fixture container ------------------------------------------------------------- */

typedef struct {
    uint32_t delayMs;
    uint32_t disposal;
    int32_t rect[4];
    const uint8_t *canvas; /* W*H little-endian u32 */
} FrameExp;

typedef struct {
    char name[96];
    uint32_t w, h, frameCount;
    int32_t loop;
    const uint8_t *data;
    uint32_t dataLen;
    FrameExp *frames;
} GifFx;

typedef struct {
    uint8_t *raw;
    GifFx *fx;
    int n;
} GifFxSet;

static bool loadFx(GifFxSet *fs) {
    memset(fs, 0, sizeof(*fs));
    size_t rawLen;
    if (!decLoadContainer("tests/data/gfx/gif-fixtures.z", &fs->raw, &rawLen)) {
        return false;
    }
    fs->fx = calloc(512, sizeof(GifFx));
    size_t off = 0;
    while (off < rawLen && fs->n < 512) {
        GifFx *x = &fs->fx[fs->n];
        uint32_t nameLen = decLe16(fs->raw + off);
        off += 2;
        memcpy(x->name, fs->raw + off, nameLen < 95 ? nameLen : 95);
        off += nameLen;
        x->w = decLe32(fs->raw + off);
        x->h = decLe32(fs->raw + off + 4);
        x->frameCount = decLe32(fs->raw + off + 8);
        x->loop = (int32_t)decLe32(fs->raw + off + 12);
        x->dataLen = decLe32(fs->raw + off + 16);
        off += 20;
        x->data = fs->raw + off;
        off += x->dataLen;
        x->frames = calloc(x->frameCount != 0 ? x->frameCount : 1, sizeof(FrameExp));
        for (uint32_t i = 0; i < x->frameCount; i++) {
            FrameExp *f = &x->frames[i];
            f->delayMs = decLe32(fs->raw + off);
            f->disposal = fs->raw[off + 4];
            for (int k = 0; k < 4; k++) {
                f->rect[k] = (int32_t)decLe32(fs->raw + off + 5 + 4 * k);
            }
            off += 21;
            f->canvas = fs->raw + off;
            off += (size_t)x->w * x->h * 4;
        }
        fs->n++;
    }
    return off == rawLen;
}

static void freeFx(GifFxSet *fs) {
    for (int i = 0; i < fs->n; i++) {
        free(fs->fx[i].frames);
    }
    free(fs->raw);
    free(fs->fx);
}

static const GifFx *findFx(const GifFxSet *fs, const char *name) {
    for (int i = 0; i < fs->n; i++) {
        if (strcmp(fs->fx[i].name, name) == 0) {
            return &fs->fx[i];
        }
    }
    return NULL;
}

/* True if frame `i` of `x` matches what the decoder produced; prints the first difference. */
static bool frameMatches(const GifFx *x, uint32_t i, const GfxGifFrame *f) {
    const FrameExp *e = &x->frames[i];
    if (f->index != i || f->delayMs != e->delayMs || f->disposal != e->disposal ||
        f->rect.x0 != e->rect[0] || f->rect.y0 != e->rect[1] || f->rect.x1 != e->rect[2] ||
        f->rect.y1 != e->rect[3]) {
        fprintf(stderr, "  %s frame %u: index %u delay %u disp %u rect %d,%d,%d,%d\n", x->name, i,
                f->index, f->delayMs, f->disposal, f->rect.x0, f->rect.y0, f->rect.x1, f->rect.y1);
        return false;
    }
    if (f->canvas.pixels == NULL || (uint32_t)f->canvas.width != x->w ||
        (uint32_t)f->canvas.height != x->h || (uint32_t)f->canvas.stride != x->w) {
        return false;
    }
    for (uint32_t p = 0; p < x->w * x->h; p++) {
        uint32_t want = decLe32(e->canvas + (size_t)p * 4);
        if (f->canvas.pixels[p] != want || !decPremulOk(f->canvas.pixels[p])) {
            fprintf(stderr, "  %s frame %u: pixel (%u,%u) got %08X want %08X\n", x->name, i,
                    p % x->w, p / x->w, f->canvas.pixels[p], want);
            return false;
        }
    }
    return true;
}

/* Plays every frame of `g` against `x`, then checks NOT_FOUND (twice) and that the canvas was not
 * touched by the failed calls. */
static bool playAndCheck(GfxGif *g, const GifFx *x) {
    GfxGifFrame f;
    for (uint32_t i = 0; i < x->frameCount; i++) {
        Status st = gfxGifNextFrame(g, &f);
        if (st != STATUS_OK) {
            fprintf(stderr, "  %s frame %u: status %d\n", x->name, i, (int)st);
            return false;
        }
        if (!frameMatches(x, i, &f)) {
            return false;
        }
    }
    for (int k = 0; k < 2; k++) {
        if (gfxGifNextFrame(g, &f) != STATUS_ERR_NOT_FOUND || f.canvas.pixels != NULL ||
            f.delayMs != 0 || f.index != 0) {
            return false;
        }
    }
    return true;
}

TEST(gfxGifMatchesIndependentFixtures) {
    GifFxSet fs;
    ASSERT_TRUE(loadFx(&fs));
    ASSERT_TRUE(fs.n >= 100);
    int animated = 0, looped = 0, interlaced = 0;
    for (int i = 0; i < fs.n; i++) {
        const GifFx *x = &fs.fx[i];
        DecCountAlloc ca;
        GfxAllocator al;
        decAllocInit(&ca, &al, -1);
        GfxGif *g = NULL;
        Status st = gfxGifOpen(x->data, x->dataLen, NULL, &al, &g);
        if (st != STATUS_OK) {
            fprintf(stderr, "  fixture %s: open status %d\n", x->name, (int)st);
        }
        ASSERT_EQ(st, STATUS_OK);
        ASSERT_TRUE(g != NULL);
        GfxGifInfo info = gfxGifGetInfo(g);
        ASSERT_EQ(info.width, x->w);
        ASSERT_EQ(info.height, x->h);
        ASSERT_EQ(info.frameCount, x->frameCount);
        ASSERT_EQ(info.loopCount, x->loop);
        ASSERT_TRUE(playAndCheck(g, x));
        /* Rewind replays the whole animation identically, twice */
        for (int pass = 0; pass < 2; pass++) {
            gfxGifRewind(g);
            ASSERT_TRUE(playAndCheck(g, x));
        }
        gfxGifClose(g);
        ASSERT_EQ(ca.live, 0);

        /* gfxGifDecode and the sniffing entry point both give frame 0 */
        GfxImage a, b;
        ASSERT_EQ(gfxGifDecode(x->data, x->dataLen, NULL, &al, &a), STATUS_OK);
        ASSERT_EQ(gfxImageDecode(x->data, x->dataLen, NULL, &al, &b), STATUS_OK);
        ASSERT_EQ(a.width, x->w);
        ASSERT_EQ(a.height, x->h);
        ASSERT_EQ(b.width, x->w);
        ASSERT_TRUE(a.alloc == &al && b.alloc == &al);
        for (uint32_t p = 0; p < x->w * x->h; p++) {
            ASSERT_EQ(a.pixels[p], decLe32(x->frames[0].canvas + (size_t)p * 4));
        }
        ASSERT_EQ(memcmp(a.pixels, b.pixels, (size_t)x->w * x->h * 4), 0);
        gfxImageFree(&a);
        gfxImageFree(&b);
        gfxImageFree(&a); /* idempotent */
        ASSERT_EQ(ca.live, 0);
        animated += x->frameCount > 1;
        looped += x->loop >= 0;
        interlaced += strncmp(x->name, "interlace", 9) == 0;
    }
    ASSERT_TRUE(animated >= 25 && looped >= 8 && interlaced >= 20);
    /* spot-check the loop-count expectations the generator documents */
    ASSERT_EQ(findFx(&fs, "loop_0")->loop, 0);
    ASSERT_EQ(findFx(&fs, "loop_5")->loop, 5);
    ASSERT_EQ(findFx(&fs, "loop_animexts")->loop, 3);
    ASSERT_EQ(findFx(&fs, "lzw_m4")->loop, -1);
    ASSERT_EQ(findFx(&fs, "loop_short_data_block")->loop, -1);
    ASSERT_EQ(findFx(&fs, "loop_between_frames_first_wins")->loop, 4);
    freeFx(&fs);
}

/* ---- helpers for crafted files ---------------------------------------------------------- */

typedef struct {
    uint8_t *b;
    size_t n, cap;
} Buf;

static void bufPut(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->b = realloc(b->b, b->cap);
    }
    if (n != 0) {
        memcpy(b->b + b->n, p, n);
    }
    b->n += n;
}

static void bufByte(Buf *b, uint32_t v) {
    uint8_t x = (uint8_t)v;
    bufPut(b, &x, 1);
}

static void bufLe16(Buf *b, uint32_t v) {
    bufByte(b, v & 0xFF);
    bufByte(b, (v >> 8) & 0xFF);
}

typedef struct {
    uint16_t code;
    uint8_t width;
} CodeW;

/* LSB-first bit packing of `codes`, split into sub-blocks of at most `block` bytes, terminated. */
static void putCodes(Buf *b, const CodeW *codes, size_t n, size_t block) {
    Buf raw = {0};
    uint32_t acc = 0, nb = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= (uint32_t)codes[i].code << nb;
        nb += codes[i].width;
        while (nb >= 8) {
            bufByte(&raw, acc & 0xFF);
            acc >>= 8;
            nb -= 8;
        }
    }
    if (nb != 0) {
        bufByte(&raw, acc & 0xFF);
    }
    for (size_t off = 0; off < raw.n;) {
        size_t len = raw.n - off < block ? raw.n - off : block;
        bufByte(b, (uint32_t)len);
        bufPut(b, raw.b + off, len);
        off += len;
    }
    bufByte(b, 0);
    free(raw.b);
}

/* "GIF89a", logical screen w x h, a global color table of `gct` entries (0 = none, else a power
 * of two 2..256; entry i is (i*37, i*11, i*3) mod 256). */
static void gifHeader(Buf *b, uint32_t w, uint32_t h, uint32_t gct) {
    bufPut(b, "GIF89a", 6);
    bufLe16(b, w);
    bufLe16(b, h);
    uint32_t bits = 0;
    while (gct != 0 && (2u << bits) < gct) {
        bits++;
    }
    bufByte(b, gct != 0 ? 0x80 | 0x70 | bits : 0x70);
    bufByte(b, 0);
    bufByte(b, 0);
    for (uint32_t i = 0; i < gct; i++) {
        bufByte(b, i * 37);
        bufByte(b, i * 11);
        bufByte(b, i * 3);
    }
}

static uint32_t palArgb(uint32_t i) {
    return 0xFF000000u | (((i * 37) & 0xFF) << 16) | (((i * 11) & 0xFF) << 8) | ((i * 3) & 0xFF);
}

/* Local table entry i is (200+i, 100+i, 50+i). */
static uint32_t lctArgb(uint32_t i) {
    return 0xFF000000u | ((200 + i) << 16) | ((100 + i) << 8) | (50 + i);
}

typedef struct {
    uint32_t x, y, w, h, m;
    int disposal; /* -1: no Graphic Control Extension */
    int transp;   /* -1: none */
    bool interlace;
    uint32_t delay;
    uint32_t lct; /* local table entries, 0 = none */
} FrameSpec;

static void gifTrailer(Buf *b) {
    bufByte(b, 0x3B);
}

static void frameStart(Buf *b, const FrameSpec *s) {
    if (s->disposal >= 0) {
        bufByte(b, 0x21);
        bufByte(b, 0xF9);
        bufByte(b, 4);
        bufByte(b, ((uint32_t)s->disposal << 2) | (s->transp >= 0 ? 1 : 0));
        bufLe16(b, s->delay);
        bufByte(b, s->transp >= 0 ? (uint32_t)s->transp : 0);
        bufByte(b, 0);
    }
    bufByte(b, 0x2C);
    bufLe16(b, s->x);
    bufLe16(b, s->y);
    bufLe16(b, s->w);
    bufLe16(b, s->h);
    uint32_t bits = 0;
    while (s->lct != 0 && (2u << bits) < s->lct) {
        bits++;
    }
    bufByte(b, (s->interlace ? 0x40 : 0) | (s->lct != 0 ? 0x80 | bits : 0));
    for (uint32_t i = 0; i < s->lct; i++) {
        bufByte(b, 200 + i);
        bufByte(b, 100 + i);
        bufByte(b, 50 + i);
    }
    bufByte(b, s->m);
}

/* A frame from raw LZW codes (values only): the code widths are worked out the way a decoder
 * tracks them, so tests can say WHAT is coded and leave HOW WIDE to this helper. (The fixtures
 * from gen_gif.py, not this mirror, are the oracle for the width rule itself.) */
static void gifFrameRaw(Buf *b, const FrameSpec *s, const uint16_t *codes, size_t n) {
    uint32_t clear = 1u << s->m, eoi = clear + 1, next = clear + 2, width = s->m + 1;
    uint32_t prev = 0xFFFFFFFFu;
    CodeW *c = malloc((n + 1) * sizeof(CodeW));
    for (size_t i = 0; i < n; i++) {
        c[i] = (CodeW){codes[i], (uint8_t)width};
        if (codes[i] == clear) {
            width = s->m + 1;
            next = clear + 2;
            prev = 0xFFFFFFFFu;
        } else if (codes[i] != eoi) {
            if (prev != 0xFFFFFFFFu && next < 4096) {
                next++;
                if (next >= (1u << width) && width < 12) {
                    width++;
                }
            }
            prev = codes[i];
        }
    }
    frameStart(b, s);
    putCodes(b, c, n, 255);
    free(c);
}

/* A frame from file-order indices, coded with literals only: a clear, one literal per pixel,
 * the end code. */
static void gifFrame(Buf *b, const FrameSpec *s, const uint16_t *idx, size_t n) {
    uint16_t *c = malloc((n + 2) * sizeof(uint16_t));
    c[0] = (uint16_t)(1u << s->m);
    memcpy(c + 1, idx, n * sizeof(uint16_t));
    c[n + 1] = (uint16_t)((1u << s->m) + 1);
    gifFrameRaw(b, s, c, n + 2);
    free(c);
}

/* A w x h frame at (x,y) filled with `fill` (pixel count w*h). */
static void gifSolid(Buf *b, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int disposal,
                     int transp, uint16_t fill) {
    size_t n = (size_t)w * h;
    uint16_t *idx = malloc((n != 0 ? n : 1) * sizeof(uint16_t));
    for (size_t i = 0; i < n; i++) {
        idx[i] = fill;
    }
    FrameSpec s = {x, y, w, h, 2, disposal, transp, false, 1, 0};
    gifFrame(b, &s, idx, n);
    free(idx);
}

/* A 4x4 canvas with a 4-entry global table and one 4x4 frame coded with `codes`. */
static void gifOneFrame(Buf *b, uint32_t m, const uint16_t *codes, size_t n) {
    gifHeader(b, 4, 4, 4);
    FrameSpec s = {0, 0, 4, 4, m, -1, -1, false, 0, 0};
    gifFrameRaw(b, &s, codes, n);
    gifTrailer(b);
}

/* The allocator must outlive an image, so crafted-file tests share one static. */
typedef struct {
    DecCountAlloc ca;
    GfxAllocator al;
} Alloc;

static void allocInit(Alloc *a, int failAt) {
    decAllocInit(&a->ca, &a->al, failAt);
}

/* Decodes `b` (a 4x4 canvas) as frame 0 with gfxGifDecode; returns the status and checks the
 * zeroing rules and live == 0. If `px` is non-NULL the 16 pixels are copied out. */
static Status decodeOnly(const Buf *b, const GfxDecodeLimits *lim, uint32_t *px) {
    Alloc a;
    allocInit(&a, -1);
    GfxImage img;
    memset(&img, 0xAA, sizeof(img));
    Status st = gfxGifDecode(b->b, b->n, lim, &a.al, &img);
    if ((st == STATUS_OK) != (img.pixels != NULL)) {
        fprintf(stderr, "  decode status %d but pixels %p\n", (int)st, (void *)img.pixels);
        st = STATUS_ERR_INVALID + 1000;
    }
    if (st != STATUS_OK && (img.width != 0 || img.height != 0 || img.allocSize != 0)) {
        st = STATUS_ERR_INVALID + 2000;
    }
    if (px != NULL && st == STATUS_OK && img.width * img.height == 16) {
        memcpy(px, img.pixels, 16 * sizeof(uint32_t));
    }
    gfxImageFree(&img);
    if (a.ca.live != 0) {
        st = STATUS_ERR_INVALID + 3000;
    }
    return st;
}

#define EXPECT_DECODE(buf, want)                                                                   \
    do {                                                                                           \
        Status r_ = decodeOnly(&(buf), NULL, NULL);                                                \
        if (r_ != (want)) {                                                                        \
            fprintf(stderr, "  line %d: got %d want %d\n", __LINE__, (int)r_, (int)(want));        \
        }                                                                                          \
        ASSERT_EQ(r_, (want));                                                                     \
        free((buf).b);                                                                             \
    } while (0)

/* Decodes the 4x4 file `b`, which must succeed, and requires the first `n` pixels to be `want`
 * and the other 16-n to be untouched (transparent). Frees `b`. */
static bool decodesTo(Buf *b, const uint32_t *want, size_t n) {
    uint32_t px[16];
    Status st = decodeOnly(b, NULL, px);
    free(b->b);
    if (st != STATUS_OK) {
        fprintf(stderr, "  decodesTo: status %d\n", (int)st);
        return false;
    }
    for (size_t i = 0; i < 16; i++) {
        uint32_t w = i < n ? want[i] : 0;
        if (px[i] != w) {
            fprintf(stderr, "  decodesTo: pixel %zu got %08X want %08X\n", i, px[i], w);
            return false;
        }
    }
    return true;
}

/* ---- LZW errors and tolerance ---------------------------------------------------------- */

TEST(gfxGifLzwErrors) {
    /* m=2: clear=4, eoi=5, first table entry 6. After the first code a decoder has next == 6;
     * each further code adds one entry, and a code may be at most `next` (KwKwK) when it is read.
     */
    Buf b;
    uint32_t want[16];
    {
        uint16_t c[] = {4, 2, 6, 5}; /* 2, then 6 == next: KwKwK -> "22" */
        b = (Buf){0};
        gifOneFrame(&b, 2, c, 4);
        want[0] = want[1] = want[2] = palArgb(2);
        ASSERT_TRUE(decodesTo(&b, want, 3));
    }
    {
        /* every kind of table string: 1 2 | 6="12" | 7="21" | 8="122" | 9="211" */
        uint16_t c[] = {4, 1, 2, 6, 7, 8, 5};
        b = (Buf){0};
        gifOneFrame(&b, 2, c, 7);
        static const uint8_t seq[] = {1, 2, 1, 2, 2, 1, 1, 2, 2};
        for (size_t i = 0; i < sizeof(seq); i++) {
            want[i] = palArgb(seq[i]);
        }
        ASSERT_TRUE(decodesTo(&b, want, sizeof(seq)));
    }
    {
        uint16_t c[] = {4, 0, 1, 7, 5}; /* the third code may equal next (7): "1" + "1" */
        b = (Buf){0};
        gifOneFrame(&b, 2, c, 5);
        want[0] = palArgb(0);
        want[1] = want[2] = want[3] = palArgb(1);
        ASSERT_TRUE(decodesTo(&b, want, 4));
    }
    /* a code beyond next */
    {
        uint16_t c[] = {4, 0, 7, 5};
        b = (Buf){0};
        gifOneFrame(&b, 2, c, 4);
        EXPECT_DECODE(b, STATUS_ERR_INVALID);
        uint16_t c2[] = {4, 0, 1, 2, 9, 5}; /* next is 8 when the 4-bit code 9 arrives */
        b = (Buf){0};
        gifOneFrame(&b, 2, c2, 6);
        EXPECT_DECODE(b, STATUS_ERR_INVALID);
        uint16_t c3[] = {4, 0, 1, 2, 3, 10, 5};
        b = (Buf){0};
        gifOneFrame(&b, 2, c3, 7);
        EXPECT_DECODE(b, STATUS_ERR_INVALID);
        uint16_t c4[] = {4, 0, 1, 2, 3, 9, 5}; /* next is 9 here: KwKwK, fine */
        b = (Buf){0};
        gifOneFrame(&b, 2, c4, 7);
        ASSERT_EQ(decodeOnly(&b, NULL, NULL), STATUS_OK);
        free(b.b);
    }
    /* the first code after a clear must be a literal: a table code, KwKwK right after the clear,
     * and (with no clear at all) the very first code; a clear in the middle restores the rule */
    {
        uint16_t c[] = {4, 6, 5};
        b = (Buf){0};
        gifOneFrame(&b, 2, c, 3);
        EXPECT_DECODE(b, STATUS_ERR_INVALID);
        uint16_t c2[] = {4, 7, 5};
        b = (Buf){0};
        gifOneFrame(&b, 2, c2, 3);
        EXPECT_DECODE(b, STATUS_ERR_INVALID);
        uint16_t c3[] = {6, 0, 5};
        b = (Buf){0};
        gifOneFrame(&b, 2, c3, 3);
        EXPECT_DECODE(b, STATUS_ERR_INVALID);
        uint16_t c4[] = {4, 0, 1, 4, 6, 5};
        b = (Buf){0};
        gifOneFrame(&b, 2, c4, 6);
        EXPECT_DECODE(b, STATUS_ERR_INVALID);
        uint16_t c5[] = {4, 0, 1, 4, 3, 2, 6, 5}; /* after a mid-stream clear: 3 2 | 6="32" */
        b = (Buf){0};
        gifOneFrame(&b, 2, c5, 8);
        uint32_t w5[] = {palArgb(0), palArgb(1), palArgb(3), palArgb(2), palArgb(3), palArgb(2)};
        ASSERT_TRUE(decodesTo(&b, w5, 6));
    }
    /* no initial clear is fine when the first code is a literal */
    {
        uint16_t c[] = {2, 3, 6, 5};
        b = (Buf){0};
        gifOneFrame(&b, 2, c, 4);
        uint32_t w[] = {palArgb(2), palArgb(3), palArgb(2), palArgb(3)};
        ASSERT_TRUE(decodesTo(&b, w, 4));
    }
    /* an immediate end code, and no data at all: OK, nothing drawn (tolerant) */
    {
        uint16_t c[] = {5};
        b = (Buf){0};
        gifOneFrame(&b, 2, c, 1);
        ASSERT_TRUE(decodesTo(&b, NULL, 0));
        b = (Buf){0};
        gifOneFrame(&b, 2, NULL, 0); /* the chain is just its terminator */
        ASSERT_TRUE(decodesTo(&b, NULL, 0));
    }
    /* the minimum code size: 2..11 decode, everything else is INVALID at open (giflib never
     * writes 1; 0 and 12+ cannot be coded) */
    for (uint32_t m = 0; m <= 255; m += (m < 16 ? 1 : 47)) {
        uint16_t c[] = {(uint16_t)(m < 12 ? 1u << m : 4), 0,
                        (uint16_t)(m < 12 ? (1u << m) + 1 : 5)};
        b = (Buf){0};
        gifHeader(&b, 4, 4, 4);
        FrameSpec s = {0, 0, 4, 4, m < 12 ? m : 2, -1, -1, false, 0, 0};
        gifFrameRaw(&b, &s, c, 3);
        b.b[13 + 12 + 10] = (uint8_t)m; /* the minimum code size byte */
        gifTrailer(&b);
        Status wantSt = (m >= 2 && m <= 11) ? STATUS_OK : STATUS_ERR_INVALID;
        Status got = decodeOnly(&b, NULL, NULL);
        if (got != wantSt) {
            fprintf(stderr, "  m=%u: got %d want %d\n", m, (int)got, (int)wantSt);
        }
        ASSERT_EQ(got, wantSt);
        Alloc a;
        allocInit(&a, -1);
        GfxGif *g = (GfxGif *)1;
        ASSERT_EQ(gfxGifOpen(b.b, b.n, NULL, &a.al, &g), wantSt);
        if (wantSt == STATUS_OK) {
            gfxGifClose(g);
        } else {
            ASSERT_TRUE(g == NULL);
        }
        ASSERT_EQ(a.ca.live, 0);
        free(b.b);
    }
    /* a frame that is full ignores the surplus codes, even invalid ones (31 > next here) */
    {
        uint16_t c[24];
        size_t n = 0;
        c[n++] = 4;
        for (int i = 0; i < 20; i++) {
            c[n++] = 1;
        }
        c[n++] = 31;
        c[n++] = 5;
        b = (Buf){0};
        gifOneFrame(&b, 2, c, n);
        for (int i = 0; i < 16; i++) {
            want[i] = palArgb(1);
        }
        ASSERT_TRUE(decodesTo(&b, want, 16));
        /* the same bad code BEFORE the frame is full is INVALID */
        uint16_t e[] = {4, 1, 31, 1};
        b = (Buf){0};
        gifOneFrame(&b, 2, e, 4);
        EXPECT_DECODE(b, STATUS_ERR_INVALID);
    }
}

/* An LZW error in a later frame is sticky until Rewind, and frame 0 is still good after it. */
TEST(gfxGifStickyErrorClearedByRewind) {
    static const uint16_t badCodes[] = {4, 1, 7, 5};
    FrameSpec s = {0, 0, 4, 4, 2, 1, -1, false, 0, 0};
    Buf b = {0};
    gifHeader(&b, 4, 4, 4);
    gifSolid(&b, 0, 0, 4, 4, 1, -1, 2);
    gifFrameRaw(&b, &s, badCodes, 4);
    gifSolid(&b, 0, 0, 4, 4, 1, -1, 3);
    gifTrailer(&b);
    Alloc a;
    allocInit(&a, -1);
    GfxGif *g;
    ASSERT_EQ(gfxGifOpen(b.b, b.n, NULL, &a.al, &g), STATUS_OK); /* open does not decode LZW */
    ASSERT_EQ(gfxGifGetInfo(g).frameCount, 3u);
    for (int round = 0; round < 2; round++) {
        GfxGifFrame f;
        ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_OK);
        ASSERT_EQ(f.index, 0u);
        ASSERT_EQ(f.canvas.pixels[0], palArgb(2));
        memset(&f, 0x55, sizeof(f));
        ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_ERR_INVALID);
        ASSERT_TRUE(f.canvas.pixels == NULL && f.rect.x1 == 0 && f.delayMs == 0);
        for (int k = 0; k < 3; k++) { /* sticky */
            memset(&f, 0x55, sizeof(f));
            ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_ERR_INVALID);
            ASSERT_TRUE(f.canvas.pixels == NULL);
        }
        gfxGifRewind(g);
    }
    gfxGifClose(g);
    ASSERT_EQ(a.ca.live, 0);
    EXPECT_DECODE(b, STATUS_OK); /* gfxGifDecode only needs frame 0 */
    /* an error in frame 0 fails the single-image path and leaves nothing behind */
    Buf c = {0};
    gifHeader(&c, 4, 4, 4);
    gifFrameRaw(&c, &s, badCodes, 4);
    gifTrailer(&c);
    EXPECT_DECODE(c, STATUS_ERR_INVALID);
}

/* ---- container structure ---------------------------------------------------------------- */

static Buf validOne(void) {
    Buf b = {0};
    gifHeader(&b, 4, 4, 4);
    gifSolid(&b, 0, 0, 4, 4, -1, -1, 1);
    gifTrailer(&b);
    return b;
}

static Status openOnly(const Buf *b, const GfxDecodeLimits *lim, GfxGifInfo *info) {
    Alloc a;
    allocInit(&a, -1);
    GfxGif *g = (GfxGif *)1;
    Status st = gfxGifOpen(b->b, b->n, lim, &a.al, &g);
    if (st == STATUS_OK) {
        if (info != NULL) {
            *info = gfxGifGetInfo(g);
        }
        gfxGifClose(g);
    } else if (g != NULL) {
        st = STATUS_ERR_INVALID + 1000;
    }
    if (a.ca.live != 0) {
        st = STATUS_ERR_INVALID + 3000;
    }
    return st;
}

#define EXPECT_OPEN(buf, want)                                                                     \
    do {                                                                                           \
        Status r_ = openOnly(&(buf), NULL, NULL);                                                  \
        if (r_ != (want)) {                                                                        \
            fprintf(stderr, "  line %d: got %d want %d\n", __LINE__, (int)r_, (int)(want));        \
        }                                                                                          \
        ASSERT_EQ(r_, (want));                                                                     \
    } while (0)

TEST(gfxGifStructure) {
    Buf b = validOne();
    EXPECT_OPEN(b, STATUS_OK);
    /* data after the trailer is ignored */
    bufPut(&b, "trailing junk \x21\x00", 15);
    EXPECT_OPEN(b, STATUS_OK);
    free(b.b);

    /* no trailer; a bad introducer where the trailer should be; a bad one between blocks */
    b = validOne();
    b.n--;
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    b.b[b.n] = 0x55;
    b.n++;
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);
    b = (Buf){0};
    gifHeader(&b, 4, 4, 4);
    bufByte(&b, 0x00); /* a stray zero (the terminator of nothing) */
    gifSolid(&b, 0, 0, 4, 4, -1, -1, 1);
    gifTrailer(&b);
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);

    /* sub-blocks running past the end: image data, an extension, a comment */
    b = validOne();
    b.b[13 + 12 + 10 + 1] = 0xFF; /* the length byte of the first data sub-block */
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);
    b = (Buf){0};
    gifHeader(&b, 4, 4, 4);
    bufPut(&b,
           "\x21\xFE\x05"
           "abc",
           6); /* a comment claiming 5 bytes, holding 3 */
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);
    b = (Buf){0};
    gifHeader(&b, 4, 4, 4);
    bufPut(&b,
           "\x21\xFE\x03"
           "abc",
           6); /* a comment with no terminator */
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);
    b = (Buf){0};
    gifHeader(&b, 4, 4, 4);
    bufPut(&b, "\x21", 1); /* an extension introducer and nothing else */
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);

    /* zero frames, with and without a global table */
    b = (Buf){0};
    gifHeader(&b, 4, 4, 4);
    gifTrailer(&b);
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);
    b = (Buf){0};
    gifHeader(&b, 4, 4, 0);
    gifTrailer(&b);
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);

    /* signatures: only exactly GIF87a / GIF89a */
    static const char *sigs[] = {"GIF89b", "GIF90a", "gif89a", "GIF88a", "GIF89 ", "GIF8", "", "G"};
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        b = validOne();
        size_t sl = strlen(sigs[i]);
        memcpy(b.b, sigs[i], sl < 6 ? sl : 6);
        if (sl < 6) {
            b.n = sl; /* also too short */
        }
        EXPECT_OPEN(b, STATUS_ERR_INVALID);
        free(b.b);
    }
    b = validOne();
    memcpy(b.b, "GIF87a", 6);
    EXPECT_OPEN(b, STATUS_OK);
    free(b.b);

    /* a header shorter than the logical screen descriptor, and a truncated global table */
    b = validOne();
    b.n = 12;
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    b.n = 13 + 11; /* 12 bytes of a 4-entry (12-byte) table is fine, 11 is not */
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);

    /* zero-sized logical screen */
    b = validOne();
    b.b[6] = b.b[7] = 0;
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);
    b = validOne();
    b.b[8] = b.b[9] = 0;
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);

    /* Graphic Control Extension: a first sub-block under 4 bytes is INVALID, more is fine */
    for (uint32_t len = 0; len <= 6; len++) {
        b = (Buf){0};
        gifHeader(&b, 4, 4, 4);
        bufByte(&b, 0x21);
        bufByte(&b, 0xF9);
        if (len != 0) {
            bufByte(&b, len);
            for (uint32_t i = 0; i < len; i++) {
                bufByte(&b, i == 0 ? 0x05 : 0x00);
            }
        }
        bufByte(&b, 0);
        gifSolid(&b, 0, 0, 4, 4, -1, -1, 1);
        gifTrailer(&b);
        Status want = len >= 4 ? STATUS_OK : STATUS_ERR_INVALID;
        Status got = openOnly(&b, NULL, NULL);
        if (got != want) {
            fprintf(stderr, "  GCE len %u: got %d want %d\n", len, (int)got, (int)want);
        }
        ASSERT_EQ(got, want);
        free(b.b);
    }

    /* an image descriptor cut short, a local table cut short, no minimum code size byte */
    b = validOne();
    b.n = 13 + 12 + 6;
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    b.n = 13 + 12 + 10;
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);
    b = (Buf){0};
    gifHeader(&b, 4, 4, 4);
    bufPut(&b, "\x2C\0\0\0\0\4\0\4\0\x81", 10); /* a 4-entry local table claimed, 5 bytes given */
    bufPut(&b, "\1\2\3\4\5", 5);
    EXPECT_OPEN(b, STATUS_ERR_INVALID);
    free(b.b);

    /* the API on odd inputs */
    gfxGifClose(NULL);
    gfxGifRewind(NULL);
    GfxGifInfo none = gfxGifGetInfo(NULL);
    ASSERT_EQ(none.frameCount, 0u);
    ASSERT_EQ(none.loopCount, -1);
    GfxImage img;
    ASSERT_EQ(gfxGifDecode((const uint8_t *)"GIF89a", 6, NULL, NULL, &img), STATUS_ERR_INVALID);
    ASSERT_TRUE(img.pixels == NULL);
    ASSERT_EQ(gfxImageDecode((const uint8_t *)"GIF87a", 6, NULL, NULL, &img), STATUS_ERR_INVALID);
    ASSERT_TRUE(img.pixels == NULL);
}

TEST(gfxGifInfoLocalTablesAndRawFields) {
    /* loop count 7; frame 1 has a local table, disposal 7 (acts as 0), delay 250 -> 2500 ms */
    Buf b = {0};
    gifHeader(&b, 6, 5, 4);
    bufPut(&b, "\x21\xFF\x0BNETSCAPE2.0\x03\x01\x07\x00\x00", 19);
    gifSolid(&b, 0, 0, 6, 5, 1, -1, 1);
    static const uint16_t lctIdx[] = {0, 1, 2, 3};
    FrameSpec s = {1, 1, 2, 2, 2, 7, -1, false, 250, 4};
    gifFrame(&b, &s, lctIdx, 4);
    gifSolid(&b, 4, 3, 2, 2, 0, -1, 3);
    gifTrailer(&b);
    Alloc a;
    allocInit(&a, -1);
    GfxGif *g;
    ASSERT_EQ(gfxGifOpen(b.b, b.n, NULL, &a.al, &g), STATUS_OK);
    GfxGifInfo info = gfxGifGetInfo(g);
    ASSERT_EQ(info.width, 6u);
    ASSERT_EQ(info.height, 5u);
    ASSERT_EQ(info.frameCount, 3u);
    ASSERT_EQ(info.loopCount, 7);
    for (int round = 0; round < 2; round++) {
        GfxGifFrame f;
        ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_OK);
        ASSERT_EQ(f.delayMs, 10u);
        ASSERT_EQ(f.disposal, 1u);
        ASSERT_TRUE(f.rect.x0 == 0 && f.rect.y0 == 0 && f.rect.x1 == 6 && f.rect.y1 == 5);
        for (int i = 0; i < 30; i++) {
            ASSERT_EQ(f.canvas.pixels[i], palArgb(1));
        }
        ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_OK);
        ASSERT_EQ(f.delayMs, 2500u);
        ASSERT_EQ(f.disposal, 7u);
        ASSERT_TRUE(f.rect.x0 == 1 && f.rect.y0 == 1 && f.rect.x1 == 3 && f.rect.y1 == 3);
        ASSERT_EQ(f.canvas.pixels[1 * 6 + 1], lctArgb(0));
        ASSERT_EQ(f.canvas.pixels[1 * 6 + 2], lctArgb(1));
        ASSERT_EQ(f.canvas.pixels[2 * 6 + 1], lctArgb(2));
        ASSERT_EQ(f.canvas.pixels[2 * 6 + 2], lctArgb(3));
        ASSERT_EQ(f.canvas.pixels[0], palArgb(1));
        ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_OK);
        ASSERT_EQ(f.delayMs, 10u);
        ASSERT_EQ(f.disposal, 0u);
        ASSERT_EQ(f.canvas.pixels[1 * 6 + 1], lctArgb(0)); /* disposal 7 left frame 1 in place */
        ASSERT_EQ(f.canvas.pixels[3 * 6 + 4], palArgb(3));
        ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_ERR_NOT_FOUND);
        gfxGifRewind(g);
    }
    gfxGifClose(g);
    ASSERT_EQ(a.ca.live, 0);
    free(b.b);
}

/* ---- limits ------------------------------------------------------------------------------ */

TEST(gfxGifLimits) {
    Buf b = validOne(); /* 4x4 canvas, one 4x4 frame */
    GfxDecodeLimits lim = {16384, 16384, 1u << 26, 512u << 20};
    ASSERT_EQ(openOnly(&b, &lim, NULL), STATUS_OK);
    lim.maxWidth = 4;
    ASSERT_EQ(openOnly(&b, &lim, NULL), STATUS_OK);
    lim.maxWidth = 3;
    ASSERT_EQ(openOnly(&b, &lim, NULL), STATUS_ERR_UNSUPPORTED);
    ASSERT_EQ(decodeOnly(&b, &lim, NULL), STATUS_ERR_UNSUPPORTED);
    lim.maxWidth = 16384;
    lim.maxHeight = 3;
    ASSERT_EQ(openOnly(&b, &lim, NULL), STATUS_ERR_UNSUPPORTED);
    lim.maxHeight = 16384;
    lim.maxPixels = 16;
    ASSERT_EQ(openOnly(&b, &lim, NULL), STATUS_OK);
    lim.maxPixels = 15;
    ASSERT_EQ(openOnly(&b, &lim, NULL), STATUS_ERR_UNSUPPORTED);
    free(b.b);

    /* a frame whose area is over maxPixels while the canvas is not */
    b = (Buf){0};
    gifHeader(&b, 4, 4, 4);
    gifSolid(&b, 0, 0, 4, 4, -1, -1, 1);
    gifSolid(&b, 2, 2, 20, 5, -1, -1, 2); /* area 100, mostly outside the canvas */
    gifTrailer(&b);
    lim = (GfxDecodeLimits){16384, 16384, 100, 512u << 20};
    ASSERT_EQ(openOnly(&b, &lim, NULL), STATUS_OK);
    lim.maxPixels = 99;
    ASSERT_EQ(openOnly(&b, &lim, NULL), STATUS_ERR_UNSUPPORTED);
    ASSERT_EQ(decodeOnly(&b, &lim, NULL), STATUS_ERR_UNSUPPORTED); /* even for frame 0 alone */
    free(b.b);

    /* the largest legal canvas against the default limits: 65535 x 65535 is over them */
    b = validOne();
    b.b[6] = b.b[7] = b.b[8] = b.b[9] = 0xFF;
    ASSERT_EQ(openOnly(&b, NULL, NULL), STATUS_ERR_UNSUPPORTED);
    ASSERT_EQ(decodeOnly(&b, NULL, NULL), STATUS_ERR_UNSUPPORTED);
    free(b.b);
    /* over-limit beats every later problem (nothing after the header is read) */
    b = (Buf){0};
    gifHeader(&b, 20000, 3, 4);
    ASSERT_EQ(openOnly(&b, NULL, NULL), STATUS_ERR_UNSUPPORTED);
    free(b.b);
    /* a huge frame in a small canvas costs no memory but is rejected by maxPixels */
    b = (Buf){0};
    gifHeader(&b, 4, 4, 4);
    gifSolid(&b, 0, 0, 4, 4, -1, -1, 1);
    bufPut(&b, "\x2C\x00\x00\x00\x00\xFF\xFF\xFF\xFF\x00\x02\x00", 12);
    gifTrailer(&b);
    ASSERT_EQ(openOnly(&b, NULL, NULL), STATUS_ERR_UNSUPPORTED); /* 65535^2 > 2^26 */
    free(b.b);
}

/* ---- the byte budget is the exact peak ---------------------------------------------------- */

/* Live-byte peak of gfxGifOpen with an unlimited budget. */
static size_t openPeak(const Buf *b) {
    Alloc a;
    allocInit(&a, -1);
    GfxGif *g;
    if (gfxGifOpen(b->b, b->n, NULL, &a.al, &g) != STATUS_OK) {
        return 0;
    }
    size_t peak = a.ca.peakBytes;
    gfxGifClose(g);
    return a.ca.live == 0 ? peak : 0;
}

static Buf budgetFile(uint32_t w, uint32_t h, int disposal) {
    Buf b = {0};
    gifHeader(&b, w, h, 4);
    gifSolid(&b, 0, 0, w, h, disposal, -1, 1);
    gifSolid(&b, 1, 1, w - 2, h - 2, 1, -1, 2);
    gifTrailer(&b);
    return b;
}

TEST(gfxGifBudgetIsTheExactPeak) {
    for (int save = 0; save < 2; save++) {
        Buf b = budgetFile(30, 20, save ? 3 : 1);
        size_t peak = openPeak(&b);
        size_t canvas = 30 * 20 * 4;
        ASSERT_TRUE(peak > canvas * (save ? 2u : 1u));
        ASSERT_TRUE(peak < canvas * (save ? 2u : 1u) + 65536); /* the struct is the only extra */
        GfxDecodeLimits lim = {16384, 16384, 1u << 26, peak};
        Alloc a;
        allocInit(&a, -1);
        GfxGif *g = NULL;
        ASSERT_EQ(gfxGifOpen(b.b, b.n, &lim, &a.al, &g), STATUS_OK);
        ASSERT_EQ(a.ca.peakBytes, peak);
        GfxGifFrame f;
        ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_OK);
        ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_OK);
        ASSERT_EQ(a.ca.peakBytes, peak); /* decoding allocates nothing more */
        gfxGifClose(g);
        ASSERT_EQ(a.ca.live, 0);
        lim.maxTotalBytes = peak - 1;
        allocInit(&a, -1);
        g = (GfxGif *)1;
        ASSERT_EQ(gfxGifOpen(b.b, b.n, &lim, &a.al, &g), STATUS_ERR_UNSUPPORTED);
        ASSERT_TRUE(g == NULL);
        ASSERT_EQ(a.ca.live, 0);
        /* the same budget governs the single-image path */
        lim.maxTotalBytes = peak;
        ASSERT_EQ(decodeOnly(&b, &lim, NULL), STATUS_OK);
        lim.maxTotalBytes = peak - 1;
        ASSERT_EQ(decodeOnly(&b, &lim, NULL), STATUS_ERR_UNSUPPORTED);
        /* far below the struct itself, and zero */
        lim.maxTotalBytes = 1000;
        ASSERT_EQ(decodeOnly(&b, &lim, NULL), STATUS_ERR_UNSUPPORTED);
        lim.maxTotalBytes = 0;
        ASSERT_EQ(decodeOnly(&b, &lim, NULL), STATUS_ERR_UNSUPPORTED);
        free(b.b);
    }
    /* disposal 3 costs exactly one more canvas-sized buffer */
    Buf n = budgetFile(30, 20, 1), s = budgetFile(30, 20, 3);
    ASSERT_EQ(openPeak(&s) - openPeak(&n), (size_t)30 * 20 * 4);
    free(n.b);
    free(s.b);
    /* disposal 3 on ANY frame counts, even the last */
    Buf late = {0};
    gifHeader(&late, 30, 20, 4);
    gifSolid(&late, 0, 0, 30, 20, 1, -1, 1);
    gifSolid(&late, 0, 0, 5, 5, 1, -1, 1);
    gifSolid(&late, 0, 0, 5, 5, 3, -1, 2);
    gifTrailer(&late);
    Buf plain = budgetFile(30, 20, 1);
    ASSERT_EQ(openPeak(&late) - openPeak(&plain), (size_t)30 * 20 * 4);
    free(late.b);
    free(plain.b);
}

/* No frame limit: a long animation needs no more memory than a short one. */
TEST(gfxGifManyFramesNeedConstantMemory) {
    Buf b = {0};
    gifHeader(&b, 3, 2, 4);
    const uint32_t frames = 3000;
    for (uint32_t i = 0; i < frames; i++) {
        gifSolid(&b, i % 3, i % 2, 1, 1, i % 4, i % 4 == 3 ? 0 : -1, (uint16_t)(i % 4));
    }
    gifTrailer(&b);
    Alloc a;
    allocInit(&a, -1);
    GfxGif *g;
    ASSERT_EQ(gfxGifOpen(b.b, b.n, NULL, &a.al, &g), STATUS_OK);
    ASSERT_EQ(gfxGifGetInfo(g).frameCount, frames);
    int allocs = a.ca.count;
    size_t peak = a.ca.peakBytes;
    GfxGifFrame f;
    for (uint32_t i = 0; i < frames; i++) {
        ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_OK);
        ASSERT_EQ(f.index, i);
    }
    ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_ERR_NOT_FOUND);
    ASSERT_EQ(a.ca.count, allocs);
    ASSERT_EQ(a.ca.peakBytes, peak);
    gfxGifClose(g);
    ASSERT_EQ(a.ca.live, 0);
    free(b.b);
}

/* ---- truncation, mutation fuzz, allocation failure -------------------------------------- */

TEST(gfxGifTruncationEveryOffset) {
    GifFxSet fs;
    ASSERT_TRUE(loadFx(&fs));
    int checked = 0;
    for (int i = 0; i < fs.n; i++) {
        const GifFx *x = &fs.fx[i];
        /* every offset for small files; a stride (plus the last 80 and first 80 offsets) for
         * the big ones, which only differ in their LZW data */
        size_t step = x->dataLen <= 4000 ? 1 : 1 + x->dataLen / 1500;
        for (size_t cut = 0; cut < x->dataLen; cut++) {
            if (step > 1 && cut > 80 && cut + 80 < x->dataLen && cut % step != 0) {
                continue;
            }
            DecCountAlloc ca;
            GfxAllocator al;
            decAllocInit(&ca, &al, -1);
            GfxGif *g = (GfxGif *)1;
            Status st = gfxGifOpen(x->data, cut, NULL, &al, &g);
            if (st != STATUS_ERR_INVALID || g != NULL || ca.live != 0) {
                fprintf(stderr, "  %s cut at %zu of %u: status %d\n", x->name, cut, x->dataLen,
                        (int)st);
                if (st == STATUS_OK) {
                    gfxGifClose(g);
                }
            }
            ASSERT_EQ(st, STATUS_ERR_INVALID);
            ASSERT_TRUE(g == NULL);
            ASSERT_EQ(ca.live, 0);
            GfxImage img;
            ASSERT_EQ(gfxGifDecode(x->data, cut, NULL, &al, &img), STATUS_ERR_INVALID);
            ASSERT_TRUE(img.pixels == NULL);
            ASSERT_EQ(ca.live, 0);
            checked++;
        }
    }
    ASSERT_TRUE(checked > 20000);
    freeFx(&fs);
}

/* Plays up to 64 frames; folds statuses and pixels into *hash; every pixel stays premultiplied
 * and every rect inside the canvas. Returns the number of frames that decoded. */
static int playChecked(GfxGif *g, uint64_t *hash, Status *last, bool *ok) {
    GfxGifInfo info = gfxGifGetInfo(g);
    int n = 0;
    *last = STATUS_OK;
    *ok = true;
    for (int i = 0; i < 64; i++) {
        GfxGifFrame f;
        Status st = gfxGifNextFrame(g, &f);
        *hash = (*hash ^ (uint64_t)st) * 0x100000001B3ull;
        *last = st;
        if (st != STATUS_OK) {
            if (f.canvas.pixels != NULL || f.delayMs != 0) {
                *ok = false;
            }
            break;
        }
        n++;
        if (f.index != (uint32_t)i || (uint32_t)f.canvas.width != info.width ||
            (uint32_t)f.canvas.height != info.height || f.rect.x0 < 0 || f.rect.y0 < 0 ||
            f.rect.x1 > (int32_t)info.width || f.rect.y1 > (int32_t)info.height ||
            f.rect.x0 > f.rect.x1 || f.rect.y0 > f.rect.y1 || f.disposal > 7) {
            *ok = false;
        }
        for (uint32_t p = 0; p < info.width * info.height; p++) {
            if (!decPremulOk(f.canvas.pixels[p])) {
                *ok = false;
            }
            *hash = (*hash ^ f.canvas.pixels[p]) * 0x100000001B3ull;
        }
    }
    return n;
}

TEST(gfxGifMutationFuzz) {
    GifFxSet fs;
    ASSERT_TRUE(loadFx(&fs));
    GfxDecodeLimits lim = {4096, 4096, 1u << 22, 64u << 20};
    uint32_t rng = 0xC0FFEEu;
    int okRuns = 0, badRuns = 0, frames = 0;
    for (int i = 0; i < fs.n; i++) {
        const GifFx *x = &fs.fx[i];
        if (x->dataLen > 6000) {
            continue;
        }
        uint8_t *m = malloc(x->dataLen);
        for (int iter = 0; iter < 40; iter++) {
            memcpy(m, x->data, x->dataLen);
            int muts = 1 + (int)(decRng(&rng) % 4);
            for (int k = 0; k < muts; k++) {
                uint32_t pos = decRng(&rng) % x->dataLen;
                uint32_t kind = decRng(&rng) % 4;
                m[pos] = kind == 0   ? (uint8_t)(m[pos] ^ (1u << (decRng(&rng) % 8)))
                         : kind == 1 ? (uint8_t)decRng(&rng)
                         : kind == 2 ? 0xFF
                                     : 0x00;
            }
            DecCountAlloc ca;
            GfxAllocator al;
            decAllocInit(&ca, &al, -1);
            GfxGif *g = NULL;
            Status st = gfxGifOpen(m, x->dataLen, &lim, &al, &g);
            ASSERT_TRUE(st == STATUS_OK || st == STATUS_ERR_INVALID ||
                        st == STATUS_ERR_UNSUPPORTED);
            ASSERT_TRUE((st == STATUS_OK) == (g != NULL));
            if (st != STATUS_OK) {
                badRuns++;
                ASSERT_EQ(ca.live, 0);
                continue;
            }
            uint64_t h1 = 0xCBF29CE484222325ull, h2 = h1;
            Status l1, l2;
            bool ok1, ok2;
            int n1 = playChecked(g, &h1, &l1, &ok1);
            ASSERT_TRUE(ok1);
            ASSERT_TRUE(l1 == STATUS_OK || l1 == STATUS_ERR_INVALID || l1 == STATUS_ERR_NOT_FOUND);
            gfxGifRewind(g); /* a replay is bit-identical, errors included */
            int n2 = playChecked(g, &h2, &l2, &ok2);
            ASSERT_TRUE(ok2);
            ASSERT_EQ(n1, n2);
            ASSERT_TRUE(l1 == l2 && h1 == h2);
            frames += n1;
            /* the single-image path agrees with frame 0 of the streaming one */
            GfxImage img;
            Status ds = gfxGifDecode(m, x->dataLen, &lim, &al, &img);
            ASSERT_EQ(ds == STATUS_OK, n1 > 0);
            ASSERT_TRUE((ds == STATUS_OK) == (img.pixels != NULL));
            gfxImageFree(&img);
            gfxGifClose(g);
            ASSERT_EQ(ca.live, 0);
            if (l1 == STATUS_ERR_INVALID) {
                badRuns++;
            } else {
                okRuns++;
            }
        }
        free(m);
    }
    ASSERT_TRUE(okRuns > 200 && badRuns > 200 && frames > 1000);
    freeFx(&fs);
}

TEST(gfxGifAllocationFailureSweep) {
    GifFxSet fs;
    ASSERT_TRUE(loadFx(&fs));
    const char *names[] = {"disposal_3_nested", "lzw_m8_transparent_255", "interlace_h9"};
    for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
        const GifFx *x = findFx(&fs, names[k]);
        ASSERT_TRUE(x != NULL);
        bool needsSave = strcmp(names[k], "disposal_3_nested") == 0;
        int total = needsSave ? 3 : 2; /* the object, the canvas, and the disposal-3 buffer */
        for (int failAt = 0; failAt <= total + 1; failAt++) {
            DecCountAlloc ca;
            GfxAllocator al;
            decAllocInit(&ca, &al, failAt);
            GfxGif *g = (GfxGif *)1;
            Status st = gfxGifOpen(x->data, x->dataLen, NULL, &al, &g);
            if (failAt < total) {
                ASSERT_EQ(st, STATUS_ERR_NO_MEMORY);
                ASSERT_TRUE(g == NULL);
            } else {
                ASSERT_EQ(st, STATUS_OK);
                GfxGifFrame f;
                ASSERT_EQ(gfxGifNextFrame(g, &f), STATUS_OK); /* nothing else allocates */
                gfxGifClose(g);
                ASSERT_EQ(ca.count, total);
            }
            ASSERT_EQ(ca.live, 0);
            decAllocInit(&ca, &al, failAt);
            GfxImage img;
            st = gfxGifDecode(x->data, x->dataLen, NULL, &al, &img);
            if (failAt < total) {
                ASSERT_EQ(st, STATUS_ERR_NO_MEMORY);
                ASSERT_TRUE(img.pixels == NULL && img.width == 0);
            } else {
                ASSERT_EQ(st, STATUS_OK);
                gfxImageFree(&img);
            }
            ASSERT_EQ(ca.live, 0);
        }
    }
    freeFx(&fs);
}
