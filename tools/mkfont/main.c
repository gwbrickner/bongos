/* tools/mkfont: builds bongOS's console font (ARCHITECTURE §5.2/§19, ROADMAP M1.4, D-069). A
 * host tool (own clang, no cross flags), per ARCHITECTURE §0's host-tool exception.
 *
 * Two subcommands:
 *   mkfont psf SRC.txt OUT.psf     parses the '#'/'.' glyph-grid source (data/fonts/console.txt)
 *                                  and writes a PSF2 binary (data/fonts/console.psf).
 *   mkfont c SYMBOL IN.psf OUT.c   validates a PSF2 file's header and emits a C source file
 *                                  defining `const uint8_t SYMBOL[]` (the raw PSF2 bytes) and
 *                                  `const uint32_t SYMBOLSize`, for linking the font directly
 *                                  into the loader/kernel image instead of loading it from disk.
 *
 * `make font` regenerates data/fonts/console.psf from data/fonts/console.txt; `make font-check`
 * (wired into `make format-check`) regenerates into build/ and diffs against the checked-in file,
 * so the source grid and the compiled binary can never drift apart silently. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#define FONT_LENGTH                                                                                \
    128 /* PSF2 glyph count: every index 0-127, even though only 0x00 and                          \
         * 0x20-0x7E are ever drawn from */
#define PSF2_MAGIC      0x864AB572u
#define PSF2_HEADERSIZE 32u

typedef struct {
    uint8_t rows[FONT_HEIGHT]; /* one byte per row; bit 7 (MSB) is the leftmost pixel */
    int defined;
} Glyph;

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(1);
}

/* Strips a trailing \n/\r\n (in place) and returns the resulting length. */
static size_t chomp(char *line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    return len;
}

/* Parses `path` (data/fonts/console.txt's grammar: '#'-comments, one "font 8x16" line, then any
 * number of "glyph 0xHH [label]" lines each immediately followed by exactly FONT_HEIGHT lines of
 * exactly FONT_WIDTH '#'/'.' characters) into `glyphs[FONT_LENGTH]`. Exits the process with a
 * line-numbered message on any malformed input, a duplicate or out-of-range glyph index, a
 * missing "font 8x16" line, a missing glyph 0x00 (the replacement glyph), or a missing glyph in
 * 0x20-0x7E (every printable ASCII character must be defined). Every index in 0-127 that isn't
 * explicitly defined is filled with a copy of glyph 0x00 (the replacement glyph) on return. */
static void parseConsoleTxt(const char *path, Glyph glyphs[FONT_LENGTH]) {
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        die("mkfont: %s: %s\n", path, strerror(errno));
    }

    memset(glyphs, 0, sizeof(Glyph) * FONT_LENGTH);
    int sawFontLine = 0;
    int curIndex = -1; /* -1: not currently inside a glyph body */
    int curRow = 0;
    char line[512];
    int lineNo = 0;

    while (fgets(line, sizeof(line), f) != NULL) {
        lineNo++;
        chomp(line);

        if (curIndex >= 0) {
            /* Inside a glyph body: this line must be exactly FONT_WIDTH '#'/'.' characters. */
            if (strlen(line) != FONT_WIDTH) {
                die("mkfont: %s:%d: glyph 0x%02x row %d must be exactly %d characters of '#'/'.'\n",
                    path, lineNo, curIndex, curRow, FONT_WIDTH);
            }
            uint8_t byte = 0;
            for (int i = 0; i < FONT_WIDTH; i++) {
                char c = line[i];
                if (c == '#') {
                    byte |= (uint8_t)(1u << (FONT_WIDTH - 1 - i));
                } else if (c != '.') {
                    die("mkfont: %s:%d: glyph 0x%02x row %d: invalid character '%c' (want '#' or "
                        "'.')\n",
                        path, lineNo, curIndex, curRow, c);
                }
            }
            glyphs[curIndex].rows[curRow] = byte;
            curRow++;
            if (curRow == FONT_HEIGHT) {
                glyphs[curIndex].defined = 1;
                curIndex = -1;
                curRow = 0;
            }
            continue;
        }

        if (line[0] == '\0' || line[0] == '#') {
            continue; /* blank line or whole-line comment */
        }
        if (strncmp(line, "font ", 5) == 0) {
            if (strcmp(line + 5, "8x16") != 0) {
                die("mkfont: %s:%d: expected \"font 8x16\", got \"font %s\"\n", path, lineNo,
                    line + 5);
            }
            sawFontLine = 1;
            continue;
        }
        if (strncmp(line, "glyph ", 6) == 0) {
            char *p = line + 6;
            char *end = NULL;
            long idx = strtol(p, &end, 0); /* base 0: accepts the "0xHH" form */
            if (end == p || idx < 0 || idx >= FONT_LENGTH) {
                die("mkfont: %s:%d: glyph index must be 0x00-0x7f\n", path, lineNo);
            }
            if (glyphs[idx].defined) {
                die("mkfont: %s:%d: duplicate glyph 0x%02lx\n", path, lineNo, idx);
            }
            curIndex = (int)idx;
            curRow = 0;
            continue;
        }
        die("mkfont: %s:%d: unrecognized line: \"%s\"\n", path, lineNo, line);
    }
    fclose(f);

    if (curIndex >= 0) {
        die("mkfont: %s: glyph 0x%02x is truncated (needs %d rows, got %d)\n", path, curIndex,
            FONT_HEIGHT, curRow);
    }
    if (!sawFontLine) {
        die("mkfont: %s: missing \"font 8x16\" line\n", path);
    }
    if (!glyphs[0].defined) {
        die("mkfont: %s: glyph 0x00 (the replacement glyph) is not defined\n", path);
    }
    for (int c = 0x20; c <= 0x7E; c++) {
        if (!glyphs[c].defined) {
            die("mkfont: %s: glyph 0x%02x is not defined (every printable ASCII character 0x20-"
                "0x7e is required)\n",
                path, c);
        }
    }
    for (int i = 0; i < FONT_LENGTH; i++) {
        if (!glyphs[i].defined) {
            memcpy(glyphs[i].rows, glyphs[0].rows, sizeof(glyphs[i].rows));
            glyphs[i].defined = 1;
        }
    }
}

static void writeLeU32(FILE *f, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF),
                    (uint8_t)((v >> 24) & 0xFF)};
    fwrite(b, 1, 4, f);
}

/* Writes the PSF2 binary (magic, version 0, headersize 32, flags 0 (no Unicode table), length
 * FONT_LENGTH, charsize FONT_HEIGHT, height FONT_HEIGHT, width FONT_WIDTH), then each glyph's
 * FONT_HEIGHT raw row bytes back to back -- exactly 32 + FONT_LENGTH*FONT_HEIGHT bytes (2080 for
 * the current dimensions). */
static void writePsf(const char *path, const Glyph glyphs[FONT_LENGTH]) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        die("mkfont: %s: %s\n", path, strerror(errno));
    }
    writeLeU32(f, PSF2_MAGIC);
    writeLeU32(f, 0);               /* version */
    writeLeU32(f, PSF2_HEADERSIZE); /* headersize */
    writeLeU32(f, 0);               /* flags: no Unicode table */
    writeLeU32(f, FONT_LENGTH);
    writeLeU32(f, FONT_HEIGHT); /* charsize: bytes per glyph = FONT_HEIGHT (1 byte/row) */
    writeLeU32(f, FONT_HEIGHT);
    writeLeU32(f, FONT_WIDTH);
    for (int i = 0; i < FONT_LENGTH; i++) {
        fwrite(glyphs[i].rows, 1, FONT_HEIGHT, f);
    }
    fclose(f);
}

static uint32_t readLeU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void cmdPsf(const char *srcPath, const char *outPath) {
    Glyph glyphs[FONT_LENGTH];
    parseConsoleTxt(srcPath, glyphs);
    writePsf(outPath, glyphs);
}

/* Reads a PSF2 file, validates its header matches exactly the shape this tool itself produces
 * (any other shape means someone hand-edited or corrupted the .psf -- data/fonts/console.psf
 * must only ever come from `mkfont psf`), then emits a C source file with the raw bytes as a
 * `const uint8_t SYMBOL[]` array plus a `const uint32_t SYMBOLSize`. */
static void cmdC(const char *symbol, const char *inPath, const char *outPath) {
    FILE *in = fopen(inPath, "rb");
    if (in == NULL) {
        die("mkfont: %s: %s\n", inPath, strerror(errno));
    }
    uint8_t header[PSF2_HEADERSIZE];
    if (fread(header, 1, sizeof(header), in) != sizeof(header)) {
        die("mkfont: %s: too short to be a PSF2 file\n", inPath);
    }
    uint32_t magic = readLeU32(header);
    uint32_t version = readLeU32(header + 4);
    uint32_t headersize = readLeU32(header + 8);
    uint32_t flags = readLeU32(header + 12);
    uint32_t length = readLeU32(header + 16);
    uint32_t charsize = readLeU32(header + 20);
    uint32_t height = readLeU32(header + 24);
    uint32_t width = readLeU32(header + 28);
    if (magic != PSF2_MAGIC || version != 0 || headersize != PSF2_HEADERSIZE || flags != 0 ||
        length != FONT_LENGTH || charsize != FONT_HEIGHT || height != FONT_HEIGHT ||
        width != FONT_WIDTH) {
        die("mkfont: %s: not a valid bongOS console PSF2 file (unexpected header)\n", inPath);
    }

    uint8_t *body = malloc((size_t)length * charsize);
    if (body == NULL) {
        die("mkfont: out of memory\n");
    }
    size_t bodyLen = (size_t)length * charsize;
    if (fread(body, 1, bodyLen, in) != bodyLen) {
        die("mkfont: %s: truncated glyph data\n", inPath);
    }
    /* A trailing byte beyond header+glyphs is unexpected for this tool's own output (PSF2 with no
     * Unicode table ends exactly here), so treat it as a validation failure rather than silently
     * ignoring extra data. */
    if (fgetc(in) != EOF) {
        die("mkfont: %s: unexpected trailing data after glyph table\n", inPath);
    }
    fclose(in);

    FILE *out = fopen(outPath, "w");
    if (out == NULL) {
        die("mkfont: %s: %s\n", outPath, strerror(errno));
    }
    fprintf(out, "/* Generated by tools/mkfont from %s. Do not edit by hand. */\n", inPath);
    fprintf(out, "#include <stdint.h>\n\n");
    fprintf(out, "const uint8_t %s[] = {\n", symbol);
    size_t totalLen = sizeof(header) + bodyLen;
    uint8_t *whole = malloc(totalLen);
    if (whole == NULL) {
        die("mkfont: out of memory\n");
    }
    memcpy(whole, header, sizeof(header));
    memcpy(whole + sizeof(header), body, bodyLen);
    for (size_t i = 0; i < totalLen; i++) {
        fprintf(out, "%s0x%02x,%s", (i % 12 == 0) ? "    " : "", whole[i],
                (i % 12 == 11 || i + 1 == totalLen) ? "\n" : " ");
    }
    fprintf(out, "};\n");
    fprintf(out, "const uint32_t %sSize = %zu;\n", symbol, totalLen);
    fclose(out);
    free(whole);
    free(body);
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s psf SRC.txt OUT.psf\n"
            "       %s c SYMBOL IN.psf OUT.c\n",
            argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "psf") == 0) {
        cmdPsf(argv[2], argv[3]);
        return 0;
    }
    if (argc == 5 && strcmp(argv[1], "c") == 0) {
        cmdC(argv[2], argv[3], argv[4]);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
