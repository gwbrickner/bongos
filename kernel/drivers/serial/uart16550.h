/* Kernel-side 16550(-compatible) COM1 driver (ARCHITECTURE §13). Polled output only, same wire
 * protocol as boot/uefi/serial.c but under kernel naming (no "loader" prefix -- ARCHITECTURE §4's
 * subsystem prefix here is the driver's own name, not a borrowed one). */
#ifndef KERNEL_DRIVERS_SERIAL_UART16550_H
#define KERNEL_DRIVERS_SERIAL_UART16550_H

#include <stdbool.h>
#include <stdint.h>

/* Probes for a UART on COM1 and (re)programs it for 115200 8N1. Polls LSR bit 6 (TEMT, bounded)
 * first so the loader's own last bytes aren't garbled mid-transmission, then reprograms without
 * clearing the FIFOs (same reasoning as boot/uefi/serial.c). No locks, boot-time only (called
 * once, before any concurrency exists); not reentrant. */
void serialInit(void);

/* Writes a NUL-terminated string to COM1, translating '\n' to "\r\n". No locks; safe to call from
 * any context this early (single core, no concurrency yet) -- not reentrant once real concurrency
 * exists (a later milestone adds a spinlock here). */
void serialWriteString(const char *s);

/* Non-blocking read: if a byte is waiting in the RX FIFO (LSR bit 0, Data Ready), reads it into
 * `*out` and returns true; otherwise returns false immediately (never spins). No locks; same
 * reentrancy caveat as serialWriteString. */
bool serialTryReadByte(uint8_t *out);

/* Reads and discards every byte currently waiting in the RX FIFO (repeated serialTryReadByte
 * until it returns false). Used before waiting on an expected ack byte, so a stale byte queued
 * before the wait began can't pre-satisfy it (ARCHITECTURE §23's screenshot-test handshake,
 * M1.4). No locks. */
void serialDrainRx(void);

#endif
