/* See bootkey.h. */
#include "include/bootkey.h"

#define ESC_BYTE 0x1Bu

void bootKeyParserInit(BootKeyParser *p) {
    p->state = BOOT_KEY_PARSE_IDLE;
}

static BootKey otherKey(void) {
    BootKey k = {BOOT_KEY_OTHER, 0};
    return k;
}

bool bootKeyParserFeed(BootKeyParser *p, uint8_t byte, BootKey *outKey) {
    switch (p->state) {
        case BOOT_KEY_PARSE_IDLE:
            if (byte == ESC_BYTE) {
                p->state = BOOT_KEY_PARSE_GOT_ESC;
                return false;
            }
            if (byte >= '1' && byte <= '9') {
                *outKey = (BootKey){BOOT_KEY_DIGIT, (uint32_t)(byte - '0')};
                return true;
            }
            if (byte == '\r' || byte == '\n') {
                *outKey = (BootKey){BOOT_KEY_ENTER, 0};
                return true;
            }
            *outKey = otherKey();
            return true;

        case BOOT_KEY_PARSE_GOT_ESC:
            p->state = BOOT_KEY_PARSE_IDLE;
            if (byte == '[' || byte == 'O') {
                p->state = BOOT_KEY_PARSE_GOT_CSI;
                return false;
            }
            *outKey = otherKey();
            return true;

        case BOOT_KEY_PARSE_GOT_CSI:
        default:
            p->state = BOOT_KEY_PARSE_IDLE;
            if (byte == 'A') {
                *outKey = (BootKey){BOOT_KEY_UP, 0};
                return true;
            }
            if (byte == 'B') {
                *outKey = (BootKey){BOOT_KEY_DOWN, 0};
                return true;
            }
            *outKey = otherKey();
            return true;
    }
}
