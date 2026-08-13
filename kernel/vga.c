#include "vga.h"
#include "io.h"
#include "font8x16.h"
#include "multiboot.h"

/* ---- backend selection ------------------------------------------------ */

enum term_mode {
    TERM_MODE_VGA_TEXT,   /* legacy 0xB8000 16-color text buffer (BIOS only) */
    TERM_MODE_FRAMEBUFFER /* linear RGB framebuffer, drawn via font8x16 (UEFI/BIOS+VBE) */
};

static enum term_mode mode;

/* -- legacy text-mode state -- */
static const size_t VGA_TEXT_WIDTH = 80;
static const size_t VGA_TEXT_HEIGHT = 25;
static uint16_t* const VGA_TEXT_MEMORY = (uint16_t*) 0xB8000;

/* -- framebuffer state -- */
static uint8_t*  fb_base;
static uint32_t  fb_pitch;
static uint32_t  fb_width;
static uint32_t  fb_height;
static uint8_t   fb_bpp;
static uint8_t   fb_red_pos,   fb_red_size;
static uint8_t   fb_green_pos, fb_green_size;
static uint8_t   fb_blue_pos,  fb_blue_size;
static size_t    fb_cols;   /* text columns  = fb_width  / 8  */
static size_t    fb_rows;   /* text rows     = fb_height / 16 */

/* -- shared layout: how many columns/rows of *content* the active
 * backend offers. One row is always reserved beneath that for a status
 * bar showing scroll position and key hints. -- */
static size_t content_cols;
static size_t content_rows; /* rows available for report text, status bar excluded */

/* -- viewport: pixel offset (framebuffer mode only) at which the text
 * content grid above is drawn. Lets kernel.c carve out a "card" region
 * to the right of a sidebar / below a top bar for VoidOS's GUI shell,
 * instead of the text grid always starting at the top-left corner of
 * the screen. Defaults to (0,0) covering the whole screen. -- */
static uint32_t viewport_x, viewport_y;

/* -- scrollback buffer --------------------------------------------------
 * Every character ever written goes here first; nothing touches the
 * screen until terminal_render_from() draws a window into this buffer.
 * That's what lets Up/Down/PageUp/PageDown/Home/End scroll back through
 * output that has already scrolled past. */
#define MAX_LINES 500
#define MAX_COLS  256

struct term_cell {
    char ch;
    uint8_t color; /* fg | bg << 4 */
};

static struct term_cell scrollback[MAX_LINES][MAX_COLS];
static uint8_t row_ready[MAX_LINES]; /* has this row been cleared/initialized yet? */
static size_t total_lines = 1; /* at least the (empty) first line exists */

/* -- write cursor (position within the scrollback buffer, not the screen) -- */
static size_t buf_row;
static size_t buf_col;
static uint8_t term_fg;
static uint8_t term_bg;

/* VoidOS palette: the classic 16 VGA color slots stay (so every existing
 * terminal_setcolor(VGA_x, ...) call site keeps working unmodified), but
 * the RGB values behind them are remapped to a cohesive modern dark
 * theme instead of raw CGA colors. On the legacy text-mode backend these
 * indices still map to the real hardware 16-color palette (it can't be
 * changed), so that path renders the classic look while the framebuffer
 * path renders VoidOS's flat dark theme. */
static const uint8_t vga_palette[16][3] = {
    {0x1E,0x1E,0x2E}, /* VGA_BLACK   -> app background      */
    {0x89,0xB4,0xFA}, /* VGA_BLUE    -> accent blue          */
    {0xA6,0xE3,0xA1}, /* VGA_GREEN   -> success green        */
    {0x94,0xE2,0xD5}, /* VGA_CYAN    -> teal accent          */
    {0xF3,0x8B,0xA8}, /* VGA_RED     -> soft red             */
    {0xCB,0xA6,0xF7}, /* VGA_MAGENTA -> lavender             */
    {0xF9,0xE2,0xAF}, /* VGA_BROWN   -> amber                */
    {0xBA,0xC2,0xDE}, /* VGA_LIGHT_GREY  -> body text        */
    {0x31,0x32,0x44}, /* VGA_DARK_GREY   -> card background  */
    {0x89,0xB4,0xFA}, /* VGA_LIGHT_BLUE  -> accent blue       */
    {0xA6,0xE3,0xA1}, /* VGA_LIGHT_GREEN -> success green     */
    {0x94,0xE2,0xD5}, /* VGA_LIGHT_CYAN  -> section headings  */
    {0xF3,0x8B,0xA8}, /* VGA_LIGHT_RED   -> warnings          */
    {0xF5,0xC2,0xE7}, /* VGA_LIGHT_MAGENTA -> pink accent     */
    {0xF9,0xE2,0xAF}, /* VGA_LIGHT_BROWN -> amber             */
    {0xCD,0xD6,0xF4}, /* VGA_WHITE   -> bright text/headings  */
};

static inline uint8_t vga_entry_color(uint8_t fg, uint8_t bg) {
    return fg | (uint8_t)(bg << 4);
}

static inline uint16_t vga_entry(unsigned char c, uint8_t color) {
    return (uint16_t) c | (uint16_t) color << 8;
}

static void update_cursor(size_t row, size_t col) {
    if (mode != TERM_MODE_VGA_TEXT) return; /* no hardware cursor on fb */
    uint16_t pos = (uint16_t)(row * VGA_TEXT_WIDTH + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

/* ---- framebuffer pixel/glyph helpers ---------------------------------- */

static inline uint32_t fb_pack_color(uint8_t index) {
    uint8_t r = vga_palette[index][0];
    uint8_t g = vga_palette[index][1];
    uint8_t b = vga_palette[index][2];
    return ((uint32_t)r << fb_red_pos) | ((uint32_t)g << fb_green_pos) | ((uint32_t)b << fb_blue_pos);
}

uint32_t gfx_get_pixel(uint32_t x, uint32_t y) {
    if (mode != TERM_MODE_FRAMEBUFFER) return 0;
    if (x >= fb_width || y >= fb_height) return 0;
    uint8_t* p = fb_base + y * fb_pitch + x * (fb_bpp / 8);
    switch (fb_bpp) {
        case 32:
            return *(uint32_t*)p;
        case 24:
            return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
        case 16:
            return *(uint16_t*)p;
        default:
            return 0;
    }
}

static inline void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    uint8_t* p = fb_base + y * fb_pitch + x * (fb_bpp / 8);
    switch (fb_bpp) {
        case 32:
            *(uint32_t*)p = color;
            break;
        case 24:
            p[0] = (uint8_t)(color & 0xFF);
            p[1] = (uint8_t)((color >> 8) & 0xFF);
            p[2] = (uint8_t)((color >> 16) & 0xFF);
            break;
        case 16:
            *(uint16_t*)p = (uint16_t)color;
            break;
        default:
            break; /* unsupported depth: nothing we can safely draw */
    }
}

static void fb_draw_glyph(size_t col, size_t screen_row, unsigned char c, uint8_t fg, uint8_t bg) {
    uint32_t fgc = fb_pack_color(fg);
    uint32_t bgc = fb_pack_color(bg);
    const uint8_t* glyph;
    if (c >= 0x20 && c <= 0x7E) {
        glyph = font8x16[c - 0x20];
    } else {
        glyph = font8x16['?' - 0x20];
    }
    uint32_t ox = viewport_x + (uint32_t)(col * 8);
    uint32_t oy = viewport_y + (uint32_t)(screen_row * 16);
    for (uint32_t y = 0; y < 16; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < 8; x++) {
            int on = (bits >> (7 - x)) & 1;
            fb_put_pixel(ox + x, oy + y, on ? fgc : bgc);
        }
    }
}

/* ---- raw pixel drawing API (framebuffer mode only) ---------------------
 * Exposed via vga.h so kernel/ui.c can paint the VoidOS chrome (top bar,
 * sidebar, cards, icons) directly onto the framebuffer, independent of
 * the text/scrollback grid above. All are no-ops in legacy text mode. */

int gfx_available(void) {
    return mode == TERM_MODE_FRAMEBUFFER;
}

uint32_t gfx_screen_width(void)  { return mode == TERM_MODE_FRAMEBUFFER ? fb_width  : 0; }
uint32_t gfx_screen_height(void) { return mode == TERM_MODE_FRAMEBUFFER ? fb_height : 0; }

uint32_t gfx_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (mode != TERM_MODE_FRAMEBUFFER) return 0;
    return ((uint32_t)r << fb_red_pos) | ((uint32_t)g << fb_green_pos) | ((uint32_t)b << fb_blue_pos);
}

uint32_t gfx_palette_color(uint8_t index) {
    if (mode != TERM_MODE_FRAMEBUFFER) return 0;
    return fb_pack_color(index);
}

void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (mode != TERM_MODE_FRAMEBUFFER) return;
    if (x >= fb_width || y >= fb_height) return;
    uint32_t x1 = x + w; if (x1 > fb_width) x1 = fb_width;
    uint32_t y1 = y + h; if (y1 > fb_height) y1 = fb_height;
    for (uint32_t yy = y; yy < y1; yy++)
        for (uint32_t xx = x; xx < x1; xx++)
            fb_put_pixel(xx, yy, color);
}

void gfx_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color) {
    gfx_fill_rect(x, y, w, 1, color);
}

void gfx_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color) {
    gfx_fill_rect(x, y, 1, h, color);
}

void gfx_rect_outline(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (mode != TERM_MODE_FRAMEBUFFER || w == 0 || h == 0) return;
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

/* Rounded-corner-ish card: a filled rect with the four corner pixels
 * clipped off in a couple of steps, which reads as "softly rounded" at
 * screen resolution without needing real arc math. */
void gfx_fill_card(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color, uint32_t radius) {
    if (mode != TERM_MODE_FRAMEBUFFER) return;
    if (radius > h / 2) radius = h / 2;
    if (radius > w / 2) radius = w / 2;
    gfx_fill_rect(x, y + radius, w, h - 2 * radius, color);
    for (uint32_t r = 0; r < radius; r++) {
        /* how far to inset this row so the corner looks chamfered */
        uint32_t inset = radius - r;
        if (inset > radius) inset = radius;
        uint32_t shrink = (inset * 2) / 3; /* soften the diagonal a bit */
        gfx_hline(x + shrink, y + r, w - 2 * shrink, color);
        gfx_hline(x + shrink, y + h - 1 - r, w - 2 * shrink, color);
    }
}

void gfx_fill_circle(uint32_t cx, uint32_t cy, uint32_t radius, uint32_t color) {
    if (mode != TERM_MODE_FRAMEBUFFER) return;
    int r = (int)radius;
    for (int dy = -r; dy <= r; dy++) {
        int dx = (int)(radius * radius) - dy * dy;
        if (dx < 0) continue;
        int half = 0;
        while ((half + 1) * (half + 1) <= dx) half++;
        gfx_fill_rect((uint32_t)((int)cx - half), (uint32_t)((int)cy + dy), (uint32_t)(2 * half + 1), 1, color);
    }
}

/* Free-standing text drawing at an arbitrary pixel position, independent
 * of the scrollback/viewport grid above - used for GUI chrome (sidebar
 * labels, top bar wordmark) rather than report content. `scale` repeats
 * each glyph pixel into a scale x scale block (2 looks good for
 * headings). Pass bg == fg to draw with a transparent background
 * (paints only the "on" pixels, leaving whatever is already there, so
 * text can sit on top of a colored panel). */
void gfx_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg, uint32_t scale) {
    if (mode != TERM_MODE_FRAMEBUFFER) return;
    if (scale == 0) scale = 1;
    const uint8_t* glyph;
    if (c >= 0x20 && c <= 0x7E) {
        glyph = font8x16[(unsigned char)c - 0x20];
    } else {
        glyph = font8x16['?' - 0x20];
    }
    int transparent = (fg == bg);
    for (uint32_t gy = 0; gy < 16; gy++) {
        uint8_t bits = glyph[gy];
        for (uint32_t gx = 0; gx < 8; gx++) {
            int on = (bits >> (7 - gx)) & 1;
            if (!on && transparent) continue;
            uint32_t color = on ? fg : bg;
            gfx_fill_rect(x + gx * scale, y + gy * scale, scale, scale, color);
        }
    }
}

void gfx_draw_text(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg, uint32_t scale) {
    if (mode != TERM_MODE_FRAMEBUFFER) return;
    if (scale == 0) scale = 1;
    uint32_t cx = x;
    for (const char* p = s; *p; p++) {
        gfx_draw_char(cx, y, *p, fg, bg, scale);
        cx += 8 * scale;
    }
}

uint32_t gfx_text_width(const char* s, uint32_t scale) {
    if (scale == 0) scale = 1;
    size_t len = 0;
    for (const char* p = s; *p; p++) len++;
    return (uint32_t)(len * 8 * scale);
}

void terminal_set_viewport(uint32_t px, uint32_t py, uint32_t pw, uint32_t ph) {
    if (mode != TERM_MODE_FRAMEBUFFER) return;
    viewport_x = px;
    viewport_y = py;
    fb_cols = pw / 8;
    fb_rows = ph / 16;
    if (fb_cols > MAX_COLS) fb_cols = MAX_COLS;
    content_cols = fb_cols;
    content_rows = fb_rows > 0 ? fb_rows - 1 : 0; /* reserve bottom row for status bar */
}

/* ---- scrollback buffer helpers ----------------------------------------- */

static void ensure_row_ready(size_t row) {
    if (row >= MAX_LINES) return;
    if (!row_ready[row]) {
        struct term_cell blank = { ' ', vga_entry_color(term_fg, term_bg) };
        for (size_t x = 0; x < MAX_COLS; x++) scrollback[row][x] = blank;
        row_ready[row] = 1;
    }
    if (row + 1 > total_lines) total_lines = row + 1;
}

/* ---- public API --------------------------------------------------------
 * kernel.c doesn't need to know which backend ended up being used. */

void terminal_initialize(struct multiboot_info* mbi) {
    buf_row = 0;
    buf_col = 0;
    term_fg = VGA_LIGHT_GREY;
    term_bg = VGA_BLACK;
    total_lines = 1;
    for (size_t i = 0; i < MAX_LINES; i++) row_ready[i] = 0;

    if (mbi && (mbi->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO) &&
        mbi->framebuffer_type == MULTIBOOT_FRAMEBUFFER_TYPE_RGB &&
        (mbi->framebuffer_bpp == 32 || mbi->framebuffer_bpp == 24 || mbi->framebuffer_bpp == 16)) {
        mode = TERM_MODE_FRAMEBUFFER;
        fb_base   = (uint8_t*)(uintptr_t)mbi->framebuffer_addr;
        fb_pitch  = mbi->framebuffer_pitch;
        fb_width  = mbi->framebuffer_width;
        fb_height = mbi->framebuffer_height;
        fb_bpp    = mbi->framebuffer_bpp;
        fb_red_pos    = mbi->rgb.framebuffer_red_field_position;
        fb_red_size   = mbi->rgb.framebuffer_red_mask_size;
        fb_green_pos  = mbi->rgb.framebuffer_green_field_position;
        fb_green_size = mbi->rgb.framebuffer_green_mask_size;
        fb_blue_pos   = mbi->rgb.framebuffer_blue_field_position;
        fb_blue_size  = mbi->rgb.framebuffer_blue_mask_size;

        /* Some BIOS/VBE implementations (observed in QEMU/SeaBIOS on
         * this exact mode) hand back a garbled color_info block - a
         * zero-width channel is never valid, since every channel needs
         * at least one bit. When that happens, fall back to the
         * standard packed layout for the reported bit depth (by far
         * the most common one in practice: BGR order, blue in the low
         * byte) rather than trusting the corrupt field positions. */
        if (fb_red_size == 0 || fb_green_size == 0 || fb_blue_size == 0) {
            if (fb_bpp == 32 || fb_bpp == 24) {
                fb_red_pos = 16; fb_red_size = 8;
                fb_green_pos = 8; fb_green_size = 8;
                fb_blue_pos = 0; fb_blue_size = 8;
            } else if (fb_bpp == 16) {
                fb_red_pos = 11; fb_red_size = 5;
                fb_green_pos = 5; fb_green_size = 6;
                fb_blue_pos = 0; fb_blue_size = 5;
            }
        }
        (void)fb_red_size; (void)fb_green_size; (void)fb_blue_size;
        fb_cols = fb_width / 8;
        fb_rows = fb_height / 16;
        if (fb_cols > MAX_COLS) fb_cols = MAX_COLS;

        uint32_t bgc = fb_pack_color(term_bg);
        for (uint32_t y = 0; y < fb_height; y++)
            for (uint32_t x = 0; x < fb_width; x++)
                fb_put_pixel(x, y, bgc);

        content_cols = fb_cols;
        content_rows = fb_rows > 0 ? fb_rows - 1 : 0; /* reserve bottom row for status bar */
    } else {
        /* No usable framebuffer - assume legacy BIOS gave us a real
         * 0xB8000 text buffer. On a UEFI machine with no framebuffer
         * info this backend will not actually work (nothing is mapped
         * at 0xB8000), but there's nowhere else left to draw. */
        mode = TERM_MODE_VGA_TEXT;
        for (size_t y = 0; y < VGA_TEXT_HEIGHT; y++) {
            for (size_t x = 0; x < VGA_TEXT_WIDTH; x++) {
                VGA_TEXT_MEMORY[y * VGA_TEXT_WIDTH + x] = vga_entry(' ', vga_entry_color(term_fg, term_bg));
            }
        }
        content_cols = VGA_TEXT_WIDTH;
        content_rows = VGA_TEXT_HEIGHT - 1; /* reserve bottom row for status bar */
    }

    ensure_row_ready(0);
}

void terminal_setcolor(uint8_t fg, uint8_t bg) {
    term_fg = fg;
    term_bg = bg;
}

void terminal_newline(void) {
    buf_col = 0;
    if (buf_row + 1 < MAX_LINES) buf_row++;
    ensure_row_ready(buf_row);
}

void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_newline();
        return;
    }

    ensure_row_ready(buf_row);
    if (buf_col < MAX_COLS && buf_row < MAX_LINES) {
        scrollback[buf_row][buf_col].ch = c;
        scrollback[buf_row][buf_col].color = vga_entry_color(term_fg, term_bg);
    }
    buf_col++;
    if (buf_col >= content_cols) {
        terminal_newline();
    }
}

void terminal_writestring(const char* data) {
    for (size_t i = 0; data[i] != '\0'; i++) {
        terminal_putchar(data[i]);
    }
}

void terminal_write_hex32(uint32_t n) {
    char buf[11];
    const char* hex = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 8; i++) {
        buf[9 - i] = hex[n & 0xF];
        n >>= 4;
    }
    buf[10] = '\0';
    terminal_writestring(buf);
}

void terminal_write_hex16(uint16_t n) {
    char buf[7];
    const char* hex = "0123456789ABCDEF";
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 4; i++) {
        buf[5 - i] = hex[n & 0xF];
        n >>= 4;
    }
    buf[6] = '\0';
    terminal_writestring(buf);
}

void terminal_write_uint(uint32_t n) {
    char buf[11];
    int i = 10;
    buf[i--] = '\0';
    if (n == 0) {
        buf[i--] = '0';
    } else {
        while (n > 0 && i >= 0) {
            buf[i--] = (char)('0' + (n % 10));
            n /= 10;
        }
    }
    terminal_writestring(&buf[i + 1]);
}

void terminal_write_uint64(uint64_t n) {
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    if (n == 0) {
        buf[i--] = '0';
    } else {
        while (n > 0 && i >= 0) {
            buf[i--] = (char)('0' + (int)(n % 10));
            n /= 10;
        }
    }
    terminal_writestring(&buf[i + 1]);
}

/* ---- scrolling / rendering ---------------------------------------------
 * These draw a window of the scrollback buffer to the actual screen.
 * Nothing above this point in the file ever draws to the screen. */

size_t terminal_line_count(void) {
    return total_lines;
}

size_t terminal_visible_rows(void) {
    return content_rows;
}

size_t terminal_max_top_line(void) {
    if (total_lines <= content_rows) return 0;
    return total_lines - content_rows;
}

static void draw_text_row(size_t screen_row, const struct term_cell* cells, size_t ncells) {
    if (mode == TERM_MODE_FRAMEBUFFER) {
        for (size_t x = 0; x < content_cols; x++) {
            char ch = (x < ncells) ? cells[x].ch : ' ';
            uint8_t color = (x < ncells) ? cells[x].color : vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
            fb_draw_glyph(x, screen_row, (unsigned char)ch, color & 0x0F, (color >> 4) & 0x0F);
        }
    } else {
        for (size_t x = 0; x < content_cols; x++) {
            char ch = (x < ncells) ? cells[x].ch : ' ';
            uint8_t color = (x < ncells) ? cells[x].color : vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
            VGA_TEXT_MEMORY[screen_row * VGA_TEXT_WIDTH + x] = vga_entry((unsigned char)ch, color);
        }
    }
}

/* Tiny in-place decimal formatter for the status bar (avoids touching
 * the scrollback buffer, which terminal_write_uint would do). */
static size_t fmt_uint(char* out, size_t n) {
    char tmp[11];
    int i = 10;
    tmp[i--] = '\0';
    if (n == 0) {
        tmp[i--] = '0';
    } else {
        while (n > 0 && i >= 0) {
            tmp[i--] = (char)('0' + (n % 10));
            n /= 10;
        }
    }
    size_t len = 0;
    for (int j = i + 1; j < 10; j++) out[len++] = tmp[j];
    out[len] = '\0';
    return len;
}

static void draw_status_bar(size_t screen_row, size_t top_line) {
    static char text[MAX_COLS + 1];
    size_t pos = 0;
    size_t first = top_line + 1; /* 1-indexed for humans */
    size_t last = top_line + content_rows;
    if (last > total_lines) last = total_lines;

    const char* prefix = "-- Lines ";
    for (const char* p = prefix; *p; p++) text[pos++] = *p;
    pos += fmt_uint(&text[pos], first);
    text[pos++] = '-';
    pos += fmt_uint(&text[pos], last);
    const char* mid = " of ";
    for (const char* p = mid; *p; p++) text[pos++] = *p;
    pos += fmt_uint(&text[pos], total_lines);

    const char* hint;
    if (top_line == 0 && terminal_max_top_line() == 0) {
        hint = "  (whole report fits on screen) --";
    } else if (top_line == 0) {
        hint = "  (Up/Down PgUp/PgDn Home/End to scroll) --";
    } else if (top_line >= terminal_max_top_line()) {
        hint = "  (at end - Up/PgUp/Home to scroll back) --";
    } else {
        hint = "  (Up/Down PgUp/PgDn Home/End to scroll) --";
    }
    for (const char* p = hint; *p && pos < MAX_COLS; p++) text[pos++] = *p;
    text[pos] = '\0';

    uint8_t color = vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY); /* inverted: stands out as a status bar */
    if (mode == TERM_MODE_FRAMEBUFFER) {
        for (size_t x = 0; x < content_cols; x++) {
            char ch = (x < pos) ? text[x] : ' ';
            fb_draw_glyph(x, screen_row, (unsigned char)ch, VGA_BLACK, VGA_LIGHT_GREY);
        }
    } else {
        for (size_t x = 0; x < content_cols; x++) {
            char ch = (x < pos) ? text[x] : ' ';
            VGA_TEXT_MEMORY[screen_row * VGA_TEXT_WIDTH + x] = vga_entry((unsigned char)ch, color);
        }
    }
}

void terminal_render_from(size_t top_line) {
    size_t max_top = terminal_max_top_line();
    if (top_line > max_top) top_line = max_top;

    for (size_t r = 0; r < content_rows; r++) {
        size_t line = top_line + r;
        if (line < total_lines) {
            draw_text_row(r, scrollback[line], content_cols);
        } else {
            draw_text_row(r, (const struct term_cell*)0, 0);
        }
    }
    draw_status_bar(content_rows, top_line);
    update_cursor(content_rows, 0);
}

static void draw_status_bar_section(size_t screen_row, size_t top_line,
                                     size_t section_first, size_t section_last) {
    static char text[MAX_COLS + 1];
    size_t pos = 0;
    size_t section_count = section_last - section_first + 1;
    size_t first = top_line - section_first + 1; /* 1-indexed, relative to section */
    size_t last = first + content_rows - 1;
    if (last > section_count) last = section_count;
    size_t section_max_top = (section_count > content_rows) ? section_last - content_rows + 1 : section_first;

    const char* prefix = "-- Lines ";
    for (const char* p = prefix; *p; p++) text[pos++] = *p;
    pos += fmt_uint(&text[pos], first);
    text[pos++] = '-';
    pos += fmt_uint(&text[pos], last);
    const char* mid = " of ";
    for (const char* p = mid; *p; p++) text[pos++] = *p;
    pos += fmt_uint(&text[pos], section_count);

    const char* hint;
    if (section_count <= content_rows) {
        hint = "  (whole section fits on screen) --";
    } else if (top_line == section_first) {
        hint = "  (Up/Down PgUp/PgDn Home/End to scroll) --";
    } else if (top_line >= section_max_top) {
        hint = "  (at end - Up/PgUp/Home to scroll back) --";
    } else {
        hint = "  (Up/Down PgUp/PgDn Home/End to scroll) --";
    }
    for (const char* p = hint; *p && pos < MAX_COLS; p++) text[pos++] = *p;
    text[pos] = '\0';

    if (mode == TERM_MODE_FRAMEBUFFER) {
        for (size_t x = 0; x < content_cols; x++) {
            char ch = (x < pos) ? text[x] : ' ';
            fb_draw_glyph(x, screen_row, (unsigned char)ch, VGA_BLACK, VGA_LIGHT_GREY);
        }
    } else {
        uint8_t color = vga_entry_color(VGA_BLACK, VGA_LIGHT_GREY);
        for (size_t x = 0; x < content_cols; x++) {
            char ch = (x < pos) ? text[x] : ' ';
            VGA_TEXT_MEMORY[screen_row * VGA_TEXT_WIDTH + x] = vga_entry((unsigned char)ch, color);
        }
    }
}

/* Like terminal_render_from(), but confines both scrolling and the
 * visible window to a single [section_first, section_last] line range
 * (inclusive) instead of the whole shared buffer - rows beyond
 * section_last are left blank rather than showing the next section's
 * content, and the status bar counts lines relative to the section. Lets
 * kernel.c keep every section's report text in one shared scrollback
 * buffer (simple to populate) while still presenting each one as its
 * own self-contained page in the GUI. */
void terminal_render_section(size_t top_line, size_t section_first, size_t section_last) {
    size_t section_count = section_last - section_first + 1;
    size_t max_top = (section_count > content_rows) ? section_last - content_rows + 1 : section_first;
    if (top_line < section_first) top_line = section_first;
    if (top_line > max_top) top_line = max_top;

    for (size_t r = 0; r < content_rows; r++) {
        size_t line = top_line + r;
        if (line <= section_last && line < total_lines) {
            draw_text_row(r, scrollback[line], content_cols);
        } else {
            draw_text_row(r, (const struct term_cell*)0, 0);
        }
    }
    draw_status_bar_section(content_rows, top_line, section_first, section_last);
    update_cursor(content_rows, 0);
}
