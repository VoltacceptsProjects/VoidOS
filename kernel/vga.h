#ifndef VGA_H
#define VGA_H

#include <stdint.h>
#include <stddef.h>

struct multiboot_info;

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE = 1, VGA_GREEN = 2, VGA_CYAN = 3,
    VGA_RED = 4, VGA_MAGENTA = 5, VGA_BROWN = 6, VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8, VGA_LIGHT_BLUE = 9, VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11, VGA_LIGHT_RED = 12, VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN = 14, VGA_WHITE = 15,
};

/* mbi may be NULL (or lack framebuffer info) - in that case we fall back
 * to the legacy 0xB8000 VGA text buffer, which only exists on BIOS/CSM
 * boots. On UEFI, mbi must carry valid framebuffer_* fields (GRUB fills
 * these in because our Multiboot header requests a video mode) or there
 * is nowhere to draw. */
void terminal_initialize(struct multiboot_info* mbi);

/* Writing: these append to an in-memory scrollback buffer. Nothing is
 * drawn to the screen until terminal_render_from() is called - that's
 * what makes scrolling back through already-written content possible. */
void terminal_setcolor(uint8_t fg, uint8_t bg);
void terminal_putchar(char c);
void terminal_writestring(const char* data);
void terminal_write_hex32(uint32_t n);
void terminal_write_hex16(uint16_t n);
void terminal_write_uint(uint32_t n);
void terminal_write_uint64(uint64_t n);
void terminal_newline(void);

/* Scrolling / rendering: draws scrollback lines [top_line, top_line +
 * terminal_visible_rows()) to the screen, plus a one-line status bar
 * beneath them showing position and key hints. */
void terminal_render_from(size_t top_line);
void terminal_render_section(size_t top_line, size_t section_first, size_t section_last);
size_t terminal_line_count(void);   /* total lines written so far */
size_t terminal_visible_rows(void); /* content rows that fit on screen (status bar excluded) */
size_t terminal_max_top_line(void); /* largest top_line that still fills the screen */

/* Shrinks the text/scrollback grid into a pixel rectangle (framebuffer
 * mode only), so kernel/ui.c can put a text "card" beside a sidebar and
 * below a top bar instead of always starting at the top-left corner.
 * No-op in legacy VGA text mode. Call once after terminal_initialize()
 * and after drawing any chrome that should sit behind the text. */
void terminal_set_viewport(uint32_t px, uint32_t py, uint32_t pw, uint32_t ph);

/* ---- raw pixel drawing (framebuffer mode only; no-ops otherwise) ------
 * Lets kernel/ui.c paint VoidOS's GUI chrome - top bar, sidebar, cards,
 * simple icons - directly onto the framebuffer. */
int      gfx_available(void);
uint32_t gfx_screen_width(void);
uint32_t gfx_screen_height(void);
uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b);
uint32_t gfx_palette_color(uint8_t index); /* one of the enum vga_color indices above */
uint32_t gfx_get_pixel(uint32_t x, uint32_t y); /* reads a pixel back, e.g. to save it before drawing over it */
void     gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     gfx_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color);
void     gfx_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color);
void     gfx_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     gfx_fill_card(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color, uint32_t radius);
void     gfx_fill_circle(uint32_t cx, uint32_t cy, uint32_t radius, uint32_t color);

/* Free-standing text (GUI chrome, not report content) - see vga.c for
 * the transparent-background trick (pass fg == bg). */
void     gfx_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg, uint32_t scale);
void     gfx_draw_text(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg, uint32_t scale);
uint32_t gfx_text_width(const char* s, uint32_t scale);

#endif
