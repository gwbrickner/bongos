#include "acpi_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int acpiFxLoad(AcpiFx *fx, const char *dir) {
    memset(fx, 0, sizeof(*fx));
    char path[512];
    snprintf(path, sizeof(path), "%s/manifest.txt", dir);
    FILE *m = fopen(path, "r");
    if (m == NULL) {
        return -1;
    }
    char line[256];
    int ok = 0;
    while (fgets(line, sizeof(line), m) != NULL) {
        if (line[0] == '#') {
            continue;
        }
        unsigned long long rsdp;
        if (sscanf(line, "rsdp 0x%llx", &rsdp) == 1) {
            fx->rsdpPhys = rsdp;
            ok = 1;
            continue;
        }
        char file[64], sig[8];
        unsigned long long phys;
        unsigned len;
        if (sscanf(line, "%63s %7s 0x%llx %u", file, sig, &phys, &len) != 4 ||
            fx->count >= ACPI_FX_MAX_BLOCKS) {
            fclose(m);
            return -1;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        FILE *f = fopen(path, "rb");
        if (f == NULL) {
            fclose(m);
            return -1;
        }
        AcpiFxBlock *b = &fx->blocks[fx->count];
        b->bytes = malloc(len);
        if (b->bytes == NULL || fread(b->bytes, 1, len, f) != len) {
            fclose(f);
            fclose(m);
            return -1;
        }
        fclose(f);
        b->phys = phys;
        b->len = len;
        snprintf(b->sig, sizeof(b->sig), "%.4s", sig);
        fx->count++;
    }
    fclose(m);
    return ok ? 0 : -1;
}

void acpiFxRelease(AcpiFx *fx) {
    for (int i = 0; i < fx->count; i++) {
        free(fx->blocks[i].bytes);
        fx->blocks[i].bytes = NULL;
    }
    fx->count = 0;
}

static Status fxRead(void *ctx, uint64_t phys, void *dst, uint32_t len) {
    AcpiFx *fx = ctx;
    for (int i = 0; i < fx->count; i++) {
        const AcpiFxBlock *b = &fx->blocks[i];
        if (phys >= b->phys && phys - b->phys <= b->len && len <= b->len - (phys - b->phys)) {
            memcpy(dst, b->bytes + (phys - b->phys), len);
            return STATUS_OK;
        }
    }
    return STATUS_ERR_INVALID;
}

static void *fxAlloc(void *ctx, uint32_t len) {
    AcpiFx *fx = ctx;
    fx->live++;
    return malloc(len);
}

static void fxFree(void *ctx, void *p, uint32_t len) {
    AcpiFx *fx = ctx;
    (void)len;
    fx->live--;
    free(p);
}

AcpiPhysOps acpiFxOps(AcpiFx *fx) {
    AcpiPhysOps o = {fx, fxRead, fxAlloc, fxFree};
    return o;
}
