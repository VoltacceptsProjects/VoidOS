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

/* Routes a byte to the mouse (0xD4 tells the controller "the next data
 * byte is for the auxiliary device") and waits for its 0xFA ack. */
static void mouse_send_command(uint8_t cmd) {
    write_cmd(0xD4);
    write_data(cmd);
    read_data(); /* discard the 0xFA ack (or whatever came back) */
}

void ps2_init(void) {
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

    mouse_send_command(0xF6); /* set defaults */
    mouse_send_command(0xF4); /* enable data reporting (start streaming packets) */
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
