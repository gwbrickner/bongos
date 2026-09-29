/* A minimal ANSI/VT100 arrow-key parser for the BIOS boot menu's serial input (D-110): the test
 * harness drives the menu over COM1 by sending real terminal escape sequences (arrow keys are
 * ESC [ A / ESC [ B, or ESC O A / ESC O B in "application cursor keys" mode), and SeaBIOS itself
 * never reads the serial port, so stage2 must parse these itself instead of relying on a
 * firmware key-read call the way BIOS INT 16h keyboard input does. Reuses boot/common/bootmenu.h's
 * BootKey/BootKeyKind rather than inventing a parallel type. */
#ifndef BOOT_COMMON_BOOTKEY_H
#define BOOT_COMMON_BOOTKEY_H

#include <stdbool.h>
#include <stdint.h>

#include "bootmenu.h"

typedef enum {
    BOOT_KEY_PARSE_IDLE = 0,
    BOOT_KEY_PARSE_GOT_ESC,
    BOOT_KEY_PARSE_GOT_CSI, /* saw ESC '[' or ESC 'O'; next byte picks the direction */
} BootKeyParseState;

typedef struct {
    BootKeyParseState state;
} BootKeyParser;

void bootKeyParserInit(BootKeyParser *p);

/* Feeds one byte from the serial stream. Returns true and fills `*outKey` once a complete key is
 * recognized: '1'-'9' -> DIGIT; '\r' or '\n' -> ENTER; ESC '[' A or ESC 'O' A -> UP; ...B -> DOWN;
 * ESC followed by anything else -> OTHER (that byte is consumed, not reprocessed); any other byte
 * -> OTHER. Returns false while mid-escape-sequence (ESC seen, waiting for '['/'O' or the
 * direction byte) -- the caller should keep feeding bytes until it gets true. No locks, boot-time
 * or host-test only; pure. */
bool bootKeyParserFeed(BootKeyParser *p, uint8_t byte, BootKey *outKey);

#endif
