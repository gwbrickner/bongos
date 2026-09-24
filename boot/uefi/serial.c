/* Raw COM1 (16550-compatible UART) output for the UEFI loader. See serial.h. */
#include "serial.h"

#include <stdint.h>

#define COM1_PORT 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#define LSR_THR_EMPTY 0x20
/* Bounds the LSR poll in serialWriteByte(): real hardware without a UART at all reads back 0xFF
 * (floating bus) or 0x00, either of which would spin forever. This is well past what any real
 * 115200-baud transmit ever takes; hitting it means "there's no working UART here", not "wait
 * longer" -- but at ~1M iterations per byte, paying that once per byte of even a short banner
 * would stall boot for tens of seconds, so loaderSerialInit() below probes for a UART once and
 * caches the answer instead of relying on every single write to give up on its own. */
#define LSR_WAIT_SPINS 1000000

/* Set by loaderSerialInit()'s scratch-register probe; serialWriteByte() checks it so a missing
 * UART costs one probe, not one LSR_WAIT_SPINS timeout per byte written. */
static int uartPresent = 1;

void loaderSerialInit(void) {
    outb(COM1_PORT + 1, 0x00); /* disable UART interrupts */
    outb(COM1_PORT + 3, 0x80); /* DLAB on to set the baud-rate divisor */
    outb(COM1_PORT + 0, 0x01); /* divisor low byte: 1 -> 115200 baud */
    outb(COM1_PORT + 1, 0x00); /* divisor high byte */
    outb(COM1_PORT + 3, 0x03); /* DLAB off; 8 data bits, no parity, 1 stop bit */
    /* Enable + 14-byte FIFO trigger threshold, without also clearing them (no 0x02/0x04 clear
     * bits): the firmware may still have bytes queued from its own ConOut output, and clearing
     * the TX FIFO here could drop the tail of that. */
    outb(COM1_PORT + 2, 0xC1);
    outb(COM1_PORT + 4, 0x03); /* DTR + RTS asserted; no loopback */

    /* A 16550-class UART's scratch register (+7) always round-trips whatever byte is written to
     * it; on a board with nothing at this I/O address, it won't. */
    outb(COM1_PORT + 7, 0xA5);
    if (inb(COM1_PORT + 7) != 0xA5) {
        uartPresent = 0;
    }
}

static void serialWriteByte(char c) {
    if (!uartPresent) {
        return;
    }
    for (int spins = 0; spins < LSR_WAIT_SPINS; spins++) {
        if (inb(COM1_PORT + 5) & LSR_THR_EMPTY) {
            outb(COM1_PORT, (uint8_t)c);
            return;
        }
    }
    /* The scratch-register probe passed but the UART still isn't draining -- drop the byte
     * rather than hang the boot forever, and stop trying for the rest of this string/future
     * calls (a stuck UART now will still be stuck on the next byte). */
    uartPresent = 0;
}

void loaderSerialWriteString(const char *s) {
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            serialWriteByte('\r');
        }
        serialWriteByte(*s);
    }
}
