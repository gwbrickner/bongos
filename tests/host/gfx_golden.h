/* Golden-image comparison for libs/gfx host tests (M12.2, D-147). Test-only (POSIX allowed).
 *
 * A golden is an exact match (delta 0, no masked pixels) against tests/data/gfx/ref/<name>.png,
 * read with tools/imgdiff's own PNG reader. A mismatch or a MISSING reference fails the test and
 * writes build/host-tests/gfx-out/<name>.png (the actual image) and <name>.diff.png. References
 * are NEVER created automatically: promoting one is a manual step (open the actual PNG, check it
 * really shows what the test claims, copy it into tests/data/gfx/ref/, and say so in the
 * milestone log). After the first commit, changing a reference needs a written justification in
 * the log (the D-070 rule: never regenerate a reference just to get a pass). */
#ifndef TESTS_HOST_GFX_GOLDEN_H
#define TESTS_HOST_GFX_GOLDEN_H

#include <stdbool.h>

#include "gfx/gfx.h"

/* Every pixel must be opaque (A == 255): tests paint an opaque background first. */
bool goldenCheck(const char *name, const GfxSurface *s);

/* An A8 mask, compared as gray (r = g = b = coverage). */
bool goldenCheckMask(const char *name, const GfxMask *m);

#endif
