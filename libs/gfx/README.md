2D rasterizer and PNG/BMP image decoders (M12.2), JPEG and GIF decoders (M12.7); the font engine (M12.3) comes later.
Premultiplied ARGB32 throughout, see `gfx.h`; paths and fills in `gfx-path.h`, blur/shadows in
`gfx-blur.h`, damage regions in `gfx-region.h`, decoders in `gfx-image.h` (JPEG in `jpeg*.c`; GIF and its animation
compositor in `gif.c`). Userland/host only.
Decisions: D-141..D-147, D-160..D-165.
