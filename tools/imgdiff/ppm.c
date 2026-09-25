/* See ppm.h. Netpbm's plain-text header grammar: whitespace-separated ASCII tokens, '#' starts a
 * comment that runs to end of line, before the single byte of whitespace that terminates the
 * header and starts the binary pixel data. */
#include "ppm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void setErr(char *errbuf, size_t errbufCap, const char *msg) {
    if (errbuf != NULL && errbufCap > 0) {
        size_t i = 0;
        for (; msg[i] != '\0' && i < errbufCap - 1; i++) {
            errbuf[i] = msg[i];
        }
        errbuf[i] = '\0';
    }
}

/* Skips whitespace and '#'-comments, then reads one whitespace-terminated token into `tok`
 * (capacity tokCap). Returns false at EOF or if the token doesn't fit. */
static bool ppmToken(FILE *f, char *tok, size_t tokCap) {
    int c;
    do {
        c = fgetc(f);
        if (c == '#') {
            while (c != '\n' && c != EOF) {
                c = fgetc(f);
            }
        }
    } while (c != EOF && (c == ' ' || c == '\t' || c == '\n' || c == '\r'));
    if (c == EOF) {
        return false;
    }
    size_t n = 0;
    while (c != EOF && c != ' ' && c != '\t' && c != '\n' && c != '\r') {
        if (n >= tokCap - 1) {
            return false;
        }
        tok[n++] = (char)c;
        c = fgetc(f);
    }
    tok[n] = '\0';
    return n > 0;
}

bool ppmRead(const char *path, Image *out, char *errbuf, size_t errbufCap) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        setErr(errbuf, errbufCap, "cannot open file");
        return false;
    }
    char tok[32];
    if (!ppmToken(f, tok, sizeof(tok)) || strcmp(tok, "P6") != 0) {
        setErr(errbuf, errbufCap, "not a P6 PPM");
        fclose(f);
        return false;
    }
    if (!ppmToken(f, tok, sizeof(tok))) {
        setErr(errbuf, errbufCap, "truncated PPM header (width)");
        fclose(f);
        return false;
    }
    long width = strtol(tok, NULL, 10);
    if (!ppmToken(f, tok, sizeof(tok))) {
        setErr(errbuf, errbufCap, "truncated PPM header (height)");
        fclose(f);
        return false;
    }
    long height = strtol(tok, NULL, 10);
    if (!ppmToken(f, tok, sizeof(tok))) {
        setErr(errbuf, errbufCap, "truncated PPM header (maxval)");
        fclose(f);
        return false;
    }
    long maxval = strtol(tok, NULL, 10);
    if (maxval != 255) {
        setErr(errbuf, errbufCap, "PPM maxval must be 255");
        fclose(f);
        return false;
    }
    /* Exactly one whitespace byte separates the header from the binary data (already consumed by
     * ppmToken's trailing read-ahead of one byte past "255" -- that byte was the separator and
     * was discarded, not pixel data, since ppmToken stops at the first whitespace/EOF). */
    if (width <= 0 || height <= 0 || width > IMGDIFF_MAX_DIM || height > IMGDIFF_MAX_DIM) {
        setErr(errbuf, errbufCap, "PPM dimensions out of range");
        fclose(f);
        return false;
    }
    if (!imageAlloc(out, (uint32_t)width, (uint32_t)height)) {
        setErr(errbuf, errbufCap, "out of memory");
        fclose(f);
        return false;
    }
    size_t need = (size_t)width * (size_t)height * 3u;
    size_t got = fread(out->rgb, 1, need, f);
    fclose(f);
    if (got != need) {
        setErr(errbuf, errbufCap, "truncated PPM pixel data");
        imageFree(out);
        return false;
    }
    return true;
}

bool ppmWrite(const char *path, const Image *img, char *errbuf, size_t errbufCap) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        setErr(errbuf, errbufCap, "cannot create file");
        return false;
    }
    if (fprintf(f, "P6\n%u %u\n255\n", img->width, img->height) < 0) {
        setErr(errbuf, errbufCap, "write error");
        fclose(f);
        return false;
    }
    size_t need = (size_t)img->width * (size_t)img->height * 3u;
    size_t wrote = fwrite(img->rgb, 1, need, f);
    fclose(f);
    if (wrote != need) {
        setErr(errbuf, errbufCap, "write error");
        return false;
    }
    return true;
}
