#include "ps2.h"
#include "keyboard.h"
#include "mouse.h"
#include "io.h"
#include <stdint.h>

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64

#define PS2_STATUS_OUTPUT_FULL 0x01 /* set: a byte is waiting to be read from PS2_DATA_PORT */
#define PS2_STATUS_INPUT_FULL  0x02 /* set: controller hasn't consumed our last command/data byte yet */
#define PS2_STATUS_AUX_DATA    0x20 /* set: the waiting byte came from the mouse, not the keyboard */

static void wait_input_clear(void) {
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL) {
    }
}

static void wait_output_full(void) {
    while (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) {
    }
}

static uint8_t read_data(void) {
    wait_output_full();
    return inb(PS2_DATA_PORT);
}

static void write_cmd(uint8_t cmd) {
    wait_input_clear();
    outb(PS2_CMD_PORT, cmd);
}

static void write_data(uint8_t val) {
    wait_input_clear();
    outb(PS2_DATA_PORT, val);
}

#define PS2_ACK 0xFA

/* Drains any byte(s) the controller already has queued up. Firmware can
 * leave a stray keyboard or (with USB legacy emulation) mouse byte
 * sitting in the output buffer before we ever touch the device; if we
 * don't clear that out first, the ack-read below can swallow the wrong
 * byte and leave the real ack to be picked up later by ps2_poll() as if
 * it were a movement packet - and since 0xFA has the "packet start" bit
 * set, mouse_feed_byte() happily accepts it, permanently shifting every
 * packet after it one byte out of phase. That single misread is what
 * turns into a cursor that never stops "trailing" and clicks that never
 * register: X/Y deltas and the button-state byte end up swapped. */
static void flush_output_buffer(void) {
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
        (void)inb(PS2_DATA_PORT);
    }
}

/* Routes a byte to the mouse (0xD4 tells the controller "the next data
 * byte is for the auxiliary device") and waits for its 0xFA ack.
 * Unlike a bare read_data() this doesn't just trust that whatever comes
 * back first is the ack - it discards anything else (stray data bytes,
 * resend requests, ...) until the real ack shows up, so a byte that was
 * already in flight can't get mistaken for it and left to corrupt the
 * packet stream later. Bounded so a genuinely dead/absent mouse can't
 * hang boot. */
static void mouse_send_command(uint8_t cmd) {
    write_cmd(0xD4);
    write_data(cmd);
    for (int tries = 0; tries < 8; tries++) {
        uint8_t b = read_data();
        if (b == PS2_ACK) return;
    }
}

void ps2_init(void) {
    /* Disable both ports first so nothing can inject a byte mid-setup,
     * then flush whatever the firmware already left queued up (see
     * flush_output_buffer() above for why this matters). */
    write_cmd(0xAD); /* disable first port (keyboard) */
    write_cmd(0xA7); /* disable second port (mouse) */
    flush_output_buffer();

    /* Enable the auxiliary device port - firmware normally leaves it
     * disabled since most keyboards-only setups never touch it. */
    write_cmd(0xA8);

    /* The controller's configuration byte has a "disable auxiliary
     * clock" bit that also needs clearing, or the mouse won't be able
     * to talk even though its port is enabled. */
    write_cmd(0x20); /* "read configuration byte" */
    uint8_t config = read_data();
    config &= (uint8_t)~0x20; /* clear: enable the auxiliary clock */
    write_cmd(0x60); /* "write configuration byte" */
    write_data(config);

    /* Re-enable the keyboard port we disabled above. */
    write_cmd(0xAE);

    mouse_send_command(0xF6); /* set defaults */
    mouse_send_command(0xF4); /* enable data reporting (start streaming packets) */

    /* Belt and suspenders: drop anything left over from negotiation
     * (e.g. a resend byte counted against the retry budget above)
     * so mouse_feed_byte() starts genuinely clean on the first real
     * movement packet. */
    flush_output_buffer();
}

void ps2_poll(void) {
    for (;;) {
        uint8_t status = inb(PS2_STATUS_PORT);
        if (!(status & PS2_STATUS_OUTPUT_FULL)) break; /* nothing waiting */
        uint8_t byte = inb(PS2_DATA_PORT);
        if (status & PS2_STATUS_AUX_DATA) {
            mouse_feed_byte(byte);
        } else {
            keyboard_feed_byte(byte);
        }
    }
}
