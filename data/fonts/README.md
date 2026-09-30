`console.psf`/`console.txt` (M1.4): the boot/kernel console bitmap font. Original work drawn for
this project, not traced or derived from any existing font (VGA ROM, Terminus, etc.) -- MIT
licensed, same as the rest of the base system (D-069). `console.txt` is the source of truth (an
8x16 `#`/`.` glyph grid per character, `tools/mkfont`); `console.psf` is the compiled PSF2
binary checked in alongside it, kept in sync by `make font-check`.

## UI fonts (M12.3, D-157)
`LiberationSans-Regular.ttf` (the UI sans) and `LiberationMono-Regular.ttf` (the monospace),
Liberation Fonts **2.1.5**, licensed under the **SIL Open Font License 1.1** (Reserved Font Name
"Liberation"); the license is `LICENSE-Liberation.txt`. These are third-party data files under
ARCHITECTURE §0's font exception, byte-identical to the Ubuntu 24.04 package
`fonts-liberation 1:2.1.5-3`. **Do not modify, subset or rename them**: the OFL's Reserved Font
Name rule applies to any modified version. The license file must be installed next to the fonts
in any image or initrd that carries them.

| File | sha256 |
|---|---|
| `LiberationSans-Regular.ttf` | `4659bc0c58c5028dd488ec928d41d9265db43d9b669fc14ca8b0832daca7b144` |
| `LiberationMono-Regular.ttf` | `395fa5ab8d40c8eba390ced528744ea75a7f69aabf3e68b6f925ca0e39a27370` |

`tests/host/gfx_font_sfnt_test.c` checks both files' sizes and CRC-32s against an independent
Python parse (the sha256 values above are for humans and for `sha256sum` against the Ubuntu package). Bold and
Italic are not shipped yet (the engine has no synthetic bold or oblique).
