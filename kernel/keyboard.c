#include "keyboard.h"

/* Tiny incremental scancode decoder: keyboard_feed_byte() is called once
 * per raw byte (from ps2_poll()) and keeps just enough state (whether
 * we're in the middle of an 0xE0-prefixed extended sequence) to turn
 * make-codes into `enum key` values, dropping everything else -
 * releases, and keys the viewer doesn't use. */

static int expecting_extended = 0;
static enum key decoded = KEY_NONE;

void keyboard_feed_byte(uint8_t code) {
    if (code == 0xE0) {
        expecting_extended = 1;
        return;
    }

    int release = code & 0x80;
    uint8_t base = code & 0x7F;

    if (expecting_extended) {
        expecting_extended = 0;
        if (release) return; /* releases are of no interest */
        switch (base) {
            case 0x48: decoded = KEY_UP; break;
            case 0x50: decoded = KEY_DOWN; break;
            case 0x4B: decoded = KEY_LEFT; break;
            case 0x4D: decoded = KEY_RIGHT; break;
            case 0x49: decoded = KEY_PAGE_UP; break;
            case 0x51: decoded = KEY_PAGE_DOWN; break;
            case 0x47: decoded = KEY_HOME; break;
            case 0x4F: decoded = KEY_END; break;
            default: break; /* some other extended key: ignore */
        }
        return;
    }

    if (release) return;
    if (base == 0x01) { decoded = KEY_ESC; return; }
    if (base == 0x1C) { decoded = KEY_ENTER; return; }
    /* Any other key: not used by the viewer, dropped. */
}

enum key keyboard_poll_key(void) {
    enum key k = decoded;
    decoded = KEY_NONE;
    return k;
}
