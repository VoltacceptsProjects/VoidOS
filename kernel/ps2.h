#ifndef PS2_H
#define PS2_H

/* The keyboard and mouse are two logical devices (the "first" and
 * "second"/auxiliary PS/2 ports) sharing one 8042 controller and one
 * output buffer. This module owns that shared hardware: it enables the
 * mouse port and starts the mouse streaming movement packets, then on
 * every poll drains whatever bytes are waiting and routes each one to
 * keyboard_feed_byte() or mouse_feed_byte() based on which port it came
 * from, since keyboard.c/mouse.c no longer touch the ports directly. */

/* Enables the auxiliary (mouse) port and tells the mouse to start
 * sending movement packets. Call once, after keyboard input already
 * works (BIOS/firmware leaves the keyboard port itself enabled). */
void ps2_init(void);

/* Drains every byte currently sitting in the controller's output
 * buffer (returns immediately once it's empty) and feeds each one to
 * the right decoder. Call this once per iteration of the main loop
 * before checking keyboard_poll_key() / mouse_get_state(). */
void ps2_poll(void);

#endif
