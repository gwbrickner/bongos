/* Portable-facing declaration for CPU table setup (GDT/TSS/IDT, ARCHITECTURE §7.1). The
 * implementation is entirely x86-specific and lives in kernel/arch/x86_64/cpu-init.c.
 */
#ifndef KERNEL_INCLUDE_ARCH_CPU_INIT_H
#define KERNEL_INCLUDE_ARCH_CPU_INIT_H

/* Builds and loads the BSP's GDT/TSS (and, once trap handling exists, the IDT). Must run before
 * anything that could fault, and before any other protected (`-fstack-protector-strong`) frame
 * outlives it. Panics if called more than once (D-072: BSP-only through M3.4 -- SMP bring-up in
 * M3.5 gives each AP its own tables instead). No locks, boot-time-only, not IRQ-safe (changes
 * privileged CPU state); never called again after the one BSP call. */
void archCpuInitBsp(void);

/* The AP's equivalent (D-190): builds its GDT/TSS inside `cl->arch` (whose stack and IST ranges the
 * BSP filled in beforehand), loads them with LTR, then loads the shared IDT. Runs on the AP, IF=0,
 * with IA32_GS_BASE already pointing at `cl`. Not IRQ-safe; boot-time-only, once per CPU. */
struct CpuLocal;
void archCpuInitAp(struct CpuLocal *cl);

#endif
