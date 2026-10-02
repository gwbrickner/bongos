/* Kernel logging (ARCHITECTURE §24). M1.3 has only the serial sink; fbcon and the dmesg ring
 * buffer arrive with M1.4/later. */
#ifndef KERNEL_KLOG_H
#define KERNEL_KLOG_H

typedef enum { KLOG_ERROR, KLOG_WARN, KLOG_INFO, KLOG_DEBUG, KLOG_TRACE } KlogLevel;

/* Initializes the serial sink and prints the banner. Must run after serialInit(). Emits a bare
 * "\r\n" first: OVMF's own ConOut output may leave the terminal's column non-zero or mid-escape-
 * sequence, and this guarantees the banner (and everything after it) starts at column 0. No
 * locks, boot-time only; not reentrant. */
void klogInit(void);

/* Formats and writes one log line: "[<level>] <tag>: <message>\r\n". The output section runs under
 * klog's leaf Spinlock with IRQs disabled (D-173, D-188), so it is safe from an interrupt handler
 * and lines never interleave; the lock is bypassed once a panic is in progress. A ring-buffer sink
 * comes later. May not sleep. Truncates silently if the formatted line exceeds the internal line
 * buffer (256 bytes). */
void klogWrite(KlogLevel level, const char *tag, const char *fmt, ...);

/* Writes `s` straight to serial with no level prefix and no added newline handling beyond
 * serialWriteString's own '\n' -> "\r\n" translation: used for lines that must not carry the klog
 * prefix, such as the KTEST wire protocol and the panic banner. IRQs are disabled for the write,
 * like klogWrite(). */
void klogRaw(const char *s);

#endif
