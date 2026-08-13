#include "mouse.h"

static struct mouse_state state = {0, 0, 0, 0, 0, 0};
static int32_t bound_x = 1023, bound_y = 767;
static uint8_t packet[3];
static int packet_index = 0;

void mouse_set_bounds(int32_t max_x, int32_t max_y) {
    bound_x = (max_x > 0) ? max_x : 0;
    bound_y = (max_y > 0) ? max_y : 0;
    if (state.x > bound_x) state.x = bound_x;
    if (state.y > bound_y) state.y = bound_y;
}

void mouse_feed_byte(uint8_t b) {
    /* Every standard PS/2 mouse packet's first byte has bit 3 set; if a
     * byte turns up out of sync with that (e.g. right after the driver
     * starts, before the first real packet boundary), drop it instead
     * of letting a whole packet be read one byte out of phase. */
    if (packet_index == 0 && !(b & 0x08)) {
        return;
    }

    packet[packet_index++] = b;
    if (packet_index < 3) return;
    packet_index = 0;

    uint8_t flags = packet[0];
    int dx = (int)(int8_t)packet[1];
    int dy = (int)(int8_t)packet[2];
    if (flags & 0x40) dx = 0; /* X overflow: the delta is garbage, drop it */
    if (flags & 0x80) dy = 0; /* Y overflow */

    int32_t nx = state.x + dx;
    int32_t ny = state.y - dy; /* PS/2 reports +Y as "up"; screen Y grows downward */
    if (nx < 0) nx = 0; else if (nx > bound_x) nx = bound_x;
    if (ny < 0) ny = 0; else if (ny > bound_y) ny = bound_y;
    if (nx != state.x || ny != state.y) state.moved = 1;
    state.x = nx;
    state.y = ny;

    uint8_t left = flags & 0x01;
    uint8_t right = flags & 0x02;
    /*
     * Treat the press edge as the one-shot click event.  The old release-edge
     * behavior made a click disappear when another packet arrived during the
     * screen redraw (or when a VM dropped the release packet).  UI actions
     * are idempotent and the button state is still exposed separately, so
     * dispatching on press keeps the desktop responsive without creating
     * duplicate clicks.
     */
    if (!state.left_down && left) state.left_clicked = 1;
    state.left_down = left;
    state.right_down = right;
}

const struct mouse_state* mouse_get_state(void) {
    return &state;
}

void mouse_clear_events(void) {
    state.moved = 0;
    state.left_clicked = 0;
}
