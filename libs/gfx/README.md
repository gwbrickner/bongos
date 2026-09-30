2D rasterizer and PNG/BMP image decoders (M12.2); JPEG/GIF (M12.7) and the font engine (M12.3) come later.
Premultiplied ARGB32 throughout, see `gfx.h`; paths and fills in `gfx-path.h`, blur/shadows in
`gfx-blur.h`, damage regions in `gfx-region.h`, decoders in `gfx-image.h`. Userland/host only.
Decisions: D-141..D-147.
