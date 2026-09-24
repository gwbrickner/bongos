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

void loaderSerialInit(void) {
    outb(COM1_PORT + 1, 0x00); /* disable UART interrupts */
    outb(COM1_PORT + 3, 0x80); /* DLAB on to set the baud-rate divisor */
    outb(COM1_PORT + 0, 0x01); /* divisor low byte: 1 -> 115200 baud */
    outb(COM1_PORT + 1, 0x00); /* divisor high byte */
    outb(COM1_PORT + 3, 0x03); /* DLAB off; 8 data bits, no parity, 1 stop bit */
    outb(COM1_PORT + 2, 0xC7); /* enable + clear the FIFOs, 14-byte trigger threshold */
    outb(COM1_PORT + 4, 0x03); /* DTR + RTS asserted; no loopback */
}

static void serialWriteByte(char c) {
    while ((inb(COM1_PORT + 5) & LSR_THR_EMPTY) == 0) {
        /* wait for the transmit holding register to empty */
    }
    outb(COM1_PORT, (uint8_t)c);
}

void loaderSerialWriteString(const char *s) {
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            serialWriteByte('\r');
        }
        serialWriteByte(*s);
    }
}
