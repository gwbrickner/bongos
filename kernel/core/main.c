/* Kernel entry point (ARCHITECTURE §5.4/§5.5 step 4). kernelEntry (entry.asm) calls kernelMain
 * once on the kernel's own stack, with `rdi` holding the BootInfo HHDM virtual address. */
#include "bootinfo-validate.h"
#include "cmdline.h"
#include "kernel-boot.h"
#include "klog.h"
#include "ktest.h"
#include "panic.h"
#include "stack-protector.h"

#include <arch/cpu-init.h>
#include <arch/cpu.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/fbcon/fbcon.h"
#include "drivers/serial/uart16550.h"

/* Copies of the loader's handoff data, in kernel .data rather than the loader's LOADER_RECLAIM
 * pages (ARCHITECTURE §5.5 design note: LOADER_RECLAIM is reclaimed once the kernel no longer
 * needs it, M2.2). The memory map array itself is left where the loader put it -- copying
 * thousands of regions into a fixed kernel buffer isn't worth it before M2.2 exists to reclaim the
 * space it currently lives in anyway. */
static BootInfo bootInfoCopy;
static char cmdlineCopy[BOOTINFO_CMDLINE_MAX];
/* The pointer kernelMain actually received, kept for kernelBootInfo(): bootInfoCopy lives in the
 * kernel image's own .data, outside the HHDM window, so bootInfoValidate() would reject *it* on
 * the pointer-range check alone (that check is about where a real BootInfo must live, per
 * ARCHITECTURE §5.4) even though its contents are byte-for-byte identical. The original page is
 * still LOADER_RECLAIM (not yet reclaimed -- that's M2.2), so it stays valid to read here. */
static const BootInfo *liveBootInfo;

const BootInfo *kernelBootInfo(void) {
    return liveBootInfo;
}

const char *kernelCmdline(void) {
    return cmdlineCopy;
}

static void kernelPrintMemoryMapSummary(const BootInfo *bi) {
    const BootMemRegion *regions =
        (const BootMemRegion *)(uintptr_t)(bi->hhdmBase + bi->memMapPhys);
    uint64_t typeTotals[BOOT_MEM_FRAMEBUFFER + 1];
    for (uint32_t t = 0; t <= BOOT_MEM_FRAMEBUFFER; t++) {
        typeTotals[t] = 0;
    }

    for (uint32_t i = 0; i < bi->memMapCount; i++) {
        const BootMemRegion *r = &regions[i];
        klogWrite(KLOG_INFO, "boot", "  0x%016llx-0x%016llx %s", r->base, r->base + r->length,
                  bootMemTypeName(r->type));
        if (r->type <= BOOT_MEM_FRAMEBUFFER) {
            typeTotals[r->type] += r->length;
        }
    }
    for (uint32_t t = BOOT_MEM_USABLE; t <= BOOT_MEM_FRAMEBUFFER; t++) {
        if (typeTotals[t] == 0) {
            continue;
        }
        klogWrite(KLOG_INFO, "boot", "%s: %llu KiB", bootMemTypeName(t), typeTotals[t] / 1024);
    }
}

/* No locks; boot-time only (called exactly once, from entry.asm, on the kernel's own boot stack);
 * never returns. `no_stack_protector`: stackGuardInit() below must run before any *other*
 * protected frame outlives it, so this function's own frame (which calls it) can't be one either
 * (D-077 -- confirmed by compiler probe that `no_stack_protector` alone doesn't stop a callee
 * from being inlined into a protected caller, which would defeat the point). */
__attribute__((no_stack_protector)) _Noreturn void kernelMain(const BootInfo *bi) {
    /* First: installs the real GDT/TSS/IDT (ARCHITECTURE §7.1/§7.2, D-072/D-074), replacing
     * entry.asm's temporary boot GDT and null IDT, so every fault from here on is reported rather
     * than triple-faulting. */
    archCpuInitBsp();

    serialInit();
    klogInit();

    const char *why = "unknown";
    Status st = bootInfoValidate(bi, &why);
    if (st != STATUS_OK) {
        /* bootInfoValidate() itself never dereferences an unsafe pointer, so `bi` (or its HHDM-
         * relative fields) are safe to have already read by the time we get here. Whether ktest=
         * was requested isn't known yet (that requires the cmdline this same validation just
         * checked), so this path always halts rather than reporting a KTEST FAIL -- acceptable
         * per the M1.3 design: the harness reports it as a HANG with the PANIC line in the log. */
        panic("bootinfo: %s", why);
    }

    /* Immediately after validation, before anything else: every protected function that ran
     * earlier already returned and checked the old fixed-constant guard, and nothing from here on
     * should run with a canary an attacker could predict from source (D-077). */
    stackGuardInit(bi);

    liveBootInfo = bi;
    bootInfoCopy = *bi;
    /* Nothing reads randomSeed out of bootInfoCopy: the only consumer so far, stackGuardInit()
     * above, already read it from the live `bi` pointer before this copy was even made, and
     * M2.6's CSPRNG init will do the same from the live page later. Don't keep a second permanent
     * copy of key material sitting around in kernel .data. A plain write here would be a dead
     * store an optimizing compiler is free to elide (nothing ever reads the field back), so this
     * goes through a volatile pointer the same way the loader wipes its own stack copy. */
    volatile uint8_t *seedWipe = bootInfoCopy.randomSeed;
    for (size_t i = 0; i < sizeof(bootInfoCopy.randomSeed); i++) {
        seedWipe[i] = 0;
    }
    const char *srcCmdline = (const char *)(uintptr_t)(bi->hhdmBase + bi->cmdlinePhys);
    uint32_t i = 0;
    for (; i < BOOTINFO_CMDLINE_MAX - 1 && srcCmdline[i] != '\0'; i++) {
        cmdlineCopy[i] = srcCmdline[i];
    }
    cmdlineCopy[i] = '\0';

    if (!cmdlineHasToken(cmdlineCopy, "fbcon=off")) {
        Status fbSt = fbconInit(&bootInfoCopy.fb, bootInfoCopy.hhdmBase);
        if (fbSt == STATUS_OK) {
            klogWrite(KLOG_INFO, "fbcon", "%ux%u framebuffer console attached",
                      bootInfoCopy.fb.width, bootInfoCopy.fb.height);
        } else if (bootInfoCopy.fb.phys != 0) {
            klogWrite(KLOG_WARN, "fbcon", "framebuffer present but unusable; serial-only console");
        }
    }
    klogWrite(KLOG_INFO, "boot", "cmdline: \"%s\"", cmdlineCopy);

    kernelPrintMemoryMapSummary(&bootInfoCopy);

    ktestRunFromCmdline(cmdlineCopy); /* never returns if ktest= was present */

    klogWrite(KLOG_INFO, "kernel", "init done");
    archHaltForever();
}
