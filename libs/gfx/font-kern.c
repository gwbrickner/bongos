/* kern / GPOS kerning (M12.3, D-154). Step 5 fills this in; until then kerning is off. */
#include "gfx/font-internal.h"

void fontKernResolve(GfxFont *f, uint32_t kernOff, uint32_t kernLen) {
    (void)f;
    (void)kernOff;
    (void)kernLen;
}

int32_t gfxFontKernUnits(const GfxFont *f, uint16_t left, uint16_t right) {
    (void)f;
    (void)left;
    (void)right;
    return 0;
}

GfxFontKernSource gfxFontKernSource(const GfxFont *f) {
    (void)f;
    return GFX_FONT_KERN_NONE;
}
