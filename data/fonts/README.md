`console.psf`/`console.txt` (M1.4): the boot/kernel console bitmap font. Original work drawn for
this project, not traced or derived from any existing font (VGA ROM, Terminus, etc.) -- MIT
licensed, same as the rest of the base system (D-069). `console.txt` is the source of truth (an
8x16 `#`/`.` glyph grid per character, `tools/mkfont`); `console.psf` is the compiled PSF2
binary checked in alongside it, kept in sync by `make font-check`.

The UI sans + monospace TrueType fonts (M12.3) will be OFL-licensed third-party data files (see
ARCHITECTURE §0's data-file exception) and will each ship with their license file here.
