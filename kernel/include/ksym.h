/* KSYM v1 lookup (docs/specs/ksyms.md, D-075): turns a return address into `name+0xoff` for
 * panic/trap backtraces (ARCHITECTURE §24). */
#ifndef KERNEL_KSYM_H
#define KERNEL_KSYM_H

#include "uapi/status.h"

#include <stddef.h>
#include <stdint.h>

/* Pure and host-testable (no kernel dependencies): looks `addr` up in a KSYM v1 blob
 * (`blob`/`size`), writing the matching symbol's name into `nameOut` (truncated to fit,
 * NUL-terminated, never overflows `nameCap`) and its exact address into `*symAddrOut`.
 * STATUS_OK if a symbol containing `addr` was found (the nearest symbol at or before `addr`,
 * within `[textBase, textEnd)`), STATUS_ERR_NOT_FOUND otherwise -- including for any malformed or
 * truncated blob, which this bounds-checks every read against rather than ever reading out of
 * bounds or faulting (this runs during a panic, when trust in the kernel's own state is already
 * reduced). No locks; safe from any context. */
Status ksymDecodeLookup(const uint8_t *blob, size_t size, uint64_t addr, char *nameOut,
                        size_t nameCap, uint64_t *symAddrOut);

/* ksymDecodeLookup() for a KASLR-slid kernel (M2.6): the KSYM blob keeps link-time addresses, so
 * `addr` is a runtime address and `slide` how far the image moved. Looks up `addr - slide` and
 * adds `slide` back to the symbol address written to `*symAddrOut` (so `addr - *symAddrOut` is
 * the offset into the symbol, as before). `symAddrOut` may be NULL if the caller only wants the
 * name. STATUS_ERR_NOT_FOUND if `addr < slide` (no link address could map to it) or on anything
 * ksymDecodeLookup() rejects; `*symAddrOut` is untouched on failure. Slide 0 is exactly
 * ksymDecodeLookup(). Pure; same safety contract as ksymDecodeLookup(). */
Status ksymDecodeLookupSlid(const uint8_t *blob, size_t size, uint64_t addr, uint64_t slide,
                            char *nameOut, size_t nameCap, uint64_t *symAddrOut);

/* Formats "name+0xoff" into `buf` (truncated to fit `bufSize`, always NUL-terminated), or "?" if
 * `addr` isn't found in the kernel's own embedded KSYM v1 blob (`ksymsStart`/`ksymsEnd`,
 * kernel.ld). `addr` is a runtime address: kernelSlide() (sections.h) is subtracted before the
 * lookup, and the printed offset is relative to the runtime symbol address. No locks; safe from any
 * context, including a trap/panic handler. */
void ksymSymbolize(uint64_t addr, char *buf, size_t bufSize);

#endif
