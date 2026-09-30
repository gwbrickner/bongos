/* Host tests for libs/gfx's JPEG decoder (M12.7, D-160..D-165). See tests/host/README.md. */
#include "framework/test.h"
#include "gfx/gfx-image.h"
#include "gfx/gfx.h"
#include "gfx_decode_testutil.h"

#include <string.h>

/* gfxImageDecode routes by magic: a bare SOI+FF is JPEG (a truncated one, so INVALID or
 * UNSUPPORTED, never "unknown"), and 'J','F','I','F' junk is still not sniffed. */
TEST(gfxImageDecodeSniffsJpegAndGif) {
    GfxImage img;
    static const uint8_t junk[] = {'J', 'F', 'I', 'F', 0, 0, 0, 0};
    ASSERT_EQ(gfxImageDecode(junk, sizeof(junk), NULL, NULL, &img), STATUS_ERR_UNSUPPORTED);
    ASSERT_TRUE(img.pixels == NULL);
    static const uint8_t soi[] = {0xFF, 0xD8, 0xFF, 0xD9};
    Status st = gfxImageDecode(soi, sizeof(soi), NULL, NULL, &img);
    ASSERT_TRUE(st != STATUS_OK);
    ASSERT_TRUE(img.pixels == NULL);
    static const uint8_t gif[] = {'G', 'I', 'F', '8', '9', 'a'};
    st = gfxImageDecode(gif, sizeof(gif), NULL, NULL, &img);
    ASSERT_TRUE(st != STATUS_OK);
    ASSERT_TRUE(img.pixels == NULL);
}
