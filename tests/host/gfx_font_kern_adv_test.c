/* Differential and adversarial host tests for libs/gfx's pair kerning (M12.3, D-154), written by
 * the step 3-5 bug sweep (docs/sweeps/M12.3.md). A from-scratch GPOS builder writes random but
 * well-formed script / feature / lookup / PairPos structures from a model; the expected kerning is
 * computed from the model (by construction, never from the bytes), and the GPOS table is the last
 * bytes of an exact-size font so ASan sees any read past it. The random models cover script choice
 * (latn, DFLT, the rest; default LangSys or the first LangSys record; required features),
 * duplicate and out-of-range feature and lookup indices, extension lookups (good and bad), both
 * PairPos formats with random value formats (including YPlacement and device fields), coverage and
 * ClassDef formats 1 and 2, absent ClassDefs, out-of-range classes and pair-set counts. Targeted
 * cases pin the 32-lookup set, the 256-subtable cap, one-past-the-end feature and lookup indices,
 * truncated subtables and coverage tables, and legacy 'kern' subtable lengths. */
#include "font_testutil.h"
#include "framework/test.h"
#include "gfx/gfx-font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- PRNG and a big-endian byte buffer ------------------------------------------------------- */

static uint64_t rs = 1;

static void seed(uint64_t s) {
    rs = s != 0 ? s : 0x9E3779B97F4A7C15ull;
}

static uint32_t rnd(void) {
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return (uint32_t)((rs * 0x2545F4914F6CDD1Dull) >> 32);
}

static uint32_t below(uint32_t n) {
    return n != 0 ? rnd() % n : 0;
}

typedef struct {
    uint8_t *d;
    size_t n, cap;
} Buf;

static void put8(Buf *b, uint32_t v) {
    if (b->n + 1 > b->cap) {
        b->cap = b->cap != 0 ? b->cap * 2 : 256;
        b->d = realloc(b->d, b->cap);
    }
    b->d[b->n++] = (uint8_t)v;
}

static void put16(Buf *b, uint32_t v) {
    put8(b, v >> 8);
    put8(b, v);
}

static void put32(Buf *b, uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xFFFFu);
}

static void putTag(Buf *b, const char *t) {
    for (int i = 0; i < 4; i++) {
        put8(b, (uint8_t)t[i]);
    }
}

static void putBuf(Buf *b, const Buf *src) {
    for (size_t i = 0; i < src->n; i++) {
        put8(b, src->d[i]);
    }
}

static void putBytes(Buf *b, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        put8(b, p[i]);
    }
}

static int offOverflow; /* a 16-bit offset did not fit: the builder's own limit, not the code's */

static void patch16(Buf *b, size_t at, size_t v) {
    if (v > 0xFFFFu) {
        offOverflow = 1;
    }
    b->d[at] = (uint8_t)(v >> 8);
    b->d[at + 1] = (uint8_t)v;
}

static void patch32(Buf *b, size_t at, uint32_t v) {
    patch16(b, at, v >> 16);
    patch16(b, at + 2, v & 0xFFFFu);
}

static void bufFree(Buf *b) {
    free(b->d);
    memset(b, 0, sizeof *b);
}

/* ---- the model ------------------------------------------------------------------------------- */

enum { NG = 10, MAXS = 20, MAXF = 8, MAXL = 48, MAXSUB = 3, MAXIDX = 48, MAXC = 5 };

typedef struct {
    int16_t f[8]; /* field k is written when bit k of the value format is set; XAdvance is f[2] */
} Val;

typedef struct {
    uint32_t fmt, vf1, vf2, covFmt;
    uint32_t ext; /* inside a type-9 lookup: 0 good, 1 extension format 2, 2 extension type 7 */
    uint16_t cov[NG];
    uint32_t nCov;
    /* format 1 */
    uint32_t nSets; /* pairSetCount; may be one less or one more than nCov */
    uint16_t g2[NG + 1][NG];
    uint32_t nPair[NG + 1];
    Val pv1[NG + 1][NG], pv2[NG + 1][NG];
    /* format 2 */
    uint32_t cdFmt[2];
    int cdAbsent[2];
    uint16_t cls[2][NG]; /* 0 = unlisted; may equal the class count (out of range) */
    uint32_t n1, n2;
    Val m1[MAXC][MAXC], m2[MAXC][MAXC];
} MSub;

typedef struct {
    uint32_t type, flag, nSub;
    uint32_t repeat; /* type 2 only: nonzero = that many subtable offsets, all to sub[0] */
    MSub sub[MAXSUB];
    const uint8_t *raw; /* type 2 only: one raw subtable, written as the very last GPOS bytes */
    size_t rawLen;
} MLookup;

typedef struct {
    uint32_t req, n;
    uint16_t idx[MAXIDX];
} MLangSys;

typedef struct {
    const char *tag;
    int hasDef;
    MLangSys def;
    uint32_t nLs;
    MLangSys ls[2];
} MScript;

typedef struct {
    const char *tag;
    uint32_t n;
    uint16_t idx[MAXIDX];
} MFeature;

typedef struct {
    uint32_t major, minor;
    uint32_t nS, nF, nL;
    MScript s[MAXS];
    MFeature fe[MAXF];
    MLookup lk[MAXL];
    int ghostFeature; /* a 'kern' record (to a feature listing lookup 0) right after the records */
    int ghostLookup;  /* lookup 0's offset repeated right after the lookup offsets */
} Model;

static Model m; /* one shared model (it is large) */

static const char *const scriptTags[] = {"latn", "DFLT", "cyrl", "grek", "arab"};

/* ---- model -> bytes -------------------------------------------------------------------------- */

static void putVal(Buf *b, const Val *v, uint32_t vf) {
    for (int k = 0; k < 8; k++) {
        if ((vf >> k) & 1u) {
            put16(b, (uint16_t)v->f[k]);
        }
    }
}

static void putCoverage(Buf *b, const MSub *s) {
    if (s->covFmt == 1) {
        put16(b, 1);
        put16(b, s->nCov);
        for (uint32_t i = 0; i < s->nCov; i++) {
            put16(b, s->cov[i]);
        }
        return;
    }
    uint32_t runs = 0;
    for (uint32_t i = 0; i < s->nCov; i++) {
        runs += i == 0 || s->cov[i] != s->cov[i - 1] + 1;
    }
    put16(b, 2);
    put16(b, runs);
    for (uint32_t i = 0; i < s->nCov;) {
        uint32_t j = i;
        while (j + 1 < s->nCov && s->cov[j + 1] == s->cov[j] + 1) {
            j++;
        }
        put16(b, s->cov[i]);
        put16(b, s->cov[j]);
        put16(b, i); /* startCoverageIndex */
        i = j + 1;
    }
}

static void putClassDef(Buf *b, const MSub *s, int k) {
    int lo = -1, hi = -1;
    for (int g = 0; g < NG; g++) {
        if (s->cls[k][g] != 0) {
            lo = lo < 0 ? g : lo;
            hi = g;
        }
    }
    if (s->cdFmt[k] == 1) {
        put16(b, 1);
        put16(b, lo < 0 ? 0 : (uint32_t)lo);
        put16(b, lo < 0 ? 0 : (uint32_t)(hi - lo + 1));
        for (int g = lo; g >= 0 && g <= hi; g++) {
            put16(b, s->cls[k][g]);
        }
        return;
    }
    Buf r = {0};
    uint32_t n = 0;
    for (int g = 0; g < NG;) {
        if (s->cls[k][g] == 0) {
            g++;
            continue;
        }
        int e = g;
        while (e + 1 < NG && s->cls[k][e + 1] == s->cls[k][g]) {
            e++;
        }
        put16(&r, (uint32_t)g);
        put16(&r, (uint32_t)e);
        put16(&r, s->cls[k][g]);
        n++;
        g = e + 1;
    }
    put16(b, 2);
    put16(b, n);
    putBuf(b, &r);
    bufFree(&r);
}

static void putPairPos(Buf *b, const MSub *s) {
    const size_t at = b->n;
    if (s->fmt == 1) {
        put16(b, 1);
        put16(b, 0); /* coverage, patched */
        put16(b, s->vf1);
        put16(b, s->vf2);
        put16(b, s->nSets);
        for (uint32_t i = 0; i < s->nSets; i++) {
            put16(b, 0);
        }
        for (uint32_t i = 0; i < s->nSets; i++) {
            patch16(b, at + 10 + 2 * i, b->n - at);
            put16(b, s->nPair[i]);
            for (uint32_t j = 0; j < s->nPair[i]; j++) {
                put16(b, s->g2[i][j]);
                putVal(b, &s->pv1[i][j], s->vf1);
                putVal(b, &s->pv2[i][j], s->vf2);
            }
        }
        patch16(b, at + 2, b->n - at);
        putCoverage(b, s);
        return;
    }
    put16(b, 2);
    put16(b, 0); /* coverage, patched */
    put16(b, s->vf1);
    put16(b, s->vf2);
    put16(b, 0); /* ClassDef1, patched unless absent */
    put16(b, 0); /* ClassDef2 */
    put16(b, s->n1);
    put16(b, s->n2);
    for (uint32_t c1 = 0; c1 < s->n1; c1++) {
        for (uint32_t c2 = 0; c2 < s->n2; c2++) {
            putVal(b, &s->m1[c1][c2], s->vf1);
            putVal(b, &s->m2[c1][c2], s->vf2);
        }
    }
    patch16(b, at + 2, b->n - at);
    putCoverage(b, s);
    for (int k = 0; k < 2; k++) {
        if (!s->cdAbsent[k]) {
            patch16(b, at + 8 + 2 * (size_t)k, b->n - at);
            putClassDef(b, s, k);
        }
    }
}

static void putLookup(Buf *b, const MLookup *l) {
    const size_t at = b->n;
    const uint32_t nOff = l->raw != NULL ? 1 : l->repeat != 0 ? l->repeat : l->nSub;
    put16(b, l->type);
    put16(b, l->flag);
    put16(b, nOff);
    for (uint32_t k = 0; k < nOff; k++) {
        put16(b, 0);
    }
    if (l->type != 2 && l->type != 9) { /* some other lookup type: 12 junk bytes */
        for (uint32_t k = 0; k < nOff; k++) {
            patch16(b, at + 6 + 2 * k, b->n - at);
        }
        for (int i = 0; i < 12; i++) {
            put8(b, 0);
        }
        return;
    }
    if (l->raw != NULL) {
        patch16(b, at + 6, b->n - at);
        putBytes(b, l->raw, l->rawLen);
        return;
    }
    if (l->type == 2) {
        for (uint32_t k = 0; k < nOff; k++) {
            if (l->repeat == 0 || k == 0) {
                patch16(b, at + 6 + 2 * k, b->n - at);
                putPairPos(b, &l->sub[k]);
            } else {
                patch16(b, at + 6 + 2 * k, (size_t)ftuGet16(b->d, (uint32_t)(at + 6)));
            }
        }
        return;
    }
    size_t ext[MAXSUB];
    for (uint32_t k = 0; k < l->nSub; k++) {
        ext[k] = b->n;
        patch16(b, at + 6 + 2 * k, b->n - at);
        put16(b, l->sub[k].ext == 1 ? 2 : 1);
        put16(b, l->sub[k].ext == 2 ? 7 : 2);
        put32(b, 0);
    }
    for (uint32_t k = 0; k < l->nSub; k++) {
        patch32(b, ext[k] + 4, (uint32_t)(b->n - ext[k]));
        putPairPos(b, &l->sub[k]);
    }
}

static void putLangSys(Buf *b, const MLangSys *ls) {
    put16(b, 0);
    put16(b, ls->req);
    put16(b, ls->n);
    for (uint32_t i = 0; i < ls->n; i++) {
        put16(b, ls->idx[i]);
    }
}

/* The GPOS table of `m` (NULL when a 16-bit offset would overflow). */
static uint8_t *buildGpos(const Model *m, size_t *len) {
    offOverflow = 0;
    Buf sl = {0}, fl = {0}, ll = {0}, g = {0};
    /* ScriptList */
    put16(&sl, m->nS);
    for (uint32_t i = 0; i < m->nS; i++) {
        putTag(&sl, m->s[i].tag);
        put16(&sl, 0);
    }
    for (uint32_t i = 0; i < m->nS; i++) {
        const MScript *s = &m->s[i];
        const size_t at = sl.n;
        patch16(&sl, 2 + 6 * i + 4, at);
        put16(&sl, 0);
        put16(&sl, s->nLs);
        for (uint32_t k = 0; k < s->nLs; k++) {
            putTag(&sl, k == 0 ? "ENG " : "TRK ");
            put16(&sl, 0);
        }
        if (s->hasDef) {
            patch16(&sl, at, sl.n - at);
            putLangSys(&sl, &s->def);
        }
        for (uint32_t k = 0; k < s->nLs; k++) {
            patch16(&sl, at + 4 + 6 * k + 4, sl.n - at);
            putLangSys(&sl, &s->ls[k]);
        }
    }
    /* FeatureList (plus the ghost record) */
    put16(&fl, m->nF);
    const uint32_t nRec = m->nF + (m->ghostFeature ? 1u : 0u);
    for (uint32_t i = 0; i < nRec; i++) {
        putTag(&fl, i < m->nF ? m->fe[i].tag : "kern");
        put16(&fl, 0);
    }
    for (uint32_t i = 0; i < nRec; i++) {
        patch16(&fl, 2 + 6 * i + 4, fl.n);
        put16(&fl, 0);
        if (i < m->nF) {
            put16(&fl, m->fe[i].n);
            for (uint32_t k = 0; k < m->fe[i].n; k++) {
                put16(&fl, m->fe[i].idx[k]);
            }
        } else {
            put16(&fl, 1);
            put16(&fl, 0); /* the ghost feature lists lookup 0 */
        }
    }
    /* LookupList (plus the ghost offset) */
    put16(&ll, m->nL);
    const uint32_t nOff = m->nL + (m->ghostLookup ? 1u : 0u);
    for (uint32_t i = 0; i < nOff; i++) {
        put16(&ll, 0);
    }
    for (uint32_t i = 0; i < m->nL; i++) {
        patch16(&ll, 2 + 2 * i, ll.n);
        putLookup(&ll, &m->lk[i]);
    }
    if (m->ghostLookup) {
        patch16(&ll, 2 + 2 * (size_t)m->nL, ftuGet16(ll.d, 2));
    }
    put16(&g, m->major);
    put16(&g, m->minor);
    put16(&g, 10);
    put16(&g, 0);
    put16(&g, 0);
    putBuf(&g, &sl);
    patch16(&g, 6, g.n);
    putBuf(&g, &fl);
    patch16(&g, 8, g.n);
    putBuf(&g, &ll);
    bufFree(&sl);
    bufFree(&fl);
    bufFree(&ll);
    if (offOverflow) {
        bufFree(&g);
        return NULL;
    }
    *len = g.n;
    return g.d;
}

/* synth-gpos.ttf (glyphs .notdef A V T o W a Y, a 'kern' table with 1-2 = +500) with its GPOS
 * replaced by `gpos`, laid out last and exact. */
static uint8_t *fontWithGpos(const uint8_t *gpos, size_t len, size_t *n) {
    const FtuTable ov[] = {{"GPOS", gpos, (uint32_t)len}};
    return ftuRebuild(ftuFont(FTU_SYNTH_GPOS, NULL), ov, 1, n);
}

/* ---- the model's own answer (from the model, not the bytes) ---------------------------------- */

static int subValid(const MSub *s, uint32_t type) {
    return ((s->vf1 | s->vf2) & 0xFF00u) == 0 && (type != 9 || s->ext == 0);
}

static int subMatch(const MSub *s, uint32_t l, uint32_t r, int32_t *adv) {
    int ci = -1;
    for (uint32_t i = 0; i < s->nCov; i++) {
        if (s->cov[i] == l) {
            ci = (int)i;
        }
    }
    if (ci < 0) {
        return 0;
    }
    if (s->fmt == 1) {
        if ((uint32_t)ci >= s->nSets) {
            return 0;
        }
        for (uint32_t j = 0; j < s->nPair[ci]; j++) {
            if (s->g2[ci][j] == r) {
                *adv = (s->vf1 & 4u) != 0 ? s->pv1[ci][j].f[2] : 0;
                return 1;
            }
        }
        return 0;
    }
    const uint32_t c1 = s->cdAbsent[0] || l >= NG ? 0 : s->cls[0][l];
    const uint32_t c2 = s->cdAbsent[1] || r >= NG ? 0 : s->cls[1][r];
    if (c1 >= s->n1 || c2 >= s->n2) {
        return 0;
    }
    *adv = (s->vf1 & 4u) != 0 ? s->m1[c1][c2].f[2] : 0;
    return 1;
}

typedef struct {
    uint32_t n;
    uint16_t lookup[256];
    const MSub *sub[256];
} SubList;

/* D-154: latn, else DFLT, else the other records in order (16 at most); the default LangSys, else
 * the first LangSys record; required feature then the listed ones; 'kern' features only; lookup
 * indices < lookupCount; a set of at most 32 distinct lookups (later ones are dropped once full).
 */
static void expectedSubtables(const Model *m, SubList *out) {
    out->n = 0;
    uint32_t cand[16], nCand = 0;
    for (int p = 0; p < 2; p++) {
        for (uint32_t i = 0; i < m->nS; i++) {
            if (strcmp(m->s[i].tag, p == 0 ? "latn" : "DFLT") == 0) {
                cand[nCand++] = i;
                break;
            }
        }
    }
    for (uint32_t i = 0; i < m->nS && nCand < 16; i++) {
        int seen = 0;
        for (uint32_t k = 0; k < nCand; k++) {
            seen |= cand[k] == i;
        }
        if (!seen) {
            cand[nCand++] = i;
        }
    }
    static int inSet[65536];
    memset(inSet, 0, sizeof inSet);
    uint32_t count = 0;
    for (uint32_t k = 0; k < nCand && count == 0; k++) {
        const MScript *s = &m->s[cand[k]];
        const MLangSys *ls = s->hasDef ? &s->def : s->nLs != 0 ? &s->ls[0] : NULL;
        if (ls == NULL) {
            continue;
        }
        uint32_t feats[MAXIDX + 1], nFeat = 0;
        if (ls->req != 0xFFFFu) {
            feats[nFeat++] = ls->req;
        }
        for (uint32_t i = 0; i < ls->n; i++) {
            feats[nFeat++] = ls->idx[i];
        }
        for (uint32_t i = 0; i < nFeat; i++) {
            if (feats[i] >= m->nF || strcmp(m->fe[feats[i]].tag, "kern") != 0) {
                continue;
            }
            const MFeature *fe = &m->fe[feats[i]];
            for (uint32_t j = 0; j < fe->n; j++) {
                const uint32_t li = fe->idx[j];
                if (li < m->nL && !inSet[li] && count < 32) {
                    inSet[li] = 1;
                    count++;
                }
            }
        }
    }
    for (uint32_t li = 0; li < m->nL; li++) {
        if (!inSet[li]) {
            continue;
        }
        const MLookup *l = &m->lk[li];
        if (l->type != 2 && l->type != 9) {
            continue;
        }
        const uint32_t nOff = l->repeat != 0 ? l->repeat : l->nSub;
        for (uint32_t k = 0; k < nOff; k++) {
            const MSub *s = &l->sub[l->repeat != 0 ? 0 : k];
            if (subValid(s, l->type) && out->n < 256) {
                out->lookup[out->n] = (uint16_t)li;
                out->sub[out->n] = s;
                out->n++;
            }
        }
    }
}

static int32_t expectedKern(const SubList *sl, uint32_t l, uint32_t r) {
    int32_t total = 0;
    for (uint32_t i = 0; i < sl->n; i++) {
        int32_t adv;
        if (subMatch(sl->sub[i], l, r, &adv)) {
            total += adv;
            /* the first matching subtable ends this lookup: skip its other subtables */
            const uint16_t lk = sl->lookup[i];
            while (i + 1 < sl->n && sl->lookup[i + 1] == lk) {
                i++;
            }
        }
    }
    return total;
}

/* Builds `m` into a font and compares every pair of glyphs 0..NG+1 (and 0xFFFF) with the model.
 * *encoded is 0 when the builder could not encode the model (a 16-bit offset overflowed). */
static void checkModel(const Model *m, const char *what, int *encoded) {
    size_t glen, n;
    *encoded = 0;
    uint8_t *gpos = buildGpos(m, &glen);
    if (gpos == NULL) {
        return;
    }
    *encoded = 1;
    uint8_t *font = fontWithGpos(gpos, glen, &n);
    free(gpos);
    GfxFont f;
    const Status st = gfxFontInit(&f, font, n);
    ASSERT_EQ(st, STATUS_OK);
    SubList sl;
    expectedSubtables(m, &sl);
    const int failuresBefore = hostTestFailures;
    ASSERT_EQ(f.nKernSub, sl.n);
    ASSERT_EQ((int)gfxFontKernSource(&f),
              (int)(sl.n != 0 ? GFX_FONT_KERN_GPOS : GFX_FONT_KERN_TABLE));
    for (uint32_t l = 0; l < NG + 2 && hostTestFailures == failuresBefore; l++) {
        for (uint32_t r = 0; r < NG + 2; r++) {
            int32_t want = sl.n != 0 ? expectedKern(&sl, l, r) : l == 1 && r == 2 ? 500 : 0;
            int32_t got = gfxFontKernUnits(&f, (uint16_t)l, (uint16_t)r);
            if (got != want) {
                fprintf(stderr, "  %s: kern(%u, %u) = %d, want %d\n", what, l, r, (int)got,
                        (int)want);
                ASSERT_EQ(got, want);
                break;
            }
        }
    }
    ASSERT_EQ(gfxFontKernUnits(&f, 0xFFFF, 0xFFFF), 0);
    free(font);
}

/* ---- random models --------------------------------------------------------------------------- */

static void randVal(Val *v) {
    for (int k = 0; k < 8; k++) {
        v->f[k] =
            (int16_t)(below(3) == 0 ? (int32_t)below(65536) - 32768 : (int32_t)below(401) - 200);
    }
}

static uint32_t randSortedSubset(uint16_t *out, uint32_t max, uint32_t oneIn) {
    uint32_t n = 0;
    for (uint32_t g = 0; g < NG && n < max; g++) {
        if (below(oneIn) == 0) {
            out[n++] = (uint16_t)g;
        }
    }
    return n;
}

static void randSub(MSub *s, int allowBad) {
    memset(s, 0, sizeof *s);
    s->fmt = 1 + below(2);
    s->vf1 = below(3) == 0 ? 4u : below(256);
    s->vf2 = below(3) == 0 ? 0u : below(256);
    if (allowBad && below(30) == 0) {
        const uint32_t bit = 0x100u << below(8); /* a reserved bit: the subtable is skipped */
        if (below(2)) {
            s->vf1 |= bit;
        } else {
            s->vf2 |= bit;
        }
    }
    s->covFmt = 1 + below(2);
    s->nCov = randSortedSubset(s->cov, NG, 1 + below(4));
    s->ext = allowBad && below(12) == 0 ? 1 + below(2) : 0;
    if (s->fmt == 1) {
        s->nSets = s->nCov;
        if (below(5) == 0) {
            s->nSets = s->nCov > 0 && below(2) ? s->nCov - 1 : s->nCov + 1;
        }
        for (uint32_t i = 0; i < s->nSets; i++) {
            s->nPair[i] = randSortedSubset(s->g2[i], NG, 1 + below(5));
            for (uint32_t j = 0; j < s->nPair[i]; j++) {
                randVal(&s->pv1[i][j]);
                randVal(&s->pv2[i][j]);
            }
        }
        return;
    }
    s->n1 = 1 + below(MAXC - 1);
    s->n2 = 1 + below(MAXC - 1);
    for (int k = 0; k < 2; k++) {
        s->cdFmt[k] = 1 + below(2);
        s->cdAbsent[k] = below(6) == 0;
        const uint32_t n = k == 0 ? s->n1 : s->n2;
        for (int g = 0; g < NG; g++) {
            s->cls[k][g] = (uint16_t)(below(2) ? 0 : below(n + 1)); /* n itself is out of range */
        }
    }
    for (uint32_t c1 = 0; c1 < MAXC; c1++) {
        for (uint32_t c2 = 0; c2 < MAXC; c2++) {
            randVal(&s->m1[c1][c2]);
            randVal(&s->m2[c1][c2]);
        }
    }
}

static void randLangSys(MLangSys *ls, uint32_t nF) {
    ls->req = below(3) == 0 ? below(nF + 1) : 0xFFFFu;
    ls->n = below(4);
    for (uint32_t i = 0; i < ls->n; i++) {
        ls->idx[i] = (uint16_t)below(nF + 1);
    }
}

static void randModel(Model *m) {
    memset(m, 0, sizeof *m);
    m->major = 1;
    m->minor = below(8) == 0 ? 1 : 0;
    m->nL = 1 + below(5);
    static const uint32_t types[] = {2, 2, 2, 9, 9, 4};
    for (uint32_t i = 0; i < m->nL; i++) {
        MLookup *l = &m->lk[i];
        l->type = types[below(6)];
        l->flag = below(2) ? 0 : 8;
        l->nSub = 1 + below(MAXSUB);
        for (uint32_t k = 0; k < l->nSub; k++) {
            randSub(&l->sub[k], 1);
        }
    }
    static const char *const featTags[] = {"kern", "kern", "kern", "liga", "mark"};
    m->nF = 1 + below(5);
    for (uint32_t i = 0; i < m->nF; i++) {
        m->fe[i].tag = featTags[below(5)];
        m->fe[i].n = below(5);
        for (uint32_t k = 0; k < m->fe[i].n; k++) {
            m->fe[i].idx[k] = (uint16_t)below(m->nL + 1); /* nL itself is out of range */
        }
    }
    m->nS = below(5);
    for (uint32_t i = 0; i < m->nS; i++) {
        MScript *s = &m->s[i];
        s->tag = scriptTags[below(5)];
        s->hasDef = below(4) != 0;
        randLangSys(&s->def, m->nF);
        s->nLs = below(3);
        for (uint32_t k = 0; k < s->nLs; k++) {
            randLangSys(&s->ls[k], m->nF);
        }
    }
    m->ghostFeature = below(3) == 0;
    m->ghostLookup = below(3) == 0;
}

TEST(fontKernAdvRandomGposMatchesModel) {
    seed(0x6B65726Eull);
    int checked = 0, withGpos = 0;
    for (int run = 0; run < 3000; run++) {
        randModel(&m);
        char what[32];
        snprintf(what, sizeof what, "model %d", run);
        const int failuresBefore = hostTestFailures;
        int encoded;
        checkModel(&m, what, &encoded);
        if (encoded) {
            checked++;
            SubList sl;
            expectedSubtables(&m, &sl);
            withGpos += sl.n != 0;
        }
        if (hostTestFailures != failuresBefore) {
            return;
        }
    }
    ASSERT_TRUE(checked > 2900);
    ASSERT_TRUE(withGpos > 1000); /* most models do kern through GPOS: the paths were reached */
}

/* ---- targeted models ------------------------------------------------------------------------- */

/* A format-1 subtable: left glyph `l`, right glyph `r`, XAdvance `v`. */
static void pairSub(MSub *s, uint16_t l, uint16_t r, int16_t v) {
    memset(s, 0, sizeof *s);
    s->fmt = 1;
    s->vf1 = 4;
    s->covFmt = 1;
    s->cov[0] = l;
    s->nCov = 1;
    s->nSets = 1;
    s->g2[0][0] = r;
    s->nPair[0] = 1;
    s->pv1[0][0].f[2] = v;
}

/* One DFLT script whose default LangSys lists `feats`. */
static void oneScript(Model *m, const uint16_t *feats, uint32_t n) {
    m->major = 1;
    m->nS = 1;
    m->s[0].tag = "DFLT";
    m->s[0].hasDef = 1;
    m->s[0].def.req = 0xFFFFu;
    m->s[0].def.n = n;
    memcpy(m->s[0].def.idx, feats, sizeof(uint16_t) * n);
}

static int32_t kernOf(const Model *m, uint16_t l, uint16_t r, GfxFontKernSource *src,
                      uint32_t *nSub) {
    size_t glen, n;
    uint8_t *gpos = buildGpos(m, &glen);
    if (gpos == NULL) {
        return -99999;
    }
    uint8_t *font = fontWithGpos(gpos, glen, &n);
    free(gpos);
    GfxFont f;
    int32_t v = -99999;
    if (gfxFontInit(&f, font, n) == STATUS_OK) {
        v = gfxFontKernUnits(&f, l, r);
        if (src != NULL) {
            *src = gfxFontKernSource(&f);
        }
        if (nSub != NULL) {
            *nSub = f.nKernSub;
        }
    }
    free(font);
    return v;
}

TEST(fontKernAdvLookupSetHoldsTheFirst32Distinct) {
    /* 40 kern lookups listed 39, 38, ..., 0: the set fills with 39..8 and then drops 7..0 */
    memset(&m, 0, sizeof m);
    m.nL = 40;
    for (uint32_t i = 0; i < m.nL; i++) {
        m.lk[i].type = 2;
        m.lk[i].nSub = 1;
        pairSub(&m.lk[i].sub[0], 1, 2, (int16_t)(i + 1));
    }
    m.nF = 1;
    m.fe[0].tag = "kern";
    m.fe[0].n = 40;
    for (uint32_t k = 0; k < 40; k++) {
        m.fe[0].idx[k] = (uint16_t)(39 - k);
    }
    const uint16_t feats[] = {0};
    oneScript(&m, feats, 1);
    uint32_t nSub = 0;
    ASSERT_EQ(kernOf(&m, 1, 2, NULL, &nSub), (9 + 40) * 32 / 2); /* lookups 8..39 give 9..40 */
    ASSERT_EQ(nSub, 32u);
    int enc;
    checkModel(&m, "40 lookups", &enc);
    ASSERT_TRUE(enc);
    /* the same lookup listed twice (in two features, and twice in one) is applied once */
    m.nL = 2;
    m.nF = 2;
    m.fe[0].n = 3;
    m.fe[0].idx[0] = 1;
    m.fe[0].idx[1] = 0;
    m.fe[0].idx[2] = 1;
    m.fe[1].tag = "kern";
    m.fe[1].n = 1;
    m.fe[1].idx[0] = 0;
    const uint16_t both[] = {0, 1, 1};
    oneScript(&m, both, 3);
    ASSERT_EQ(kernOf(&m, 1, 2, NULL, &nSub), 1 + 2);
    ASSERT_EQ(nSub, 2u);
}

TEST(fontKernAdvSubtableCapIs256) {
    /* lookup 0: 300 offsets to one valid subtable; lookup 1 (never reached): 3-4 = -7 */
    memset(&m, 0, sizeof m);
    m.nL = 2;
    m.lk[0].type = 2;
    m.lk[0].nSub = 1;
    m.lk[0].repeat = 300;
    pairSub(&m.lk[0].sub[0], 1, 2, -5);
    m.lk[1].type = 2;
    m.lk[1].nSub = 1;
    pairSub(&m.lk[1].sub[0], 3, 4, -7);
    m.nF = 1;
    m.fe[0].tag = "kern";
    m.fe[0].n = 2;
    m.fe[0].idx[0] = 0;
    m.fe[0].idx[1] = 1;
    const uint16_t feats[] = {0};
    oneScript(&m, feats, 1);
    uint32_t nSub = 0;
    ASSERT_EQ(kernOf(&m, 1, 2, NULL, &nSub), -5); /* the first match ends the lookup */
    ASSERT_EQ(nSub, (uint32_t)GFX_FONT_MAX_KERN_SUBTABLES);
    ASSERT_EQ(kernOf(&m, 3, 4, NULL, NULL), 0);
    int enc;
    checkModel(&m, "300 subtables", &enc);
    ASSERT_TRUE(enc);
    /* 255 + 1: lookup 1's subtable is exactly the 256th and still counts */
    m.lk[0].repeat = 255;
    ASSERT_EQ(kernOf(&m, 3, 4, NULL, &nSub), -7);
    ASSERT_EQ(nSub, 256u);
}

TEST(fontKernAdvIndicesOnePastTheEndAreDropped) {
    /* a feature index == featureCount: the 6 bytes after the records spell a 'kern' record */
    memset(&m, 0, sizeof m);
    m.nL = 1;
    m.lk[0].type = 2;
    m.lk[0].nSub = 1;
    pairSub(&m.lk[0].sub[0], 1, 2, -33);
    m.nF = 1;
    m.fe[0].tag = "liga";
    m.fe[0].n = 1;
    m.ghostFeature = 1;
    const uint16_t feats[] = {0, 1};
    oneScript(&m, feats, 2);
    GfxFontKernSource src = GFX_FONT_KERN_GPOS;
    ASSERT_EQ(kernOf(&m, 1, 2, &src, NULL), 500); /* no kern feature: the 'kern' table applies */
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
    m.s[0].def.n = 0;
    m.s[0].def.req = 1; /* the same, as the required feature */
    ASSERT_EQ(kernOf(&m, 1, 2, &src, NULL), 500);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
    m.fe[0].tag = "kern";
    ASSERT_EQ(kernOf(&m, 1, 2, &src, NULL), 500); /* req index 1 is still out of range */
    m.s[0].def.req = 0;                           /* in range: the required feature alone kerns */
    ASSERT_EQ(kernOf(&m, 1, 2, &src, NULL), -33);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_GPOS);

    /* a lookup index == lookupCount: the 2 bytes after the offsets repeat lookup 0's offset */
    memset(&m, 0, sizeof m);
    m.nL = 2;
    for (uint32_t i = 0; i < 2; i++) {
        m.lk[i].type = 2;
        m.lk[i].nSub = 1;
    }
    pairSub(&m.lk[0].sub[0], 1, 2, -33);
    pairSub(&m.lk[1].sub[0], 3, 4, -44);
    m.nF = 1;
    m.fe[0].tag = "kern";
    m.fe[0].n = 2;
    m.fe[0].idx[0] = 1;
    m.fe[0].idx[1] = 2;
    m.ghostLookup = 1;
    const uint16_t f0[] = {0};
    oneScript(&m, f0, 1);
    ASSERT_EQ(kernOf(&m, 3, 4, NULL, NULL), -44);
    ASSERT_EQ(kernOf(&m, 1, 2, NULL, NULL), 0);
    int enc;
    checkModel(&m, "ghost lookup", &enc);
    ASSERT_TRUE(enc);
}

TEST(fontKernAdvScriptChoice) {
    /* scripts: cyrl (kerns 5-6), DFLT (kerns 3-4), latn (kerns 1-2), in that record order */
    memset(&m, 0, sizeof m);
    m.major = 1;
    m.nL = 3;
    for (uint32_t i = 0; i < 3; i++) {
        m.lk[i].type = 2;
        m.lk[i].nSub = 1;
        pairSub(&m.lk[i].sub[0], (uint16_t)(1 + 2 * i), (uint16_t)(2 + 2 * i), (int16_t)(-10 - i));
    }
    m.nF = 3;
    for (uint32_t i = 0; i < 3; i++) {
        m.fe[i].tag = "kern";
        m.fe[i].n = 1;
        m.fe[i].idx[0] = (uint16_t)i;
    }
    static const char *const order[] = {"cyrl", "DFLT", "latn"};
    static const uint32_t featOf[] = {2, 1, 0};
    m.nS = 3;
    for (uint32_t i = 0; i < 3; i++) {
        m.s[i].tag = order[i];
        m.s[i].hasDef = 1;
        m.s[i].def.req = 0xFFFFu;
        m.s[i].def.n = 1;
        m.s[i].def.idx[0] = (uint16_t)featOf[i];
    }
    ASSERT_EQ(kernOf(&m, 1, 2, NULL, NULL), -10); /* latn wins */
    ASSERT_EQ(kernOf(&m, 3, 4, NULL, NULL), 0);
    m.s[2].def.n = 0; /* latn without kern: DFLT */
    ASSERT_EQ(kernOf(&m, 1, 2, NULL, NULL), 0);
    ASSERT_EQ(kernOf(&m, 3, 4, NULL, NULL), -11);
    m.s[1].hasDef = 0; /* DFLT without a default LangSys or records: the first other script */
    ASSERT_EQ(kernOf(&m, 5, 6, NULL, NULL), -12);
    m.s[1].nLs = 2; /* no default, but LangSys records: the first record's LangSys */
    m.s[1].ls[0].req = 0xFFFFu;
    m.s[1].ls[0].n = 1;
    m.s[1].ls[0].idx[0] = 1;
    m.s[1].ls[1].req = 0xFFFFu;
    m.s[1].ls[1].n = 1;
    m.s[1].ls[1].idx[0] = 2;
    ASSERT_EQ(kernOf(&m, 3, 4, NULL, NULL), -11);
    ASSERT_EQ(kernOf(&m, 5, 6, NULL, NULL), 0);
    int enc;
    checkModel(&m, "script choice", &enc);
    ASSERT_TRUE(enc);
    /* 20 scripts (neither latn nor DFLT), one of them with kern: only the first 16 records are
     * candidates */
    memset(m.s, 0, sizeof m.s);
    m.nS = 20;
    for (uint32_t i = 0; i < 20; i++) {
        m.s[i].tag = "grek";
        m.s[i].hasDef = 1;
        m.s[i].def.req = 0xFFFFu;
    }
    m.s[15].def.n = 1;
    m.s[15].def.idx[0] = 1;
    ASSERT_EQ(kernOf(&m, 3, 4, NULL, NULL), -11); /* the 16th record is still examined */
    m.s[15].def.n = 0;
    m.s[16].def.n = 1;
    m.s[16].def.idx[0] = 1;
    GfxFontKernSource src;
    ASSERT_EQ(kernOf(&m, 3, 4, &src, NULL), 0); /* the 17th is not */
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
    ASSERT_EQ(kernOf(&m, 1, 2, NULL, NULL), 500);
}

/* A GPOS with one DFLT script, one kern feature and one type-2 lookup whose only subtable is
 * `raw`, the last bytes of the table (and of the file). */
static int32_t rawKern(const uint8_t *raw, size_t len, uint16_t l, uint16_t r,
                       GfxFontKernSource *src) {
    memset(&m, 0, sizeof m);
    m.nL = 1;
    m.lk[0].type = 2;
    m.lk[0].raw = raw;
    m.lk[0].rawLen = len;
    m.nF = 1;
    m.fe[0].tag = "kern";
    m.fe[0].n = 1;
    const uint16_t feats[] = {0};
    oneScript(&m, feats, 1);
    return kernOf(&m, l, r, src, NULL);
}

TEST(fontKernAdvTruncatedSubtablesAtTheTableEnd) {
    GfxFontKernSource src;
    /* baseline: format 1, coverage {1, 5} as the last 8 bytes, 1-2 = -33 */
    static const uint8_t good[] = {0, 1, 0, 20, 0,    4,    0, 0,
                                   0, 2, 0, 14, 0,    14,          /* hdr + offsets */
                                   0, 1, 0, 2,  0xFF, 0xDF,        /* pair set 0 */
                                   0, 1, 0, 2,  0,    1,    0, 5}; /* coverage */
    static uint8_t b[64];
    ASSERT_EQ(rawKern(good, sizeof good, 1, 2, &src), -33);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_GPOS);
    ASSERT_EQ(rawKern(good, sizeof good, 5, 2, &src), -33); /* both sets are the same one */
    /* the coverage claims 3 glyphs, only 2 are there: the coverage table is rejected */
    memcpy(b, good, sizeof good);
    b[23] = 3;
    ASSERT_EQ(rawKern(b, sizeof good, 1, 2, &src), 0);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_GPOS); /* the subtable itself is still valid */
    /* coverage offset 0: the subtable is invalid, so the 'kern' table applies */
    memcpy(b, good, sizeof good);
    b[3] = 0;
    ASSERT_EQ(rawKern(b, sizeof good, 1, 2, &src), 500);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
    /* only 2 of the coverage table's 4 header bytes are inside the table: invalid */
    static const uint8_t cov2[] = {0, 1, 0, 10, 0, 4, 0, 0, 0, 0, 0, 1};
    ASSERT_EQ(rawKern(cov2, sizeof cov2, 1, 2, &src), 500);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
    static const uint8_t cov4[] = {0, 1, 0, 10, 0, 4, 0, 0, 0, 0, 0, 1, 0, 0};
    ASSERT_EQ(rawKern(cov4, sizeof cov4, 1, 2, &src), 0); /* 4 bytes: valid, covers nothing */
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_GPOS);
    /* format 2 needs its 16-byte header: 14 and 15 bytes are invalid, 16 is valid */
    static const uint8_t f2[] = {0, 2, 0, 8, 0, 4, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1};
    ASSERT_EQ(rawKern(f2, 14, 1, 2, &src), 500);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
    ASSERT_EQ(rawKern(f2, 15, 1, 2, &src), 500);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
    ASSERT_EQ(rawKern(f2, 16, 1, 2, &src), 0);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_GPOS);
    /* format 1 needs 10 bytes */
    static const uint8_t f1[] = {0, 1, 0, 6, 0, 4, 0, 0, 0, 0};
    ASSERT_EQ(rawKern(f1, 9, 1, 2, &src), 500);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
    ASSERT_EQ(rawKern(f1, 10, 1, 2, &src), 0);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_GPOS);
    /* a pair set whose records run past the end: no match */
    memcpy(b, good, sizeof good);
    b[15] = 4; /* 4 records of 4 bytes claimed in set 0, from byte 16 of 28 */
    ASSERT_EQ(rawKern(b, sizeof good, 1, 2, &src), 0);
    /* huge class counts with class 0 x class 0 still inside the table */
    static const uint8_t f2rec[] = {
        0, 2, 0, 18, 0, 4, 0, 0,    0,
        0, 0, 0, 0,  1, 0, 2, 0xFF, 0xF0, /* class (0, 0) = -16; (0, 1) would be next */
        0, 1, 0, 1,  0, 1};
    ASSERT_EQ(rawKern(f2rec, sizeof f2rec, 1, 2, &src), -16);
    static uint8_t f2big[sizeof f2rec];
    memcpy(f2big, f2rec, sizeof f2rec);
    f2big[15] = 200; /* class2Count 200: record (0, 0) is still inside */
    ASSERT_EQ(rawKern(f2big, sizeof f2big, 1, 2, &src), -16);
    f2big[13] = 200; /* class1Count 200 too: class 0 x class 0 still inside */
    ASSERT_EQ(rawKern(f2big, sizeof f2big, 1, 2, &src), -16);
}

/* ---- the legacy 'kern' table ----------------------------------------------------------------- */

static int32_t kernTableOf(const uint8_t *kern, size_t len, uint16_t l, uint16_t r,
                           GfxFontKernSource *src) {
    const FtuTable ov[] = {{"kern", kern, (uint32_t)len}};
    size_t n;
    uint8_t *font = ftuRebuild(ftuFont(FTU_SYNTH_FALLBACK, NULL), ov, 1, &n);
    GfxFont f;
    int32_t v = -99999;
    if (gfxFontInit(&f, font, n) == STATUS_OK) {
        v = gfxFontKernUnits(&f, l, r);
        *src = gfxFontKernSource(&f);
    }
    free(font);
    return v;
}

TEST(fontKernAdvKernTableSubtableLengths) {
    GfxFontKernSource src;
    /* Microsoft: the first subtable (format 2) claims length 4, shorter than its 6-byte header:
     * the walk ends there, even though a format-0 subtable could be parsed at offset 8 */
    uint8_t k[28];
    memset(k, 0, sizeof k);
    ftuPut16(k, 2, 2);       /* nTables */
    ftuPut16(k, 6, 4);       /* subtable 1: length 4 */
    ftuPut16(k, 8, 0x0201);  /* format 2, horizontal */
    ftuPut16(k, 10, 26);     /* (at 8) length */
    ftuPut16(k, 12, 0x0001); /* (at 8) format 0, horizontal */
    ftuPut16(k, 14, 1);      /* (at 8) nPairs */
    ftuPut16(k, 22, 1);
    ftuPut16(k, 24, 2);
    ftuPut16(k, 26, (uint16_t)-77);
    ASSERT_EQ(kernTableOf(k, sizeof k, 1, 2, &src), 0);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_NONE);
    /* length 6 (exactly the header): the walk goes on to the next subtable at 10 */
    uint8_t k2[4 + 6 + 14 + 6];
    memset(k2, 0, sizeof k2);
    ftuPut16(k2, 2, 2);
    ftuPut16(k2, 6, 6);
    ftuPut16(k2, 8, 0x0201);
    ftuPut16(k2, 12, 20);
    ftuPut16(k2, 14, 0x0001);
    ftuPut16(k2, 16, 1);
    ftuPut16(k2, 24, 1);
    ftuPut16(k2, 26, 2);
    ftuPut16(k2, 28, (uint16_t)-77);
    ASSERT_EQ(kernTableOf(k2, sizeof k2, 1, 2, &src), -77);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
    /* nTables 1: the second subtable is never looked at */
    ftuPut16(k2, 2, 1);
    ASSERT_EQ(kernTableOf(k2, sizeof k2, 1, 2, &src), 0);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_NONE);
    /* the pair array exactly fills the table, and one byte short drops the last pair */
    ftuPut16(k2, 2, 2);
    ASSERT_EQ(kernTableOf(k2, sizeof k2 - 1, 1, 2, &src), 0);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_NONE);
    /* Apple: length 7 (< its 8-byte header) ends the walk; 8 goes on */
    uint8_t a[8 + 8 + 16 + 6];
    memset(a, 0, sizeof a);
    ftuPut32(a, 0, 0x00010000u);
    ftuPut32(a, 4, 2);
    ftuPut32(a, 8, 7);
    ftuPut16(a, 12, 0x0002); /* format 2 */
    ftuPut32(a, 16, 22);
    ftuPut16(a, 20, 0x0000); /* format 0, horizontal */
    ftuPut16(a, 24, 1);
    ftuPut16(a, 32, 1);
    ftuPut16(a, 34, 2);
    ftuPut16(a, 36, (uint16_t)-55);
    ASSERT_EQ(kernTableOf(a, sizeof a, 1, 2, &src), 0);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_NONE);
    ftuPut32(a, 8, 8);
    ASSERT_EQ(kernTableOf(a, sizeof a, 1, 2, &src), -55);
    ASSERT_EQ((int)src, (int)GFX_FONT_KERN_TABLE);
}
