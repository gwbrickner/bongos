/* acpiextract CLI (D-169):
 *   acpiextract -o DIR LOG                          write DIR/<SIG4>-<n>.dat and DIR/manifest.txt
 *   acpiextract --check [--require SIG,SIG,...] LOG parse and verify; exit 1 on any problem
 * LOG is a serial log (build/logs/<name>.serial.log) containing an ACPIDUMP v1 block. */
#include "acpiextract.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *readFile(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "acpiextract: cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    while (buf != NULL) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (n < cap) {
            break;
        }
        char *nb = realloc(buf, cap * 2);
        if (nb == NULL) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = nb;
        cap *= 2;
    }
    fclose(f);
    if (buf == NULL) {
        fprintf(stderr, "acpiextract: out of memory reading %s\n", path);
        return NULL;
    }
    *len = n;
    return buf;
}

static int hasSig(const AcpiExtractDump *d, const char *sig, size_t n) {
    for (uint32_t i = 0; i < d->count; i++) {
        if (strncmp(d->tables[i].sig, sig, n) == 0 && strlen(d->tables[i].sig) == n) {
            return 1;
        }
    }
    return 0;
}

static int writeOut(const AcpiExtractDump *d, const char *dir) {
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "acpiextract: cannot create %s: %s\n", dir, strerror(errno));
        return 1;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/manifest.txt", dir);
    FILE *man = fopen(path, "w");
    if (man == NULL) {
        fprintf(stderr, "acpiextract: cannot write %s\n", path);
        return 1;
    }
    fprintf(man, "# acpiextract manifest v1\nrsdp 0x%016llx\n", (unsigned long long)d->rsdp);
    for (uint32_t i = 0; i < d->count; i++) {
        const AcpiExtractTable *t = &d->tables[i];
        uint32_t n = 0;
        for (uint32_t j = 0; j < i; j++) {
            n += strcmp(d->tables[j].sig, t->sig) == 0;
        }
        char name[32];
        snprintf(name, sizeof(name), "%s-%u.dat", t->sig, n);
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        FILE *f = fopen(path, "wb");
        if (f == NULL || fwrite(t->data, 1, t->len, f) != t->len) {
            fprintf(stderr, "acpiextract: cannot write %s\n", path);
            if (f != NULL) {
                fclose(f);
            }
            fclose(man);
            return 1;
        }
        fclose(f);
        fprintf(man, "%s %s 0x%016llx %u\n", name, t->sig, (unsigned long long)t->phys, t->len);
    }
    fclose(man);
    return 0;
}

int main(int argc, char **argv) {
    const char *outDir = NULL, *require = NULL, *log = NULL;
    int check = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        } else if (strcmp(argv[i], "--check") == 0) {
            check = 1;
        } else if (strcmp(argv[i], "--require") == 0 && i + 1 < argc) {
            require = argv[++i];
        } else if (argv[i][0] != '-' && log == NULL) {
            log = argv[i];
        } else {
            log = NULL;
            outDir = NULL;
            check = 0;
            break;
        }
    }
    if (log == NULL || (outDir == NULL) == (check == 0)) {
        fprintf(stderr,
                "usage: acpiextract -o DIR LOG | acpiextract --check [--require A,B] LOG\n");
        return 2;
    }
    size_t len;
    char *text = readFile(log, &len);
    if (text == NULL) {
        return 1;
    }
    AcpiExtractDump d;
    char err[256];
    int rc = acpiExtractParse(text, len, &d, err, sizeof(err));
    free(text);
    if (rc != 0) {
        fprintf(stderr, "acpiextract: %s: %s\n", log, err);
        return 1;
    }
    if (check) {
        printf("acpiextract: %s: %u tables ok (rsdp 0x%016llx)\n", log, d.count,
               (unsigned long long)d.rsdp);
        int missing = 0;
        for (const char *p = require; p != NULL && *p != '\0';) {
            const char *comma = strchr(p, ',');
            size_t n = comma != NULL ? (size_t)(comma - p) : strlen(p);
            if (!hasSig(&d, p, n)) {
                fprintf(stderr, "acpiextract: %s: required table %.*s is missing\n", log, (int)n,
                        p);
                missing = 1;
            }
            p = comma != NULL ? comma + 1 : p + n;
        }
        acpiExtractFree(&d);
        return missing;
    }
    rc = writeOut(&d, outDir);
    acpiExtractFree(&d);
    return rc;
}
