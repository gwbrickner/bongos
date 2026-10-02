/* Legacy 8259 PIC pair: remapped out of the exception vector range and then fully masked, since
 * every interrupt in this kernel goes through the APICs (ARCHITECTURE §7.2/§7.3, D-172). */
#include "include/apic.h"

#include "klog.h"

#include <arch/io.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

/* The traditional I/O delay: a write to the unused POST port. Old 8259 implementations need a
 * pause between initialization words. */
static void ioDelay(void) {
    ioOutByte(0x80, 0);
}

void pic8259RemapAndMask(void) {
    ioOutByte(PIC1_CMD, 0x11); /* ICW1: edge-triggered, cascade, ICW4 needed. Clears the IMR. */
    ioDelay();
    ioOutByte(PIC2_CMD, 0x11);
    ioDelay();
    ioOutByte(PIC1_DATA, 0x20); /* ICW2: vector base */
    ioDelay();
    ioOutByte(PIC2_DATA, 0x28);
    ioDelay();
    ioOutByte(PIC1_DATA, 0x04); /* ICW3: slave on IRQ2 / slave id 2 */
    ioDelay();
    ioOutByte(PIC2_DATA, 0x02);
    ioDelay();
    ioOutByte(PIC1_DATA, 0x01); /* ICW4: 8086 mode */
    ioDelay();
    ioOutByte(PIC2_DATA, 0x01);
    ioDelay();
    ioOutByte(PIC1_DATA, 0xFF); /* OCW1: mask everything */
    ioOutByte(PIC2_DATA, 0xFF);
    if (pic8259ReadImr(0) != 0xFF || pic8259ReadImr(1) != 0xFF) {
        klogWrite(KLOG_WARN, "irq", "8259 IMR did not read back 0xFF (master 0x%02x slave 0x%02x)",
                  pic8259ReadImr(0), pic8259ReadImr(1));
    }
}

uint8_t pic8259ReadImr(int slave) {
    return ioInByte(slave ? PIC2_DATA : PIC1_DATA);
}

uint8_t pic8259ReadIsr(int slave) {
    uint16_t cmd = slave ? PIC2_CMD : PIC1_CMD;
    ioOutByte(cmd, 0x0B); /* OCW3: the next read of the command port returns the ISR */
    return ioInByte(cmd);
}

void pic8259EoiMaster(void) {
    ioOutByte(PIC1_CMD, 0x20);
}
