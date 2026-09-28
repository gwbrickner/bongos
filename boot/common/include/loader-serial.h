/* Raw COM1 output/input, shared by both loaders (boot/common/hw/serial.c). Port I/O straight
 * from the loader is fine (ARCHITECTURE §5.5); the kernel gets its own 16550 driver in M1.3
 * (kernel/drivers/serial). */
#ifndef LOADER_SERIAL_H
#define LOADER_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

/* Must be called before loaderSerialWriteString(). Safe before or after ExitBootServices (raw
 * port I/O, no Boot Services calls); not reentrant/thread-safe, but nothing in the loader is
 * concurrent. Re-initializing mid-boot risks dropping bytes the firmware has queued (see the
 * FCR comment in serial.c), so call it exactly once. */
void loaderSerialInit(void);

/* Writes a NUL-terminated string to COM1, translating each '\n' to "\r\n" so a plain terminal
 * doesn't stairstep the output. Safe before or after ExitBootServices. */
void loaderSerialWriteString(const char *s);

/* Writes `v` as decimal digits (no leading zeros, "0" for zero), no newline. For error line
 * numbers, the menu's row numbers, and the countdown -- this loader has no printf (D-065: no
 * libc), so this is the one place decimal formatting lives. Safe before or after
 * ExitBootServices. */
void loaderSerialWriteUint(uint32_t v);

/* Non-blocking read of one byte from COM1 (checks LSR bit 0, "data ready"; never waits). Returns
 * true and fills *out if a byte was pending, false otherwise. BIOS-only in practice: D-068
 * forbids polling COM1 receive before ExitBootServices under UEFI (OVMF's TerminalDxe owns it
 * until then), but the function itself is firmware-agnostic port I/O, so it lives here rather
 * than being duplicated. Not reentrant, same as the rest of this file. */
bool loaderSerialReadByte(uint8_t *out);

#endif
