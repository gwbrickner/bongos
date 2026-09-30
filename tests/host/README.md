The host-side unit test framework (a tiny assert-based, constructor-registered runner) and tests for pure logic: allocators' algorithms, bongfs, crypto vectors, and parsers. Wired to `make host-tests` starting in M1.1.

## libs/gfx golden images (M12.2, D-147)
`gfx_golden.h` compares a canvas against `tests/data/gfx/ref/<name>.png` exactly. A mismatch or a
missing reference fails the test and writes the actual image and a diff to
`build/host-tests/gfx-out/`. References are never auto-created. To promote one: open the actual
PNG (Read/an image viewer), check it really shows what the test says, copy it into
`tests/data/gfx/ref/`, and record that in the milestone log. Changing an existing reference later
needs a written justification in the log (never regenerate one just to get a pass).

## libs/gfx JPEG and GIF decoders (M12.7, D-165)
`tests/data/gfx/gen_jpeg.py` and `gen_gif.py` (Python stdlib only) are independent encoders that write
`jpeg-fixtures.z` and `gif-fixtures.z`; run them by hand, the build never does. The expected pixels
come from the encoder's own coefficients or indices through an exact model, never from parsing the
bitstream, and comparison is exact. `jg_<group>__<variant>` fixtures of one group encode the same
coefficients (different scan scripts, tables, restarts, marker noise) and must decode identically.
If the decoder and a fixture disagree, the specification (T.81 / GIF89a) decides which is wrong.
`gfx_decode_testutil.{h,c}` holds the shared counting/failing allocator and the container loader.
