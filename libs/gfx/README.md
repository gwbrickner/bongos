2D rasterizer and PNG/BMP image decoders (M12.2); JPEG/GIF (M12.7) come later. The font engine and text layout (M12.3) are here too.
Premultiplied ARGB32 throughout, see `gfx.h`; paths and fills in `gfx-path.h`, blur/shadows in
`gfx-blur.h`, damage regions in `gfx-region.h`, decoders in `gfx-image.h`. Userland/host only.
Decisions: D-141..D-147 (graphics), D-150..D-158 (fonts and text).

## Fonts and text (M12.3)
- `gfx-font.h`: `GfxFont` (a zero-copy TrueType parser: `font-sfnt.c`, `font-cmap.c`, `font-glyf.c`,
  `font-kern.c`), `gfxFontRenderGlyph` (`font-render.c`: an unhinted A8 mask at one of four
  quarter-pixel positions), and `GfxFontStack` (`font-cache.c`: fallback faces plus an LRU glyph
  cache under a byte budget; a returned image is valid until the next `gfxFontStackGlyph`).
- `gfx-text.h`: UTF-8 decoding, the simplified UAX #14 line breaker (`text-utf8.c`,
  `text-break.c`), paragraph layout (`text-layout.c`) and drawing (`text-draw.c`). Spec and tables:
  `docs/specs/gfx-text.md`.
- Fonts and text are untrusted input. Tests: `tests/host/gfx_font_*`, `gfx_text_*`; oracles from
  `tests/data/font/gen.py` (synthetic fonts built by construction, an independent Python font
  parse, Python's UTF-8 decoder, a Python line-break and layout reference); goldens
  `tests/data/gfx/ref/font_*.png`. Shipped fonts: `data/fonts/` (Liberation, OFL 1.1).
