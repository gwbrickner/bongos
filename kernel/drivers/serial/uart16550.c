/* See uart16550.h. Mirrors boot/uefi/serial.c's probe/wait/translate approach (same 16550
 * hardware, same reasoning), rewritten under kernel naming and using kernel/include/arch/io.h
 * instead of a private outb/inb pair. */
#include "uart16550.h"

#include <arch/io.h>
#include <stdint.h>

#define COM1_PORT     0x3F8
#define LSR_THR_EMPTY 0x20
/* See boot/uefi/serial.c's LSR_WAIT_SPINS comment: bounds the per-byte wait so a board with no
 * working UART can't hang boot forever; serialInit()'s scratch-register probe means this bound is
 * hit at most once (when the probe itself was wrong), not once per byte written. */
#define LSR_WAIT_SPINS 1000000

static int uartPresent = 1;

void serialInit(void) {
    /* Wait for any bytes the loader already queued to finish transmitting before reprogramming
     * the UART out from under them (bounded: a truly stuck UART must not hang boot). */
    for (int spins = 0; spins < LSR_WAIT_SPINS; spins++) {
        if (ioInByte(COM1_PORT + 5) & 0x40) { /* TEMT: transmitter (FIFO + shift register) empty */
            break;
        }
    }

    ioOutByte(COM1_PORT + 1, 0x00); /* disable UART interrupts */
    ioOutByte(COM1_PORT + 3, 0x80); /* DLAB on to set the baud-rate divisor */
    ioOutByte(COM1_PORT + 0, 0x01); /* divisor low byte: 1 -> 115200 baud */
    ioOutByte(COM1_PORT + 1, 0x00); /* divisor high byte */
    ioOutByte(COM1_PORT + 3, 0x03); /* DLAB off; 8 data bits, no parity, 1 stop bit */
    /* Enable + 14-byte FIFO trigger threshold, without clearing them (0x02/0x04) -- see the
     * matching comment in boot/uefi/serial.c. */
    ioOutByte(COM1_PORT + 2, 0xC1);
    ioOutByte(COM1_PORT + 4, 0x03); /* DTR + RTS asserted; no loopback */

    ioOutByte(COM1_PORT + 7, 0xA5);
    uartPresent = (ioInByte(COM1_PORT + 7) == 0xA5);
}

static void serialWriteByte(char c) {
    if (!uartPresent) {
        return;
    }
    for (int spins = 0; spins < LSR_WAIT_SPINS; spins++) {
        if (ioInByte(COM1_PORT + 5) & LSR_THR_EMPTY) {
            ioOutByte(COM1_PORT, (uint8_t)c);
            return;
        }
    }
    uartPresent = 0; /* stuck UART: stop trying, don't hang the rest of boot */
}

void serialWriteString(const char *s) {
    for (; *s != '\0'; s++) {
        if (*s == '\n') {
            serialWriteByte('\r');
        }
        serialWriteByte(*s);
    }
}

#define LSR_DATA_READY 0x01

bool serialTryReadByte(uint8_t *out) {
    if (!uartPresent) {
        return false;
    }
    if ((ioInByte(COM1_PORT + 5) & LSR_DATA_READY) == 0) {
        return false;
    }
    *out = ioInByte(COM1_PORT);
    return true;
}

void serialDrainRx(void) {
    uint8_t discard;
    while (serialTryReadByte(&discard)) {
        /* keep reading until the FIFO is empty */
    }
}
