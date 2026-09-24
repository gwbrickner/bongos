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
/* Bounds the LSR poll below: real hardware without a UART at all reads back 0xFF (floating bus)
 * or 0x00, either of which would spin serialWriteByte() forever. This is well past what any real
 * 115200-baud transmit ever takes; hitting it means "there's no working UART here", not "wait
 * longer". */
#define LSR_WAIT_SPINS 1000000

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
}

static void serialWriteByte(char c) {
    for (int spins = 0; spins < LSR_WAIT_SPINS; spins++) {
        if (inb(COM1_PORT + 5) & LSR_THR_EMPTY) {
            outb(COM1_PORT, (uint8_t)c);
            return;
        }
    }
    /* No working UART on this port; drop the byte rather than hang the boot forever. */
}

void loaderSerialWriteString(const char *s) {
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            serialWriteByte('\r');
        }
        serialWriteByte(*s);
    }
}
