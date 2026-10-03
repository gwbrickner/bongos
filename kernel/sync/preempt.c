/* See preempt.h. */
#include "preempt.h"

#include "irq.h"
#include "panic.h"

#include <arch/cpu.h>

_Noreturn void preemptUnderflow(void) {
    panicBug("preemptEnable: preemptCount underflow");
}

bool preemptInAtomic(void) {
    return preemptCount() != 0 || irqDepth() != 0 || !archInterruptsEnabled();
}
