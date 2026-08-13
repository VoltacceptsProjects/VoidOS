#include "apps.h"
#include "fs.h"
#include "keyboard.h"
#include "mouse.h"
#include "ps2.h"
#include "ui.h"
#include "vga.h"
#include <stdint.h>

#define APP_COUNT 3
#define APP_CARD_H 112
#define APP_GAP 16

struct app_descriptor {
    const char* name;
    const char* description;
};

static const struct app_descriptor app_list[APP_COUNT] = {
    { "Calculator", "Evaluate quick integer expressions" },
    { "Terminal",   "Run commands from the VoidOS shell" },
    { "Scratchpad", "Write a temporary note" },
};

static const char* app_name(int index) {
    return (index >= 0 && index < APP_COUNT) ? app_list[index].name : "";
}

void apps_print_launcher(void) {
    terminal_setcolor(VGA_WHITE, VGA_DARK_GREY);
    terminal_writestring("Applications\n\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    terminal_writestring("VoidOS can launch built-in applications from this page.\n");
    terminal_writestring("Select Applications, then press Enter to open the launcher.\n\n");
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_DARK_GREY);
    terminal_writestring("Included apps\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    terminal_writestring("  Calculator    integer arithmetic\n");
    terminal_writestring("  Terminal      small built-in command shell\n");
    terminal_writestring("  Scratchpad    temporary text editor\n\n");
    terminal_writestring("Esc returns to the launcher or shuts down from the main screen.\n");
}

static void app_area(uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) {
    ui_content_viewport(x, y, w, h);
}

static void fill_app_area(void) {
    uint32_t x, y, w, h;
    app_area(&x, &y, &w, &h);
    gfx_fill_rect(x - 2, y - 2, w + 4, h + 4, gfx_palette_color(VGA_DARK_GREY));
}

static void draw_app_header(const char* title, const char* subtitle) {
    uint32_t x, y, w, h;
    app_area(&x, &y, &w, &h);
    (void)w;
    (void)h;
    fill_app_area();
    gfx_draw_text(x, y, title, gfx_palette_color(VGA_WHITE),
                  gfx_palette_color(VGA_DARK_GREY), 2);
    gfx_draw_text(x, y + 34, subtitle, gfx_palette_color(VGA_LIGHT_GREY),
                  gfx_palette_color(VGA_DARK_GREY), 1);
}

static void draw_app_footer(const char* hint) {
    uint32_t x, y, w, h;
    app_area(&x, &y, &w, &h);
    gfx_hline(x, y + h - 24, w, gfx_palette_color(VGA_LIGHT_BLUE));
    gfx_draw_text(x, y + h - 16, hint, gfx_palette_color(VGA_LIGHT_GREY),
                  gfx_palette_color(VGA_DARK_GREY), 1);
}

static void redraw_cursor(int content_changed) {
    const struct mouse_state* ms = mouse_get_state();
    /* Only invalidate the saved-backing sprite when something underneath
     * it was actually repainted this frame. ui_draw_cursor() erases the
     * cursor by restoring those saved pixels; invalidating on every call
     * (including plain moves) skips that restore permanently, which is
     * what was leaving an arrow-shaped trail at every past position. */
    if (content_changed) ui_cursor_invalidate();
    ui_draw_cursor(ms->x, ms->y);
}

static int card_hit(uint32_t mx, uint32_t my, int index) {
    uint32_t x, y, w, h;
    app_area(&x, &y, &w, &h);
    uint32_t card_w = (w - APP_GAP) / 2;
    uint32_t col = (uint32_t)(index % 2);
    uint32_t row = (uint32_t)(index / 2);
    uint32_t cx = x + col * (card_w + APP_GAP);
    uint32_t cy = y + 68 + row * (APP_CARD_H + APP_GAP);
    return mx >= cx && mx < cx + card_w && my >= cy && my < cy + APP_CARD_H;
}

static void draw_launcher(int selected) {
    uint32_t x, y, w, h;
    app_area(&x, &y, &w, &h);
    fill_app_area();
    gfx_draw_text(x, y, "Applications", gfx_palette_color(VGA_WHITE),
                  gfx_palette_color(VGA_DARK_GREY), 2);
    gfx_draw_text(x, y + 34, "Select an app to run inside VoidOS", gfx_palette_color(VGA_LIGHT_GREY),
                  gfx_palette_color(VGA_DARK_GREY), 1);

    uint32_t card_w = (w - APP_GAP) / 2;
    for (int i = 0; i < APP_COUNT; i++) {
        uint32_t col = (uint32_t)(i % 2);
        uint32_t row = (uint32_t)(i / 2);
        uint32_t cx = x + col * (card_w + APP_GAP);
        uint32_t cy = y + 68 + row * (APP_CARD_H + APP_GAP);
        uint32_t bg = (i == selected) ? gfx_palette_color(VGA_BLUE)
                                      : gfx_palette_color(VGA_BLACK);
        gfx_fill_card(cx, cy, card_w, APP_CARD_H, bg, 8);
        gfx_fill_rect(cx, cy, 5, APP_CARD_H, gfx_palette_color(VGA_LIGHT_BLUE));
        gfx_draw_text(cx + 20, cy + 20, app_name(i), gfx_palette_color(VGA_WHITE), bg, 1);
        gfx_draw_text(cx + 20, cy + 48, app_list[i].description,
                      gfx_palette_color(VGA_LIGHT_GREY), bg, 1);
        gfx_draw_text(cx + 20, cy + 76, (i == selected) ? "Enter to launch" : " ",
                      gfx_palette_color(VGA_LIGHT_CYAN), bg, 1);
    }
    draw_app_footer("Left/Right: select   Enter: launch   Esc: return");
}

static int str_equal(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void copy_text(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    if (cap == 0) return;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void int_to_text(int32_t value, char* out, unsigned int cap) {
    char reverse[16];
    unsigned int n = 0;
    uint32_t magnitude;
    int negative = value < 0;
    if (cap == 0) return;
    if (negative) {
        magnitude = (uint32_t)(-(value + 1)) + 1;
    } else {
        magnitude = (uint32_t)value;
    }
    do {
        reverse[n++] = (char)('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude && n < sizeof(reverse));

    unsigned int pos = 0;
    if (negative && pos + 1 < cap) out[pos++] = '-';
    while (n && pos + 1 < cap) out[pos++] = reverse[--n];
    out[pos] = '\0';
}

static int32_t calculate(const char* expression, int* ok) {
    int32_t total = 0;
    int32_t number = 0;
    char operation = 0;
    int have_number = 0;
    int negative = 0;
    *ok = 0;

    for (const char* p = expression;; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            number = number * 10 + (int32_t)(c - '0');
            have_number = 1;
            continue;
        }
        if (c == '-' && !have_number && !operation) {
            negative = 1;
            continue;
        }
        if ((c == '+' || c == '-' || c == '*' || c == '/') && have_number) {
            if (negative) number = -number;
            if (!operation) {
                total = number;
            } else if (operation == '+') {
                total += number;
            } else if (operation == '-') {
                total -= number;
            } else if (operation == '*') {
                total *= number;
            } else if (number != 0) {
                total /= number;
            } else {
                return 0;
            }
            operation = c;
            number = 0;
            negative = 0;
            have_number = 0;
            continue;
        }
        if (c == '\0' && have_number) {
            if (negative) number = -number;
            if (!operation) total = number;
            else if (operation == '+') total += number;
            else if (operation == '-') total -= number;
            else if (operation == '*') total *= number;
            else if (number != 0) total /= number;
            else return 0;
            *ok = 1;
            return total;
        }
        if (c == '\0') return 0;
        return 0;
    }
}

static void run_calculator(void) {
    char expression[64] = {0};
    char result[20] = "Ready";
    unsigned int length = 0;
    int dirty = 1;

    while (1) {
        ps2_poll();
        enum key key = keyboard_poll_key();
        char c = keyboard_poll_char();
        const struct mouse_state* ms = mouse_get_state();
        int moved = ms->moved;
        if (key == KEY_ESC) {
            mouse_clear_events();
            return;
        }
        if (key == KEY_BACKSPACE && length) {
            expression[--length] = '\0';
            dirty = 1;
        } else if (key == KEY_ENTER) {
            int ok = 0;
            int32_t value = calculate(expression, &ok);
            if (ok) int_to_text(value, result, sizeof(result));
            else copy_text(result, "Invalid expression", sizeof(result));
            dirty = 1;
        } else if (c == 'c' || c == 'C') {
            length = 0;
            expression[0] = '\0';
            copy_text(result, "Ready", sizeof(result));
            dirty = 1;
        } else if ((c >= '0' && c <= '9') || c == '+' || c == '-' ||
                   c == '*' || c == '/') {
            if (length + 1 < sizeof(expression)) {
                expression[length++] = c;
                expression[length] = '\0';
                dirty = 1;
            }
        }
        int redrew_content = dirty;
        if (dirty) {
            draw_app_header("Calculator", "Type an expression using digits and + - * /");
            uint32_t x, y, w, h;
            app_area(&x, &y, &w, &h);
            (void)w;
            gfx_draw_text(x, y + 92, "Expression", gfx_palette_color(VGA_LIGHT_CYAN),
                          gfx_palette_color(VGA_DARK_GREY), 1);
            gfx_draw_text(x, y + 122, expression[0] ? expression : "0",
                          gfx_palette_color(VGA_WHITE), gfx_palette_color(VGA_BLACK), 2);
            gfx_draw_text(x, y + 178, "Result", gfx_palette_color(VGA_LIGHT_CYAN),
                          gfx_palette_color(VGA_DARK_GREY), 1);
            gfx_draw_text(x, y + 208, result, gfx_palette_color(VGA_WHITE),
                          gfx_palette_color(VGA_BLACK), 2);
            draw_app_footer("Enter: calculate   C: clear   Backspace: delete   Esc: close");
            dirty = 0;
        }
        if (redrew_content || moved) redraw_cursor(redrew_content);
        mouse_clear_events();
    }
}

static void run_terminal(void) {
    char command[72] = {0};
    char output[96] = "Type help for available commands.";
    unsigned int length = 0;
    int format_armed = 0;
    int dirty = 1;

    while (1) {
        ps2_poll();
        enum key key = keyboard_poll_key();
        char c = keyboard_poll_char();
        const struct mouse_state* ms = mouse_get_state();
        int moved = ms->moved;
        if (key == KEY_ESC) {
            mouse_clear_events();
            return;
        }
        if (key == KEY_BACKSPACE && length) {
            command[--length] = '\0';
            dirty = 1;
        } else if (key == KEY_ENTER) {
            if (str_equal(command, "help")) copy_text(output, "help  about  apps  clear  format voidfs", sizeof(output));
            else if (str_equal(command, "about")) copy_text(output, "VoidOS built-in shell; Esc closes the app.", sizeof(output));
            else if (str_equal(command, "apps")) copy_text(output, "calculator  terminal  scratchpad", sizeof(output));
            else if (str_equal(command, "clear")) copy_text(output, "", sizeof(output));
            else if (str_equal(command, "format voidfs")) {
                format_armed = 1;
                copy_text(output, "WARNING: this erases Drive 0. Type format confirm to continue.", sizeof(output));
            } else if (str_equal(command, "format confirm")) {
                if (format_armed && voidfs_format_drive(0) == 0) {
                    copy_text(output, "Drive 0 formatted as VoidFS. Restart to refresh the Files page.", sizeof(output));
                } else if (!format_armed) {
                    copy_text(output, "Run format voidfs first.", sizeof(output));
                } else {
                    copy_text(output, "Format failed: no usable ATA drive or disk error.", sizeof(output));
                }
                format_armed = 0;
            }
            else if (length) copy_text(output, "command not found", sizeof(output));
            else copy_text(output, "", sizeof(output));
            length = 0;
            command[0] = '\0';
            dirty = 1;
        } else if (c >= 32 && c <= 126 && length + 1 < sizeof(command)) {
            command[length++] = c;
            command[length] = '\0';
            dirty = 1;
        }
        int redrew_content = dirty;
        if (dirty) {
            draw_app_header("Terminal", "A small command shell built into the VoidOS application runtime");
            uint32_t x, y, w, h;
            app_area(&x, &y, &w, &h);
            (void)w;
            (void)h;
            gfx_draw_text(x, y + 96, output, gfx_palette_color(VGA_LIGHT_GREEN),
                          gfx_palette_color(VGA_DARK_GREY), 1);
            gfx_draw_text(x, y + 146, "voidos> ", gfx_palette_color(VGA_LIGHT_CYAN),
                          gfx_palette_color(VGA_DARK_GREY), 1);
            gfx_draw_text(x + 72, y + 146, command, gfx_palette_color(VGA_WHITE),
                          gfx_palette_color(VGA_BLACK), 1);
            draw_app_footer("Enter: run command   Backspace: delete   Esc: close");
            dirty = 0;
        }
        if (redrew_content || moved) redraw_cursor(redrew_content);
        mouse_clear_events();
    }
}

static void run_scratchpad(void) {
    char note[120] = {0};
    unsigned int length = 0;
    int dirty = 1;

    while (1) {
        ps2_poll();
        enum key key = keyboard_poll_key();
        char c = keyboard_poll_char();
        const struct mouse_state* ms = mouse_get_state();
        int moved = ms->moved;
        if (key == KEY_ESC) {
            mouse_clear_events();
            return;
        }
        if (key == KEY_BACKSPACE && length) {
            note[--length] = '\0';
            dirty = 1;
        } else if (c >= 32 && c <= 126 && length + 1 < sizeof(note)) {
            note[length++] = c;
            note[length] = '\0';
            dirty = 1;
        }
        int redrew_content = dirty;
        if (dirty) {
            draw_app_header("Scratchpad", "A temporary note that lives until you close the app");
            uint32_t x, y, w, h;
            app_area(&x, &y, &w, &h);
            (void)w;
            (void)h;
            gfx_draw_text(x, y + 100, "Your note", gfx_palette_color(VGA_LIGHT_CYAN),
                          gfx_palette_color(VGA_DARK_GREY), 1);
            gfx_fill_card(x, y + 132, 680, 112, gfx_palette_color(VGA_BLACK), 8);
            gfx_draw_text(x + 16, y + 162, note[0] ? note : "Start typing...",
                          gfx_palette_color(VGA_WHITE), gfx_palette_color(VGA_BLACK), 1);
            draw_app_footer("Type to write   Backspace: delete   Esc: close");
            dirty = 0;
        }
        if (redrew_content || moved) redraw_cursor(redrew_content);
        mouse_clear_events();
    }
}

static void run_app(int index) {
    if (index == 0) run_calculator();
    else if (index == 1) run_terminal();
    else if (index == 2) run_scratchpad();
}

void apps_run_launcher(void) {
    int selected = 0;
    int dirty = 1;

    /* Do not reprocess the click that opened the Applications section. */
    mouse_clear_events();
    while (1) {
        ps2_poll();
        enum key key = keyboard_poll_key();
        const struct mouse_state* ms = mouse_get_state();
        int moved = ms->moved;
        int clicked = ms->left_clicked;
        if (key == KEY_ESC) {
            mouse_clear_events();
            return;
        }
        if (key == KEY_LEFT) {
            selected = (selected + APP_COUNT - 1) % APP_COUNT;
            dirty = 1;
        } else if (key == KEY_RIGHT) {
            selected = (selected + 1) % APP_COUNT;
            dirty = 1;
        } else if (key == KEY_ENTER) {
            mouse_clear_events();
            run_app(selected);
            dirty = 1;
        }

        if (clicked) {
            for (int i = 0; i < APP_COUNT; i++) {
                if (card_hit((uint32_t)ms->x, (uint32_t)ms->y, i)) {
                    selected = i;
                    mouse_clear_events();
                    run_app(i);
                    dirty = 1;
                    break;
                }
            }
        }
        mouse_clear_events();

        int redrew_content = dirty;
        if (dirty) {
            draw_launcher(selected);
            dirty = 0;
        }
        /* Movement only repaints the small cursor backing rectangle. */
        if (moved || redrew_content) redraw_cursor(redrew_content);
    }
}