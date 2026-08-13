#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

/* Standard 3-byte PS/2 mouse packet decoder. Like the keyboard, this OS
 * has no interrupts set up, so mouse_feed_byte() is called by ps2_poll()
 * for each raw byte read from the controller, and assembles them into
 * an absolute, screen-clamped cursor position plus button state that
 * the main loop can query with mouse_get_state(). */

struct mouse_state {
    int32_t x, y;          /* absolute position, clamped by mouse_set_bounds() */
    uint8_t left_down;      /* current button state */
    uint8_t right_down;
    uint8_t moved;          /* position changed since the last mouse_clear_events() */
    uint8_t left_clicked;   /* left button was released (a completed click) since last clear */
};

/* Clamps future cursor positions to [0, max_x] x [0, max_y] - normally
 * the screen's pixel dimensions minus one. Call once after the video
 * mode is known, before the main loop starts polling. */
void mouse_set_bounds(int32_t max_x, int32_t max_y);

/* Feeds one raw byte from the 8042 controller's auxiliary (mouse)
 * stream into the packet assembler. Called by ps2_poll() - not meant
 * to be called directly. */
void mouse_feed_byte(uint8_t b);

/* Current state. The returned pointer is to static storage that
 * mouse_feed_byte() updates in place - read it before the next
 * ps2_poll() call if you need a stable snapshot. */
const struct mouse_state* mouse_get_state(void);

/* Clears the one-shot `moved` / `left_clicked` flags after the main
 * loop has acted on them for this iteration. */
void mouse_clear_events(void);

#endif
