#include "keyboard.h"

/* Tiny incremental scancode decoder: keyboard_feed_byte() is called once
 * per raw byte (from ps2_poll()) and keeps just enough state (whether
 * we're in the middle of an 0xE0-prefixed extended sequence) to turn
 * make-codes into `enum key` values, dropping everything else -
 * releases, and keys the viewer doesn't use. */

static int expecting_extended = 0;
static enum key decoded = KEY_NONE;
static int shift_down = 0;

#define CHAR_QUEUE_SIZE 64
static char char_queue[CHAR_QUEUE_SIZE];
static unsigned char char_head = 0;
static unsigned char char_tail = 0;

static void queue_char(char c) {
    unsigned char next = (unsigned char)((char_tail + 1) % CHAR_QUEUE_SIZE);
    if (next == char_head) return; /* drop input rather than overwrite it */
    char_queue[char_tail] = c;
    char_tail = next;
}

static char shifted_char(uint8_t base) {
    static const char normal[] =
        "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
    static const char shifted[] =
        "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
    static const uint8_t scan[] = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
        0x0C, 0x0D, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23,
        0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2B, 0x2C, 0x2D, 0x2E,
        0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
    };
    for (unsigned int i = 0; i < sizeof(scan); i++) {
        if (scan[i] == base) return shift_down ? shifted[i] : normal[i];
    }
    if (base == 0x39) return ' ';
    return 0;
}

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

    if (base == 0x2A || base == 0x36) {
        shift_down = !release;
        return;
    }

    if (release) return;
    if (base == 0x01) { decoded = KEY_ESC; return; }
    if (base == 0x1C) { decoded = KEY_ENTER; return; }
    if (base == 0x0E) { decoded = KEY_BACKSPACE; return; }

    char printable = shifted_char(base);
    if (printable) queue_char(printable);
}

enum key keyboard_poll_key(void) {
    enum key k = decoded;
    decoded = KEY_NONE;
    return k;
}

char keyboard_poll_char(void) {
    if (char_head == char_tail) return 0;
    char c = char_queue[char_head];
    char_head = (unsigned char)((char_head + 1) % CHAR_QUEUE_SIZE);
    return c;
}
