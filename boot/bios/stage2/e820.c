#include "e820.h"

#include "bootmem.h"
#include "rm.h"

#include <stdbool.h>

#define E820_BUFFER_ADDR 0x1100u
#define E820_SMAP        0x534D4150u
#define E820_MAX_CALLS   256u

uint32_t e820Scan(MemMapInput *out, uint32_t outCap) {
    uint32_t n = 0;
    uint32_t ebx = 0;
    uint8_t *buf = (uint8_t *)(uintptr_t)E820_BUFFER_ADDR;

    for (uint32_t calls = 0; calls < E820_MAX_CALLS; calls++) {
        RmRegs r;
        bootMemset(&r, 0, sizeof(r));
        r.eax = 0xE820;
        r.edx = E820_SMAP;
        r.ecx = 24;
        r.ebx = ebx;
        r.edi = E820_BUFFER_ADDR;
        r.es = 0;
        bootMemset(buf, 0, 24);
        /* D-107: pre-set the ACPI 3.0 extended-attribute dword's "valid" bit before every call.
         * A BIOS that reports ECX=24 (claiming the extended field is present) but doesn't
         * actually write it would otherwise leave this word zeroed by the memset above, making
         * every single descriptor look "invalid" (bit 0 clear) and silently drop the whole map. */
        {
            uint32_t validBit = 1u;
            bootMemcpy(buf + 20, &validBit, sizeof(validBit));
        }

        rmInt(0x15, &r);

        bool carry = (r.eflags & 1u) != 0;
        if (carry || r.eax != E820_SMAP) {
            break;
        }

        uint32_t descLen = r.ecx;
        uint64_t base = 0, length = 0;
        uint32_t type = 0, extAttr = 0;
        bootMemcpy(&base, buf + 0, sizeof(base));
        bootMemcpy(&length, buf + 8, sizeof(length));
        bootMemcpy(&type, buf + 16, sizeof(type));
        if (descLen >= 24) {
            bootMemcpy(&extAttr, buf + 20, sizeof(extAttr));
        }

        bool skip = descLen < 20 || length == 0 || (descLen >= 24 && (extAttr & 1u) == 0);
        if (!skip && n < outCap) {
            out[n].base = base;
            out[n].length = length;
            out[n].type = memMapE820TypeToBootMem(type, extAttr);
            n++;
        }

        ebx = r.ebx;
        if (ebx == 0 || n >= outCap) {
            break;
        }
    }

    return n;
}
