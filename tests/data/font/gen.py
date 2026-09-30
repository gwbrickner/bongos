#!/usr/bin/env python3
"""Generates the font-engine test data for libs/gfx (M12.3, D-158). Python stdlib only
(ARCHITECTURE §0's host-tool exception); run by hand, the output is checked in and the build never
regenerates it.

Outputs, in this directory:
  synth-fallback.ttf  glyf/composite/cmap-choice/kern-table font, built BY CONSTRUCTION
  synth-gpos.ttf      GPOS kerning (extension, pair formats 1 and 2, coverage/classdef 1+2), plus a
                      'kern' table that must never apply
  synth-grid.ttf      every ASCII glyph a box of advance 500 (space classes empty), CJK 1000
  synth-symbol.ttf    a (3,0) symbol cmap: U+F041 is reachable as U+0041
  synth.oracle        expected values and outlines of the synthetic fonts, by construction
  liberation.oracle   an INDEPENDENT Python parse of data/fonts/*.ttf (sfnt, cmap 4/12, hmtx, glyf
                      incl. composites, GPOS 'kern'): sizes, crc32, metrics, cmap, advances, kerning
                      and expanded outlines, which libs/gfx must reproduce
  utf8.cases          hex input -> expected codepoints, from Python's utf-8 'replace' decoder

The oracle text format is line based (see tests/host/font_testutil.c for the reader):
  <kind> <font> <numbers...>
"""
import os
import random
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = os.path.join(HERE, "..", "..", "..", "data", "fonts")


# ---------------------------------------------------------------- sfnt building ----------------
def u16(v):
    return struct.pack(">H", v & 0xFFFF)


def s16(v):
    return struct.pack(">h", v)


def u32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def pad4(b):
    return b + b"\0" * ((-len(b)) % 4)


def checksum(b):
    b = pad4(b)
    return sum(struct.unpack(">%dI" % (len(b) // 4), b)) & 0xFFFFFFFF


def build_sfnt(tables):
    tags = sorted(tables)
    n = len(tags)
    out = bytearray(struct.pack(">IHHHH", 0x00010000, n, 0, 0, 0))
    off = 12 + 16 * n
    recs = []
    body = bytearray()
    for t in tags:
        data = tables[t]
        recs.append((t, checksum(data), off + len(body), len(data)))
        body += pad4(data)
    for t, cs, o, ln in recs:
        out += t.encode("ascii") + u32(cs) + u32(o) + u32(ln)
    out += body
    # checkSumAdjustment (ignored by the parser under test, but a real font has it right)
    adj = (0xB1B0AFBA - checksum(bytes(out))) & 0xFFFFFFFF
    for t, cs, o, ln in recs:
        if t == "head":
            out[o + 8 : o + 12] = u32(adj)
    return bytes(out)


def head_table(upem, bbox, loca_fmt):
    return (
        u32(0x00010000)
        + u32(0x00010000)
        + u32(0)
        + u32(0x5F0F3CF5)
        + u16(0)
        + u16(upem)
        + b"\0" * 16
        + s16(bbox[0])
        + s16(bbox[1])
        + s16(bbox[2])
        + s16(bbox[3])
        + u16(0)
        + u16(8)
        + s16(2)
        + s16(loca_fmt)
        + s16(0)
    )


def hhea_table(asc, desc, gap, nhm):
    return (
        u32(0x00010000) + s16(asc) + s16(desc) + s16(gap) + u16(1000) + b"\0" * 6 + s16(1)
        + s16(0) + s16(0) + b"\0" * 8 + s16(0) + u16(nhm)
    )


def maxp_table(ng):
    return u32(0x00010000) + u16(ng) + b"\0" * 26


def os2_table(fs_selection, typo_asc, typo_desc, typo_gap):
    b = bytearray(96)
    struct.pack_into(">H", b, 0, 4)
    struct.pack_into(">H", b, 62, fs_selection)
    struct.pack_into(">h", b, 68, typo_asc)
    struct.pack_into(">h", b, 70, typo_desc)
    struct.pack_into(">h", b, 72, typo_gap)
    return bytes(b)


def hmtx_table(advs, nhm):
    out = b""
    for i in range(nhm):
        out += u16(advs[i]) + s16(0)
    for i in range(nhm, len(advs)):
        out += s16(0)
    return out


def loca_glyf(glyphs, long_fmt):
    """glyphs: list of bytes (may repeat the same object to share ranges via tuple ('same', idx))."""
    glyf = b""
    offs = []
    for g in glyphs:
        offs.append(len(glyf))
        glyf += pad4(g) if long_fmt else g + b"\0" * ((-len(g)) % 2)
    offs.append(len(glyf))
    if long_fmt:
        loca = b"".join(u32(o) for o in offs)
    else:
        loca = b"".join(u16(o // 2) for o in offs)
    return loca, glyf


# ---------------------------------------------------------------- glyph encoding ---------------
def simple_glyph(contours):
    """contours: list of list of (x, y, on). Returns the glyf bytes (with short vectors and repeat
    flags, so the loader's every branch is exercised)."""
    pts = [p for c in contours for p in c]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    out = s16(len(contours)) + s16(min(xs)) + s16(min(ys)) + s16(max(xs)) + s16(max(ys))
    end = -1
    for c in contours:
        end += len(c)
        out += u16(end)
    out += u16(0)  # no instructions
    flags = []
    xb = b""
    yb = b""
    px = py = 0
    for (x, y, on) in pts:
        f = 1 if on else 0
        dx, dy = x - px, y - py
        if dx == 0:
            f |= 0x10
        elif -255 <= dx <= 255:
            f |= 0x02 | (0x10 if dx > 0 else 0)
            xb += bytes([abs(dx)])
        else:
            xb += s16(dx)
        if dy == 0:
            f |= 0x20
        elif -255 <= dy <= 255:
            f |= 0x04 | (0x20 if dy > 0 else 0)
            yb += bytes([abs(dy)])
        else:
            yb += s16(dy)
        flags.append(f)
        px, py = x, y
    i = 0
    fb = b""
    while i < len(flags):
        j = i
        while j + 1 < len(flags) and flags[j + 1] == flags[i] and j - i < 255:
            j += 1
        if j > i:
            fb += bytes([flags[i] | 0x08, j - i])
        else:
            fb += bytes([flags[i]])
        i = j + 1
    return out + fb + xb + yb


def f2dot14(v):
    return s16(int(round(v * 16384)))


def composite_glyph(components, bbox=(0, 0, 0, 0)):
    """components: list of dicts: gid, dx, dy, and optional scale (1 value), xy (2 values),
    m2 (4 values a,b,c,d in file order), words, scaled_off, use_my_metrics."""
    out = s16(-1) + b"".join(s16(v) for v in bbox)
    for i, c in enumerate(components):
        fl = 0x0002  # ARGS_ARE_XY_VALUES
        if c.get("words") or not (-128 <= c["dx"] <= 127 and -128 <= c["dy"] <= 127):
            fl |= 0x0001
        if "scale" in c:
            fl |= 0x0008
        elif "xy" in c:
            fl |= 0x0040
        elif "m2" in c:
            fl |= 0x0080
        if c.get("scaled_off"):
            fl |= 0x0800
        if c.get("unscaled_off"):
            fl |= 0x1000
        if c.get("use_my_metrics"):
            fl |= 0x0200
        if i + 1 < len(components):
            fl |= 0x0020
        out += u16(fl) + u16(c["gid"])
        if fl & 1:
            out += s16(c["dx"]) + s16(c["dy"])
        else:
            out += struct.pack(">bb", c["dx"], c["dy"])
        if "scale" in c:
            out += f2dot14(c["scale"])
        elif "xy" in c:
            out += f2dot14(c["xy"][0]) + f2dot14(c["xy"][1])
        elif "m2" in c:
            out += b"".join(f2dot14(v) for v in c["m2"])
    return out


# ---------------------------------------------------------------- cmap building ----------------
def cmap_f4(segs):
    """segs: list of (start, end, kind, arg): kind 'delta' arg=delta, or 'array' arg=list of gids.
    The 0xFFFF terminator is appended."""
    segs = list(segs) + [(0xFFFF, 0xFFFF, "delta", 1)]
    n = len(segs)
    ends = b"".join(u16(s[1]) for s in segs)
    starts = b"".join(u16(s[0]) for s in segs)
    deltas = b""
    ranges = b""
    garr = b""
    for i, (a, b, kind, arg) in enumerate(segs):
        if kind == "delta":
            deltas += u16(arg)
            ranges += u16(0)
        else:
            deltas += u16(0)
            # offset from this idRangeOffset element to the glyph array entry
            ro = (n - i) * 2 + len(garr)
            ranges += u16(ro)
            garr += b"".join(u16(g) for g in arg)
    length = 16 + n * 8 + len(garr)
    return (
        u16(4) + u16(length) + u16(0) + u16(n * 2) + u16(0) + u16(0) + u16(0)
        + ends + u16(0) + starts + deltas + ranges + garr
    )


def cmap_f12(groups):
    body = b"".join(u32(a) + u32(b) + u32(g) for a, b, g in groups)
    return u16(12) + u16(0) + u32(16 + len(body)) + u32(0) + u32(len(groups)) + body


def cmap_table(subs):
    """subs: list of (platform, encoding, bytes)."""
    n = len(subs)
    hdr = u16(0) + u16(n)
    off = 4 + 8 * n
    recs = b""
    body = b""
    for p, e, b in subs:
        recs += u16(p) + u16(e) + u32(off + len(body))
        body += b
    return hdr + recs + body


def ms_kern(pairs):
    pairs = sorted(pairs)
    body = b"".join(u16(l) + u16(r) + s16(v) for l, r, v in pairs)
    length = 14 + len(body)
    return u16(0) + u16(1) + u16(0) + u16(length) + u16(0x0001) + u16(len(pairs)) + u16(0) * 3 + body


# ---------------------------------------------------------------- shapes -----------------------
def box(x0, y0, x1, y1):
    # counter-clockwise on-curve rectangle in font space (y up)
    return [[(x0, y0, 1), (x1, y0, 1), (x1, y1, 1), (x0, y1, 1)]]


def hollow_box(x0, y0, x1, y1, t):
    outer = [(x0, y0, 1), (x1, y0, 1), (x1, y1, 1), (x0, y1, 1)]
    inner = [(x0 + t, y0 + t, 1), (x0 + t, y1 - t, 1), (x1 - t, y1 - t, 1), (x1 - t, y0 + t, 1)]
    return [outer, inner]


def circle(cx, cy, r):
    """A closed contour of 4 OFF-curve points only (an implied-on-curve circle-ish)."""
    return [[(cx + r, cy + r, 0), (cx - r, cy + r, 0), (cx - r, cy - r, 0), (cx + r, cy - r, 0)]]


# ---------------------------------------------------------------- oracle expansion --------------
class Oracle:
    def __init__(self):
        self.lines = []

    def add(self, *parts):
        self.lines.append(" ".join(str(p) for p in parts))

    def outline(self, font, gid, ends, pts):
        self.add("outline", font, gid, len(pts), len(ends))
        self.add("ends", *ends)
        self.add("pts", *[v for p in pts for v in (repr(float(p[0])), repr(float(p[1])), int(p[2]))])

    def write(self, path):
        with open(path, "w") as f:
            f.write("\n".join(self.lines) + "\n")


def compose(parent, child):
    """parent, child: (a, b, c, d, e, f) as x' = a*x + c*y + e, y' = b*x + d*y + f; child applied
    first, then parent (matches M o comp)."""
    pa, pb, pc, pd, pe, pf = parent
    ca, cb, cc, cd, ce, cf = child
    return (
        pa * ca + pc * cb, pb * ca + pd * cb, pa * cc + pc * cd, pb * cc + pd * cd,
        pa * ce + pc * cf + pe, pb * ce + pd * cf + pf,
    )


class SynthGlyph:
    """Glyph definitions kept alongside their bytes so expected outlines come from the definition,
    not from re-parsing the bytes."""

    def __init__(self, contours=None, comps=None):
        self.contours = contours
        self.comps = comps

    def data(self):
        if self.contours is not None:
            return simple_glyph(self.contours) if self.contours else b""
        return composite_glyph(self.comps)

    def expand(self, glyphs, m=(1, 0, 0, 1, 0, 0)):
        if self.contours is not None:
            res = []
            for c in self.contours:
                res.append([(m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5], on) for x, y, on in c])
            return res
        out = []
        for c in self.comps:
            if "scale" in c:
                a, b, cc, d = c["scale"], 0, 0, c["scale"]
            elif "xy" in c:
                a, b, cc, d = c["xy"][0], 0, 0, c["xy"][1]
            elif "m2" in c:
                a, b, cc, d = c["m2"]
            else:
                a, b, cc, d = 1, 0, 0, 1
            dx, dy = c["dx"], c["dy"]
            if c.get("scaled_off") and not c.get("unscaled_off"):
                e, f = a * dx + cc * dy, b * dx + d * dy
            else:
                e, f = dx, dy
            out += glyphs[c["gid"]].expand(glyphs, compose(m, (a, b, cc, d, e, f)))
        return out


def write_oracle_outlines(orc, font, glyphs, gids):
    for gid in gids:
        cs = glyphs[gid].expand(glyphs)
        ends = []
        pts = []
        for c in cs:
            pts += c
            ends.append(len(pts) - 1)
        orc.outline(font, gid, ends, pts)


# ---------------------------------------------------------------- synth-fallback ---------------
def make_fallback(orc):
    G = []
    G.append(SynthGlyph(hollow_box(100, 0, 900, 700, 80)))  # 0 .notdef
    bars = []
    for i in range(16):  # 1..16 -> U+4E00..4E0F: a full-width bar at a different height
        y = 50 * i
        G.append(SynthGlyph(box(0, y, 1000, y + 40)))
    G.append(SynthGlyph(circle(500, 350, 300)))  # 17 U+3002 (all off-curve)
    G.append(SynthGlyph(circle(500, 500, 400)))  # 18 U+E000 large all-off circle
    # 19: composite of 18 scaled 0.5, word args (250,250), SCALED_COMPONENT_OFFSET
    G.append(SynthGlyph(comps=[dict(gid=18, dx=250, dy=250, scale=0.5, words=True, scaled_off=True)]))
    # 20: composite of 19 with a 2x2 rotation [0,1;-1,0], byte args
    G.append(SynthGlyph(comps=[dict(gid=19, dx=10, dy=-20, m2=(0, 1, -1, 0))]))
    # 21: composite of 18 with X_AND_Y_SCALE (0.75, -1.0) and USE_MY_METRICS
    G.append(SynthGlyph(comps=[dict(gid=18, dx=100, dy=900, xy=(0.75, -1.0), use_my_metrics=True)]))
    ng = len(G)
    advs = [1000] * ng
    advs[0] = 1000
    advs[17] = 1000
    advs[18] = 1000
    advs[19] = 1000
    advs[20] = 1000
    advs[21] = 1000
    nhm = ng - 3  # the trailing 3 glyphs reuse the last advance (1000)
    loca, glyf = loca_glyf([g.data() for g in G], False)
    f4 = cmap_f4([(0x3002, 0x3002, "delta", (17 - 0x3002) & 0xFFFF),
                  (0x4E00, 0x4E0F, "delta", (1 - 0x4E00) & 0xFFFF)])
    f12 = cmap_f12([(0x3002, 0x3002, 17), (0x4E00, 0x4E0F, 1), (0xE000, 0xE003, 18)])
    tables = {
        "head": head_table(1000, (0, 0, 1000, 900), 0),
        "hhea": hhea_table(800, -200, 0, nhm),
        "maxp": maxp_table(ng),
        "OS/2": os2_table(0x80, 700, -300, 100),
        "hmtx": hmtx_table(advs, nhm),
        "cmap": cmap_table([(3, 1, f4), (3, 10, f12)]),
        "loca": loca,
        "glyf": glyf,
        "kern": ms_kern([(1, 2, -100)]),
    }
    write_oracle_outlines(orc, "fallback", G, range(ng))
    orc.add("kernpair", "fallback", 1, 2, -100)
    orc.add("metrics", "fallback", 1000, 700, -300, 100, ng, nhm)
    return build_sfnt(tables)


# ---------------------------------------------------------------- synth-gpos -------------------
def coverage_f1(gids):
    gids = sorted(gids)
    return u16(1) + u16(len(gids)) + b"".join(u16(g) for g in gids)


def coverage_f2(ranges):
    out = u16(2) + u16(len(ranges))
    idx = 0
    for a, b in ranges:
        out += u16(a) + u16(b) + u16(idx)
        idx += b - a + 1
    return out


def classdef_f1(start, classes):
    return u16(1) + u16(start) + u16(len(classes)) + b"".join(u16(c) for c in classes)


def classdef_f2(ranges):
    return u16(2) + u16(len(ranges)) + b"".join(u16(a) + u16(b) + u16(c) for a, b, c in ranges)


def make_gpos(orc):
    # glyph order: .notdef A V T o W a Y
    A, V, T, o, W, a, Y = 1, 2, 3, 4, 5, 6, 7
    ng = 8
    # ---- L0: extension -> PairPos format 1, vf1 = XPlacement|XAdvance, vf2 = XAdvance
    def pairset(pairs, vf1, vf2):
        b = u16(len(pairs))
        for g2, v1, v2 in pairs:
            b += u16(g2) + v1 + v2
        return b

    cov0 = coverage_f1([A, T])
    ps_a = pairset([(V, s16(11) + s16(-80), s16(7))], 5, 4)
    ps_t = pairset([(o, s16(0) + s16(-120), s16(0))], 5, 4)
    hdr = 10 + 2 * 2
    cov_off = hdr
    ps_a_off = cov_off + len(cov0)
    ps_t_off = ps_a_off + len(ps_a)
    pp0 = u16(1) + u16(cov_off) + u16(5) + u16(4) + u16(2) + u16(ps_a_off) + u16(ps_t_off) + cov0 + ps_a + ps_t
    ext0 = u16(1) + u16(2) + u32(8) + pp0
    L0 = u16(9) + u16(0) + u16(1) + u16(8) + ext0

    # ---- L1: type 2, two subtables
    cov1 = coverage_f2([(A, T)])  # A, V, T (excludes W)
    cd1 = classdef_f2([(A, A, 1), (T, T, 2)])
    cd2 = classdef_f1(V, [2, 0, 1, 0, 1])  # V=2, T=0, o=1, W=0, a=1
    # class1Count 3 (0,1,2), class2Count 3; value = XAdvance only (vf1 = 4, vf2 = 0)
    vals = {(1, 2): -30, (2, 1): -60, (1, 1): 0}
    recs = b""
    for c1 in range(3):
        for c2 in range(3):
            recs += s16(vals.get((c1, c2), 0))
    off = 16 + len(recs)
    cov1_off = off
    cd1_off = cov1_off + len(cov1)
    cd2_off = cd1_off + len(cd1)
    sub0 = (u16(2) + u16(cov1_off) + u16(4) + u16(0) + u16(cd1_off) + u16(cd2_off) + u16(3) + u16(3)
            + recs + cov1 + cd1 + cd2)
    cov_s1 = coverage_f1([A, W])
    ps_s1a = pairset([(V, s16(-999), b"")], 4, 0)
    ps_s1w = pairset([(a, s16(-40), b"")], 4, 0)
    hdr = 10 + 2 * 2
    sub1 = (u16(1) + u16(hdr) + u16(4) + u16(0) + u16(2) + u16(hdr + len(cov_s1))
            + u16(hdr + len(cov_s1) + len(ps_s1a)) + cov_s1 + ps_s1a + ps_s1w)
    l1_hdr = 6 + 4
    L1 = (u16(2) + u16(0) + u16(2) + u16(l1_hdr) + u16(l1_hdr + len(sub0)) + sub0 + sub1)

    # ---- L2: type 2 format 1, Y-o = -50, vf1 = XAdvance | XPlaDevice (0x14): device offset unused
    cov2 = coverage_f1([Y])
    ps_y = u16(1) + u16(o) + s16(-50) + u16(0)
    hdr = 10 + 2
    sub2 = u16(1) + u16(hdr) + u16(0x14) + u16(0) + u16(1) + u16(hdr + len(cov2)) + cov2 + ps_y
    L2 = u16(2) + u16(0) + u16(1) + u16(8) + sub2

    # ---- L3: type 4 (mark to base): must be ignored
    L3 = u16(4) + u16(0) + u16(1) + u16(8) + b"\0" * 12

    lookups = [L0, L1, L2, L3]
    ll = u16(len(lookups))
    off = 2 + 2 * len(lookups)
    body = b""
    for L in lookups:
        ll += u16(off + len(body))
        body += L
    lookup_list = ll + body

    def feature(idxs):
        return u16(0) + u16(len(idxs)) + b"".join(u16(i) for i in idxs)

    feats = [("kern", feature([2, 0])), ("kern", feature([1])), ("mark", feature([3]))]
    fl = u16(len(feats))
    off = 2 + 6 * len(feats)
    body = b""
    for tag, fb in feats:
        fl += tag.encode() + u16(off + len(body))
        body += fb
    feature_list = fl + body

    langsys = u16(0) + u16(0xFFFF) + u16(3) + u16(0) + u16(1) + u16(2)
    script = u16(4) + u16(0) + langsys
    script_list = u16(1) + b"DFLT" + u16(8) + script
    hdr = 10
    sl_off = hdr
    fl_off = sl_off + len(script_list)
    ll_off = fl_off + len(feature_list)
    gpos = u32(0x00010000) + u16(sl_off) + u16(fl_off) + u16(ll_off) + script_list + feature_list + lookup_list

    advs = [600] * ng
    glyphs = [SynthGlyph(box(0, 0, 500, 700)) if i else SynthGlyph(hollow_box(50, 0, 550, 700, 60))
              for i in range(ng)]
    loca, glyf = loca_glyf([g.data() for g in glyphs], True)
    names = ".notdef A V T o W a Y".split()
    cps = {"A": 0x41, "V": 0x56, "T": 0x54, "o": 0x6F, "W": 0x57, "a": 0x61, "Y": 0x59}
    order = sorted((cps[n], i) for i, n in enumerate(names) if n in cps)
    segs = [(cp, cp, "delta", (g - cp) & 0xFFFF) for cp, g in order]
    tables = {
        "head": head_table(1000, (0, 0, 600, 700), 1),
        "hhea": hhea_table(800, -200, 0, ng),
        "maxp": maxp_table(ng),
        "hmtx": hmtx_table(advs, ng),
        "cmap": cmap_table([(3, 1, cmap_f4(segs))]),
        "loca": loca,
        "glyf": glyf,
        "GPOS": gpos,
        "kern": ms_kern([(A, V, 500)]),
    }
    exp = {(A, V): -110, (T, o): -180, (T, a): -60, (W, a): -40, (Y, o): -50, (A, o): 0}
    for (l, r), v in exp.items():
        orc.add("kernpair", "gpos", l, r, v)
    orc.add("metrics", "gpos", 1000, 800, -200, 0, ng, ng)
    return build_sfnt(tables)


# ---------------------------------------------------------------- synth-grid -------------------
def make_grid(orc):
    # glyph 0 .notdef, 1 = the shared box, 2 = the empty glyph (space-like), 3 = CJK box
    cps_box = [c for c in range(0x21, 0x7F)] + [0x2010, 0x2014, 0x3001, 0x3002, 0xFF08, 0xFF09]
    cps_empty = [0x20, 0xA0]
    cps_cjk = list(range(0x4E00, 0x4E10))
    glyphs = [SynthGlyph(hollow_box(50, 0, 450, 700, 50)), SynthGlyph(box(50, 0, 450, 700)),
              SynthGlyph([]), SynthGlyph(box(100, 0, 900, 800))]
    cp2g = {}
    for c in cps_box:
        cp2g[c] = 1
    for c in cps_empty:
        cp2g[c] = 2
    for c in cps_cjk:
        cp2g[c] = 3
    # Give each codepoint its own glyph id (advance differs by class); glyph i shares the box data.
    order = sorted(cp2g)
    gid_of = {}
    data = [glyphs[0].data()]
    advs = [500]
    for c in order:
        gid_of[c] = len(data)
        data.append(glyphs[cp2g[c]].data())
        advs.append(1000 if c in cps_cjk else 500)
    # cmap format 4: ASCII via glyphIdArray (idRangeOffset), the rest via delta segments
    ascii_cps = [c for c in order if c < 0x80]
    segs = [(0x20, 0x7E, "array", [gid_of.get(c, 0) for c in range(0x20, 0x7F)])]
    for c in order:
        if c >= 0x80:
            segs.append((c, c, "delta", (gid_of[c] - c) & 0xFFFF))
    ng = len(data)
    loca, glyf = loca_glyf(data, False)
    tables = {
        "head": head_table(1000, (0, 0, 1000, 800), 0),
        "hhea": hhea_table(800, -200, 0, ng),
        "maxp": maxp_table(ng),
        "hmtx": hmtx_table(advs, ng),
        "cmap": cmap_table([(3, 1, cmap_f4(segs))]),
        "loca": loca,
        "glyf": glyf,
    }
    orc.add("metrics", "grid", 1000, 800, -200, 0, ng, ng)
    for c in order:
        orc.add("cmap", "grid", c, gid_of[c])
        orc.add("adv", "grid", gid_of[c], advs[gid_of[c]])
    return build_sfnt(tables)


# ---------------------------------------------------------------- synth-symbol -----------------
def make_symbol(orc):
    """A (3,0) symbol cmap: the codepoints live at U+F0xx and must also be found at U+00xx."""
    glyphs = [SynthGlyph(hollow_box(50, 0, 450, 700, 50)), SynthGlyph(box(50, 0, 450, 700))]
    loca, glyf = loca_glyf([g.data() for g in glyphs], False)
    tables = {
        "head": head_table(1000, (0, 0, 500, 700), 0),
        "hhea": hhea_table(800, -200, 0, 2),
        "maxp": maxp_table(2),
        "hmtx": hmtx_table([500, 500], 2),
        "cmap": cmap_table([(3, 0, cmap_f4([(0xF041, 0xF041, "delta", (1 - 0xF041) & 0xFFFF)]))]),
        "loca": loca,
        "glyf": glyf,
    }
    return build_sfnt(tables)


# ---------------------------------------------------------------- Liberation oracle ------------
class PyFont:
    """A from-scratch reader written straight from the OpenType spec, independent of libs/gfx."""

    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        n = struct.unpack(">H", d[4:6])[0]
        self.t = {}
        for i in range(n):
            tag, cs, off, ln = struct.unpack(">4sIII", d[12 + 16 * i : 28 + 16 * i])
            self.t[tag.decode("latin-1")] = (off, ln)
        h = self.t["head"][0]
        self.upem = struct.unpack(">H", d[h + 18 : h + 20])[0]
        self.locfmt = struct.unpack(">h", d[h + 50 : h + 52])[0]
        self.ng = struct.unpack(">H", d[self.t["maxp"][0] + 4 : self.t["maxp"][0] + 6])[0]
        hh = self.t["hhea"][0]
        self.asc, self.desc, self.gap = struct.unpack(">hhh", d[hh + 4 : hh + 10])
        self.nhm = struct.unpack(">H", d[hh + 34 : hh + 36])[0]
        o2 = self.t.get("OS/2")
        self.typo = None
        self.use_typo = False
        if o2 and o2[1] >= 78:
            fs = struct.unpack(">H", d[o2[0] + 62 : o2[0] + 64])[0]
            self.use_typo = bool(fs & 0x80)
            self.typo = struct.unpack(">hhh", d[o2[0] + 68 : o2[0] + 74])
        self.cmap = self._read_cmap()

    def metrics(self):
        if self.use_typo:
            a, dd, g = self.typo
        elif self.asc or self.desc:
            a, dd, g = self.asc, self.desc, self.gap
        elif self.typo:
            a, dd, g = self.typo
        else:
            a, dd, g = 0, 0, 0
        return max(a, 0), -abs(dd) if dd > 0 else dd, max(g, 0)

    def _read_cmap(self):
        d = self.d
        base = self.t["cmap"][0]
        n = struct.unpack(">H", d[base + 2 : base + 4])[0]
        best = None
        for i in range(n):
            p, e, o = struct.unpack(">HHI", d[base + 4 + 8 * i : base + 12 + 8 * i])
            fmt = struct.unpack(">H", d[base + o : base + o + 2])[0]
            rank = 0
            if fmt == 12 and (p, e) == (3, 10):
                rank = 5
            elif fmt == 12 and p == 0 and e in (4, 6):
                rank = 4
            elif fmt == 4 and (p, e) == (3, 1):
                rank = 3
            elif fmt == 4 and p == 0 and e <= 3:
                rank = 2
            elif fmt == 4 and (p, e) == (3, 0):
                rank = 1
            if rank and (best is None or rank > best[0]):
                best = (rank, base + o, fmt)
        _, sub, fmt = best
        m = {}
        if fmt == 12:
            ng = struct.unpack(">I", d[sub + 12 : sub + 16])[0]
            for i in range(ng):
                a, b, g = struct.unpack(">III", d[sub + 16 + 12 * i : sub + 28 + 12 * i])
                for c in range(a, b + 1):
                    m[c] = g + (c - a)
        else:
            segx2 = struct.unpack(">H", d[sub + 6 : sub + 8])[0]
            sc = segx2 // 2
            ends = struct.unpack(">%dH" % sc, d[sub + 14 : sub + 14 + segx2])
            st_off = sub + 16 + segx2
            starts = struct.unpack(">%dH" % sc, d[st_off : st_off + segx2])
            deltas = struct.unpack(">%dH" % sc, d[st_off + segx2 : st_off + 2 * segx2])
            ro_off = st_off + 2 * segx2
            ranges = struct.unpack(">%dH" % sc, d[ro_off : ro_off + segx2])
            for i in range(sc):
                for c in range(starts[i], ends[i] + 1):
                    if c == 0xFFFF:
                        continue
                    if ranges[i] == 0:
                        g = (c + deltas[i]) & 0xFFFF
                    else:
                        a = ro_off + 2 * i + ranges[i] + 2 * (c - starts[i])
                        g = struct.unpack(">H", d[a : a + 2])[0]
                        if g:
                            g = (g + deltas[i]) & 0xFFFF
                    if g:
                        m[c] = g
        return {c: g for c, g in m.items() if g < self.ng}

    def adv(self, g):
        h = self.t["hmtx"][0]
        i = min(g, self.nhm - 1)
        return struct.unpack(">H", self.d[h + 4 * i : h + 4 * i + 2])[0]

    def glyph_bytes(self, g):
        d = self.d
        lo = self.t["loca"][0]
        if self.locfmt:
            a, b = struct.unpack(">II", d[lo + 4 * g : lo + 4 * g + 8])
        else:
            a, b = struct.unpack(">HH", d[lo + 2 * g : lo + 2 * g + 4])
            a, b = a * 2, b * 2
        go = self.t["glyf"][0]
        return d[go + a : go + b]

    def outline(self, g, m=(1, 0, 0, 1, 0, 0)):
        b = self.glyph_bytes(g)
        if not b:
            return []
        nc = struct.unpack(">h", b[:2])[0]
        if nc >= 0:
            return self._simple(b, nc, m)
        out = []
        p = 10
        while True:
            fl, gid = struct.unpack(">HH", b[p : p + 4])
            p += 4
            if fl & 1:
                dx, dy = struct.unpack(">hh", b[p : p + 4])
                p += 4
            else:
                dx, dy = struct.unpack(">bb", b[p : p + 2])
                p += 2
            assert fl & 2, "point matching not expected in the shipped fonts"
            a = dd = 1.0
            bb = c = 0.0
            if fl & 8:
                a = dd = struct.unpack(">h", b[p : p + 2])[0] / 16384
                p += 2
            elif fl & 0x40:
                a, dd = [v / 16384 for v in struct.unpack(">hh", b[p : p + 4])]
                p += 4
            elif fl & 0x80:
                a, bb, c, dd = [v / 16384 for v in struct.unpack(">hhhh", b[p : p + 8])]
                p += 8
            if (fl & 0x800) and not (fl & 0x1000):
                e, f = a * dx + c * dy, bb * dx + dd * dy
            else:
                e, f = dx, dy
            out += self.outline(gid, compose(m, (a, bb, c, dd, e, f)))
            if not fl & 0x20:
                break
        return out

    def _simple(self, b, nc, m):
        ends = struct.unpack(">%dH" % nc, b[10 : 10 + 2 * nc])
        p = 10 + 2 * nc
        il = struct.unpack(">H", b[p : p + 2])[0]
        p += 2 + il
        npts = ends[-1] + 1
        flags = []
        while len(flags) < npts:
            f = b[p]
            p += 1
            flags.append(f)
            if f & 8:
                r = b[p]
                p += 1
                flags += [f] * r
        xs = []
        v = 0
        for f in flags:
            if f & 2:
                dx = b[p]
                p += 1
                v += dx if f & 0x10 else -dx
            elif not f & 0x10:
                v += struct.unpack(">h", b[p : p + 2])[0]
                p += 2
            xs.append(v)
        ys = []
        v = 0
        for f in flags:
            if f & 4:
                dy = b[p]
                p += 1
                v += dy if f & 0x20 else -dy
            elif not f & 0x20:
                v += struct.unpack(">h", b[p : p + 2])[0]
                p += 2
            ys.append(v)
        contours = []
        s = 0
        for e in ends:
            contours.append([(m[0] * xs[i] + m[2] * ys[i] + m[4], m[1] * xs[i] + m[3] * ys[i] + m[5],
                              flags[i] & 1) for i in range(s, e + 1)])
            s = e + 1
        return contours

    # ---- GPOS 'kern' (the subset of D-154), written from the spec
    def gpos_kern_fn(self):
        if "GPOS" not in self.t:
            return None
        d = self.d
        g = self.t["GPOS"][0]
        sl, fl, ll = struct.unpack(">HHH", d[g + 4 : g + 10])
        sl += g
        fl += g
        ll += g
        nsc = struct.unpack(">H", d[sl : sl + 2])[0]
        scripts = []
        for i in range(nsc):
            tag, off = struct.unpack(">4sH", d[sl + 2 + 6 * i : sl + 8 + 6 * i])
            scripts.append((tag, sl + off))
        order = [s for s in scripts if s[0] == b"latn"] + [s for s in scripts if s[0] == b"DFLT"]
        order += [s for s in scripts if s not in order]
        nfeat = struct.unpack(">H", d[fl : fl + 2])[0]
        feats = []
        for i in range(nfeat):
            tag, off = struct.unpack(">4sH", d[fl + 2 + 6 * i : fl + 8 + 6 * i])
            feats.append((tag, fl + off))
        lookups = set()
        for tag, sc in order[:16]:
            dls, nls = struct.unpack(">HH", d[sc : sc + 4])
            if dls:
                ls = sc + dls
            elif nls:
                ls = sc + struct.unpack(">H", d[sc + 8 : sc + 10])[0]
            else:
                continue
            req, cnt = struct.unpack(">HH", d[ls + 2 : ls + 6])
            idxs = list(struct.unpack(">%dH" % cnt, d[ls + 6 : ls + 6 + 2 * cnt]))[:256]
            if req != 0xFFFF:
                idxs = [req] + idxs
            for fi in idxs:
                if fi < len(feats) and feats[fi][0] == b"kern":
                    fo = feats[fi][1]
                    lc = struct.unpack(">H", d[fo + 2 : fo + 4])[0]
                    li = struct.unpack(">%dH" % lc, d[fo + 4 : fo + 4 + 2 * lc])
                    lookups.update(li[:256])
            if lookups:
                break
        nl = struct.unpack(">H", d[ll : ll + 2])[0]
        lookups = sorted(x for x in lookups if x < nl)
        subs_by_lookup = []
        for li in lookups:
            lo = ll + struct.unpack(">H", d[ll + 2 + 2 * li : ll + 4 + 2 * li])[0]
            typ, flag, sc = struct.unpack(">HHH", d[lo : lo + 6])
            subs = []
            for k in range(sc):
                so = lo + struct.unpack(">H", d[lo + 6 + 2 * k : lo + 8 + 2 * k])[0]
                t2 = typ
                if typ == 9:
                    fmt, et, eo = struct.unpack(">HHI", d[so : so + 8])
                    if fmt != 1 or et != 2:
                        continue
                    so, t2 = so + eo, 2
                if t2 == 2:
                    subs.append(so)
            if typ in (2, 9):
                subs_by_lookup.append(subs)

        def cov(o, gid):
            fmt = struct.unpack(">H", d[o : o + 2])[0]
            if fmt == 1:
                n = struct.unpack(">H", d[o + 2 : o + 4])[0]
                arr = struct.unpack(">%dH" % n, d[o + 4 : o + 4 + 2 * n])
                return arr.index(gid) if gid in arr else None
            if fmt == 2:
                n = struct.unpack(">H", d[o + 2 : o + 4])[0]
                for i in range(n):
                    a, b, s = struct.unpack(">HHH", d[o + 4 + 6 * i : o + 10 + 6 * i])
                    if a <= gid <= b:
                        return s + gid - a
            return None

        def cls(o, gid):
            if o == 0:
                return 0
            fmt = struct.unpack(">H", d[o : o + 2])[0]
            if fmt == 1:
                sg, n = struct.unpack(">HH", d[o + 2 : o + 6])
                if sg <= gid < sg + n:
                    return struct.unpack(">H", d[o + 6 + 2 * (gid - sg) : o + 8 + 2 * (gid - sg)])[0]
                return 0
            n = struct.unpack(">H", d[o + 2 : o + 4])[0]
            for i in range(n):
                a, b, c = struct.unpack(">HHH", d[o + 4 + 6 * i : o + 10 + 6 * i])
                if a <= gid <= b:
                    return c
            return 0

        def vsize(vf):
            return 2 * bin(vf & 0xFF).count("1")

        def xadv(vf, o):
            return struct.unpack(">h", d[o + 2 * bin(vf & 3).count("1") : o + 2 * bin(vf & 3).count("1") + 2])[0] if vf & 4 else 0

        def pair(so, l, r):
            fmt = struct.unpack(">H", d[so : so + 2])[0]
            co, vf1, vf2 = struct.unpack(">HHH", d[so + 2 : so + 8])
            ci = cov(so + co, l)
            if ci is None:
                return None
            if fmt == 1:
                pc = struct.unpack(">H", d[so + 8 : so + 10])[0]
                if ci >= pc:
                    return None
                ps = so + struct.unpack(">H", d[so + 10 + 2 * ci : so + 12 + 2 * ci])[0]
                n = struct.unpack(">H", d[ps : ps + 2])[0]
                rs = 2 + vsize(vf1) + vsize(vf2)
                for i in range(n):
                    o = ps + 2 + rs * i
                    if struct.unpack(">H", d[o : o + 2])[0] == r:
                        return xadv(vf1, o + 2)
                return None
            cd1, cd2, c1n, c2n = struct.unpack(">HHHH", d[so + 8 : so + 16])
            c1 = cls(so + cd1 if cd1 else 0, l)
            c2 = cls(so + cd2 if cd2 else 0, r)
            if c1 >= c1n or c2 >= c2n:
                return None
            rs = vsize(vf1) + vsize(vf2)
            return xadv(vf1, so + 16 + (c1 * c2n + c2) * rs)

        def kern(l, r):
            total = 0
            for subs in subs_by_lookup:
                for so in subs:
                    v = pair(so, l, r)
                    if v is not None:
                        total += v
                        break
            return total

        return kern if subs_by_lookup else None


def liberation_oracle(orc):
    import binascii

    for name, short in (("LiberationSans-Regular.ttf", "sans"), ("LiberationMono-Regular.ttf", "mono")):
        path = os.path.join(FONTS, name)
        raw = open(path, "rb").read()
        orc.add("file", short, len(raw), "%08x" % (binascii.crc32(raw) & 0xFFFFFFFF))
        f = PyFont(path)
        a, dd, g = f.metrics()
        orc.add("metrics", short, f.upem, a, dd, g, f.ng, min(f.nhm, f.ng))
        wanted = list(range(0, 0x250)) + list(range(0x370, 0x500)) + list(range(0x2000, 0x2070))
        wanted += [0x20AC, 0xFFFD, 0x4E00, 0x1F600, 0x10FFFF]
        for cp in wanted:
            orc.add("cmap", short, cp, f.cmap.get(cp, 0))
        for gid in range(f.ng):
            orc.add("adv", short, gid, f.adv(gid))
        kern = f.gpos_kern_fn()
        orc.add("kernsrc", short, 1 if kern else 0)
        if kern and short == "sans":
            for l in range(0x20, 0x7F):
                for r in range(0x20, 0x7F):
                    v = kern(f.cmap[l], f.cmap[r])
                    if v:
                        orc.add("kernpair", short, f.cmap[l], f.cmap[r], v)
        gids = set()
        for ch in "AQ@&%g" + "".join(chr(c) for c in range(0xC0, 0x100)):
            gids.add(f.cmap[ord(ch)])
        for gid in sorted(gids):
            cs = f.outline(gid)
            ends = []
            pts = []
            for c in cs:
                pts += c
                ends.append(len(pts) - 1)
            orc.outline(short, gid, ends, pts)


# ---------------------------------------------------------------- utf-8 cases ------------------
def utf8_cases(path):
    rnd = random.Random(0x5EED)
    cases = [
        b"", b"A", b"\xc3\xa9", b"\xe2\x82\xac", b"\xf0\x9f\x98\x80", b"\xc0\x80", b"\xc1\xbf",
        b"\xe0\x80\x80", b"\xe0\x9f\xbf", b"\xed\xa0\x80", b"\xed\xbf\xbf", b"\xf0\x80\x80\x80",
        b"\xf0\x8f\xbf\xbf", b"\xf4\x90\x80\x80", b"\xf5\x80\x80\x80", b"\xf8\x88\x80\x80\x80",
        b"\xfc\x84\x80\x80\x80\x80", b"\xfe", b"\xff", b"\x80", b"\xbf", b"\xe2\x82", b"\xe2",
        b"\xf0\x9f\x98", b"\xf0\x9f", b"\xf0", b"\xc2", b"A\xe2\x82B", b"\xe2\x28\xa1", b"\xf0\x28\x8c\xbc",
        b"\xf0\x90\x28\xbc", b"\xf0\x28\x8c\x28", b"\xef\xbf\xbe", b"\xef\xbf\xbf", b"\x00", b"a\x00b",
        b"\xf4\x8f\xbf\xbf", b"\xf4\x8f\xbf", b"\xed\x9f\xbf", b"\xee\x80\x80", b"\xc2\x80", b"\xdf\xbf",
        b"\xe0\xa0\x80", b"\xf0\x90\x80\x80", b"\xc3", b"\xc3\xc3\xa9", b"\xe2\x82\xe2\x82\xac",
    ]
    alphabet = [0x41, 0x7F, 0x80, 0x9F, 0xA0, 0xBF, 0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xED, 0xEE,
                0xEF, 0xF0, 0xF1, 0xF4, 0xF5, 0xFF, 0x8F, 0x90]
    for _ in range(300):
        n = rnd.randint(1, 12)
        cases.append(bytes(rnd.choice(alphabet) if rnd.random() < 0.85 else rnd.randrange(256)
                           for _ in range(n)))
    with open(path, "w") as f:
        for c in cases:
            cps = [ord(ch) for ch in c.decode("utf-8", "replace")]
            f.write("%s : %s\n" % (c.hex() or "-", " ".join("%d" % v for v in cps)))


def main():
    orc = Oracle()
    for name, fn in (("synth-fallback.ttf", make_fallback), ("synth-gpos.ttf", make_gpos),
                     ("synth-grid.ttf", make_grid),
                     ("synth-symbol.ttf", make_symbol)):
        data = fn(orc)
        open(os.path.join(HERE, name), "wb").write(data)
        print(name, len(data))
    orc.write(os.path.join(HERE, "synth.oracle"))
    lo = Oracle()
    liberation_oracle(lo)
    lo.write(os.path.join(HERE, "liberation.oracle"))
    utf8_cases(os.path.join(HERE, "utf8.cases"))


if __name__ == "__main__":
    main()
