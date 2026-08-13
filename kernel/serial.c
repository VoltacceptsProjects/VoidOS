#include "serial.h"
#include "io.h"

#define COM1 0x3F8

static int serial_ready = 0;

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* disable UART interrupts - we poll */
    outb(COM1 + 3, 0x80); /* enable DLAB to set the baud divisor */
    outb(COM1 + 0, 0x03); /* divisor low byte  -> 115200 / 3 = 38400 baud */
    outb(COM1 + 1, 0x00); /* divisor high byte */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit; DLAB off */
    outb(COM1 + 2, 0xC7); /* enable + clear 14-byte FIFO */
    outb(COM1 + 4, 0x0B); /* IRQs off, RTS/DSR set (required for the
                              UART to actually assert its output line) */

    /* Loopback self-test: write a byte and confirm the UART reflects
     * it back before trusting the port is real and wired up. Catches
     * "no UART present" (or -serial none in QEMU) cleanly instead of
     * silently dropping every future serial_writestring() call. */
    outb(COM1 + 4, 0x1E);        /* enable loopback mode */
    outb(COM1 + 0, 0xAE);        /* send test byte */
    serial_ready = (inb(COM1 + 0) == 0xAE);
    outb(COM1 + 4, 0x0F);        /* back to normal operation */
}

static int transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_write_char(char c) {
    if (!serial_ready) return;
    while (!transmit_empty()) { }
    if (c == '\n') {
        while (!transmit_empty()) { }
        outb(COM1, '\r'); /* most serial terminals want CRLF */
    }
    outb(COM1, (uint8_t)c);
}

void serial_writestring(const char* str) {
    if (!serial_ready) return;
    while (*str) serial_write_char(*str++);
}

void serial_write_hex(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buf[11];
    buf[0] = '0';
    buf[1] = 'x';
    buf[10] = '\0';
    for (int i = 0; i < 8; i++) {
        buf[9 - i] = digits[value & 0xF];
        value >>= 4;
    }
    serial_writestring(buf);
}

void serial_write_uint(uint32_t value) {
    char buf[11];
    int i = 10;
    buf[i--] = '\0';
    if (value == 0) {
        buf[i--] = '0';
    } else {
        while (value > 0 && i >= 0) {
            buf[i--] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    serial_writestring(&buf[i + 1]);
}
