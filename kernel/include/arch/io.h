/* Portable-facing port-I/O interface (ARCHITECTURE §2: "include/arch/ is the arch interface").
 * The real inline-asm implementation is x86-specific and lives under kernel/arch/x86_64/
 * (ARCHITECTURE §4: "x86 specifics only in kernel/arch/x86_64/"); this header is what the rest of
 * the kernel includes (`#include <arch/io.h>`).
 *
 * D-062 records the mechanism: kernel.mk puts kernel/arch/x86_64/include on the include path, and
 * this file pulls the arch backend in by a name distinct from its own ("io-impl.h", not "io.h")
 * -- a quoted #include first searches the directory of the file doing the including, so a wrapper
 * named identically to the file it wraps would find only itself and never reach the real one. A
 * second arch backend adds its own kernel/arch/<arch>/include/io-impl.h and is selected the same
 * way, by kernel.mk's include path for that arch. */
#ifndef KERNEL_INCLUDE_ARCH_IO_H
#define KERNEL_INCLUDE_ARCH_IO_H

#include "io-impl.h"

#endif
