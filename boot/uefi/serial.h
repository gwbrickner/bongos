/* Raw COM1 output for the UEFI loader. Port I/O straight from the loader is fine
 * (ARCHITECTURE §5.5); the kernel gets its own 16550 driver in M1.3 (kernel/drivers/serial). */
#ifndef LOADER_SERIAL_H
#define LOADER_SERIAL_H

/* Must be called before loaderSerialWriteString(). Safe before or after ExitBootServices (raw
 * port I/O, no Boot Services calls); not reentrant/thread-safe, but nothing in the loader is
 * concurrent. Re-initializing mid-boot risks dropping bytes the firmware has queued (see the
 * FCR comment in serial.c), so call it exactly once. */
void loaderSerialInit(void);

/* Writes a NUL-terminated string to COM1, translating each '\n' to "\r\n" so a plain terminal
 * doesn't stairstep the output. Safe before or after ExitBootServices. */
void loaderSerialWriteString(const char *s);

#endif
