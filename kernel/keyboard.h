#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

/* Minimal PS/2 keyboard decoder. This OS has no IDT/IRQs set up (it runs
 * with interrupts disabled), so the 8042 controller's output buffer is
 * drained by polling - see ps2.h. Because the same controller also
 * carries mouse bytes once the mouse is enabled, this module no longer
 * reads the hardware ports itself: ps2_poll() reads each byte and hands
 * keyboard ones to keyboard_feed_byte(), which assembles them into key
 * events that keyboard_poll_key() hands out. Only the keys the viewer
 * needs are decoded; everything else is read and discarded. */

enum key {
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_ENTER,
    KEY_ESC,
};

/* Feeds one raw scancode byte from the 8042 controller into the
 * decoder. Called by ps2_poll() - not meant to be called directly. */
void keyboard_feed_byte(uint8_t code);

/* Non-blocking: returns the next decoded key, or KEY_NONE if nothing
 * new has been decoded since the last call. Call ps2_poll() first each
 * time round the main loop to make sure any waiting bytes have been
 * fed in. */
enum key keyboard_poll_key(void);

#endif
