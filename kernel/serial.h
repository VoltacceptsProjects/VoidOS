#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

/* Minimal 16550-compatible UART driver on COM1 (0x3F8), the port every
 * BIOS/QEMU/real PC chipset exposes by default. This exists purely as
 * a debug channel: once real WiFi (or any other) driver bring-up
 * starts touching hardware registers directly, VGA text output isn't
 * enough to diagnose a hang or a bad register read - you need a log
 * that keeps flowing even if the screen never updates again.
 *
 * On real hardware, wire a USB-to-serial (TTL, 3.3V/5V - NOT RS232
 * voltage directly) adapter to the board's UART header/pins if it has
 * one; if not, a PCIe/PCI serial card also shows up as COM1 as long as
 * nothing else claims 0x3F8 first. In QEMU, add "-serial stdio" to see
 * this output straight in your terminal - no hardware needed to test
 * this file at all. */

void serial_init(void);
void serial_write_char(char c);
void serial_writestring(const char* str);
void serial_write_hex(uint32_t value);
void serial_write_uint(uint32_t value);

#endif
