#include "apps.h"
#include "fs.h"
#include "keyboard.h"
#include "mouse.h"
#include "ps2.h"
#include "ui.h"
#include "vga.h"
#include <stdint.h>

/* --- Application registry -------------------------------------------
 *
 * Two "system" utilities (Calculator, Terminal) are still linked into
 * the kernel, because they need direct access to VoidOS internals that
 * a distributable package shouldn't have (Terminal's "format voidfs"
 * in particular).
 *
 * Every other card in the launcher is discovered from VoidFS: any
 * installed file whose mime is VOIDFS_VAPP_MIME is a .vapp package
 * (see fs.h) and gets listed here automatically. The package's manifest
 * (a small "key=value" text block carried inside it, see
 * parse_manifest() below) supplies the display name, description and
 * "kind" of app it is - "kind" picks which built-in runtime in this
 * file actually executes it, since VoidOS has no general-purpose
 * executable loader yet.
 *
 * VoidOS itself never talks to the network - .vapp packages reach
 * VoidFS as GRUB Multiboot modules, installed at boot by
 * voidfs_install_multiboot_modules() (see fs.c / kernel.c). The
 * canonical directory of every .vapp that can be installed is kept
 * online, outside the kernel, at:
 *
 *   https://github.com/VoltacceptsProjects/VoidOS-Applications
 *
 * vapps/ at the repository root is a synced mirror of packages from
 * there and is what actually gets bundled into the ISO - see
 * vapps/README.md and tools/sync-vapps.sh. */

#define MAX_APP_SLOTS 8
#define APP_CARD_H 112
#define APP_GAP 16
#define APP_NAME_CAP 40
#define APP_DESC_CAP 64
#define APP_KIND_CAP 24
#define VAPP_MANIFEST_READ_CAP 256
#define VAPP_TEXT_CAP 480
#define VAPP_PACKAGE_READ_CAP 4096

struct app_slot {
    int is_builtin;          /* 1 for Calculator/Terminal, 0 for a .vapp */
    unsigned int file_index; /* valid only when !is_builtin */
    char name[APP_NAME_CAP];
    char description[APP_DESC_CAP];
    char kind[APP_KIND_CAP]; /* manifest "kind=" - dispatches run_vapp() */
};

static struct app_slot app_slots[MAX_APP_SLOTS];
static int app_total = 0;

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

/* Very small "key=value" reader for the text manifest bundled inside a
 * .vapp. One assignment per line; blank lines and unrecognised keys are
 * ignored, so the format can grow without breaking older builds of
 * VoidOS. Recognised keys: name, description, kind. */
static void parse_manifest(const char* text, unsigned int length, struct app_slot* slot) {
    unsigned int i = 0;
    while (i < length) {
        unsigned int line_start = i;
        while (i < length && text[i] != '\n') i++;
        unsigned int line_end = i;
        if (i < length) i++; /* skip the newline itself */
        if (line_end > line_start && text[line_end - 1] == '\r') line_end--;

        unsigned int eq = line_start;
        while (eq < line_end && text[eq] != '=') eq++;
        if (eq >= line_end) continue; /* no '=' on this line */

        unsigned int key_start = line_start, key_end = eq;
        while (key_start < key_end && text[key_start] == ' ') key_start++;
        while (key_end > key_start && text[key_end - 1] == ' ') key_end--;

        unsigned int val_start = eq + 1, val_end = line_end;
        while (val_start < val_end && text[val_start] == ' ') val_start++;
        while (val_end > val_start && text[val_end - 1] == ' ') val_end--;

        char key[16] = {0};
        unsigned int klen = key_end - key_start;
        if (klen >= sizeof(key)) klen = sizeof(key) - 1;
        for (unsigned int k = 0; k < klen; k++) key[k] = text[key_start + k];
        key[klen] = '\0';

        char* dst = 0;
        unsigned int cap = 0;
        if (str_equal(key, "name")) { dst = slot->name; cap = APP_NAME_CAP; }
        else if (str_equal(key, "description")) { dst = slot->description; cap = APP_DESC_CAP; }
        else if (str_equal(key, "kind")) { dst = slot->kind; cap = APP_KIND_CAP; }
        if (!dst) continue;

        unsigned int vlen = val_end - val_start;
        if (vlen >= cap) vlen = cap - 1;
        for (unsigned int v = 0; v < vlen; v++) dst[v] = text[val_start + v];
        dst[vlen] = '\0';
    }
}

/* Rebuilds app_slots[] from whatever is currently installed in VoidFS.
 * Cheap enough (a handful of files, each read once) to call whenever the
 * launcher is (re)entered rather than caching it. */
static void refresh_app_slots(void) {
    app_total = 0;

    copy_text(app_slots[app_total].name, "Calculator", APP_NAME_CAP);
    copy_text(app_slots[app_total].description, "Evaluate quick integer expressions", APP_DESC_CAP);
    app_slots[app_total].is_builtin = 1;
    app_total++;

    copy_text(app_slots[app_total].name, "Terminal", APP_NAME_CAP);
    copy_text(app_slots[app_total].description, "Run commands from the VoidOS shell", APP_DESC_CAP);
    app_slots[app_total].is_builtin = 1;
    app_total++;

    unsigned int count = voidfs_file_count();
    for (unsigned int i = 0; i < count && app_total < MAX_APP_SLOTS; i++) {
        const struct voidfs_file* f = voidfs_file_at(i);
        if (!f || !str_equal(f->mime, VOIDFS_VAPP_MIME)) continue;

        struct vapp_header header;
        if (voidfs_read_file(i, 0, (uint8_t*)&header, sizeof(header)) != 0) continue;
        if (header.magic[0] != 'V' || header.magic[1] != 'A' ||
            header.magic[2] != 'P' || header.magic[3] != 'P') continue;

        struct app_slot* slot = &app_slots[app_total];
        slot->is_builtin = 0;
        slot->file_index = i;
        copy_text(slot->name, header.name[0] ? header.name : f->name, APP_NAME_CAP);
        copy_text(slot->description, "Installed application", APP_DESC_CAP);
        slot->kind[0] = '\0';

        if (header.manifest_size) {
            char manifest[VAPP_MANIFEST_READ_CAP];
            uint32_t mlen = header.manifest_size;
            if (mlen >= sizeof(manifest)) mlen = sizeof(manifest) - 1;
            if (voidfs_read_file(i, header.manifest_offset, (uint8_t*)manifest, mlen) == 0) {
                manifest[mlen] = '\0';
                parse_manifest(manifest, mlen, slot);
            }
        }
        app_total++;
    }
}

static const char* app_name(int index) {
    return (index >= 0 && index < app_total) ? app_slots[index].name : "";
}

void apps_print_launcher(void) {
    refresh_app_slots();
    terminal_setcolor(VGA_WHITE, VGA_DARK_GREY);
    terminal_writestring("Applications\n\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    terminal_writestring("VoidOS can launch built-in utilities and any .vapp package\n");
    terminal_writestring("installed into VoidFS. Select Applications, then press Enter\n");
    terminal_writestring("to open the launcher.\n\n");
    terminal_setcolor(VGA_LIGHT_CYAN, VGA_DARK_GREY);
    terminal_writestring("Available now\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
    for (int i = 0; i < app_total; i++) {
        terminal_writestring("  ");
        terminal_writestring(app_slots[i].name);
        terminal_writestring(app_slots[i].is_builtin ? "    built-in\n" : "    installed .vapp\n");
    }
    terminal_writestring("\n");
    terminal_setcolor(VGA_DARK_GREY, VGA_DARK_GREY);
    terminal_writestring("More apps: https://github.com/VoltacceptsProjects/VoidOS-Applications\n");
    terminal_setcolor(VGA_LIGHT_GREY, VGA_DARK_GREY);
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

static void redraw_cursor(void) {
    const struct mouse_state* ms = mouse_get_state();
    ui_cursor_invalidate();
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
    for (int i = 0; i < app_total; i++) {
        uint32_t col = (uint32_t)(i % 2);
        uint32_t row = (uint32_t)(i / 2);
        uint32_t cx = x + col * (card_w + APP_GAP);
        uint32_t cy = y + 68 + row * (APP_CARD_H + APP_GAP);
        uint32_t bg = (i == selected) ? gfx_palette_color(VGA_BLUE)
                                      : gfx_palette_color(VGA_BLACK);
        gfx_fill_card(cx, cy, card_w, APP_CARD_H, bg, 8);
        gfx_fill_rect(cx, cy, 5, APP_CARD_H, gfx_palette_color(VGA_LIGHT_BLUE));
        gfx_draw_text(cx + 20, cy + 20, app_name(i), gfx_palette_color(VGA_WHITE), bg, 1);
        gfx_draw_text(cx + 20, cy + 48, app_slots[i].description,
                      gfx_palette_color(VGA_LIGHT_GREY), bg, 1);
        gfx_draw_text(cx + 20, cy + 76, (i == selected) ? "Enter to launch" : " ",
                      gfx_palette_color(VGA_LIGHT_CYAN), bg, 1);
    }
    draw_app_footer("Left/Right: select   Enter: launch   Esc: return");
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
        if (redrew_content || moved) redraw_cursor();
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
            else if (str_equal(command, "apps")) {
                char listing[96] = {0};
                unsigned int pos = 0;
                for (int i = 0; i < app_total && pos + 1 < sizeof(listing); i++) {
                    const char* n = app_slots[i].name;
                    unsigned int j = 0;
                    while (n[j] && pos + 1 < sizeof(listing)) listing[pos++] = n[j++];
                    if (i + 1 < app_total && pos + 1 < sizeof(listing)) listing[pos++] = ' ';
                }
                listing[pos] = '\0';
                copy_text(output, listing, sizeof(output));
            }
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
        if (redrew_content || moved) redraw_cursor();
        mouse_clear_events();
    }
}

/* --- Generic ".vapp" runtimes -----------------------------------------
 *
 * VoidOS has no executable loader, so a .vapp's payload isn't machine
 * code: it's plain data interpreted by whichever of these small,
 * built-in runtimes matches the manifest's "kind=". Today that's just
 * "text_editor" (what the example Notepad package uses), but the
 * dispatch in run_vapp() is where a future kind would be added. */

static void run_vapp_text_editor(const struct app_slot* slot) {
    static uint8_t package[VAPP_PACKAGE_READ_CAP];
    const struct voidfs_file* f = voidfs_file_at(slot->file_index);
    if (!f) return;

    struct vapp_header header;
    if (voidfs_read_file(slot->file_index, 0, (uint8_t*)&header, sizeof(header)) != 0) return;
    if (header.package_size == 0 || header.package_size > sizeof(package)) {
        draw_app_header(slot->name, "This package is too large for the built-in text editor runtime.");
        draw_app_footer("Esc: close");
        redraw_cursor();
        while (1) {
            ps2_poll();
            if (keyboard_poll_key() == KEY_ESC) { mouse_clear_events(); return; }
            mouse_clear_events();
        }
    }
    if (voidfs_read_file(slot->file_index, 0, package, header.package_size) != 0) return;

    char text[VAPP_TEXT_CAP] = {0};
    uint32_t cap = header.payload_size;
    if (cap >= sizeof(text)) cap = sizeof(text) - 1;
    unsigned int length = 0;
    while (length < cap && package[header.payload_offset + length]) {
        text[length] = (char)package[header.payload_offset + length];
        length++;
    }
    text[length] = '\0';

    int dirty = 1;
    while (1) {
        ps2_poll();
        enum key key = keyboard_poll_key();
        char c = keyboard_poll_char();
        const struct mouse_state* ms = mouse_get_state();
        int moved = ms->moved;
        if (key == KEY_ESC) {
            for (uint32_t i = 0; i < header.payload_size; i++) {
                package[header.payload_offset + i] = (i < length) ? (uint8_t)text[i] : 0;
            }
            voidfs_install_vapp(package, header.package_size, f->name);
            mouse_clear_events();
            return;
        }
        if (key == KEY_BACKSPACE && length) {
            text[--length] = '\0';
            dirty = 1;
        } else if (c >= 32 && c <= 126 && length + 1 < cap) {
            text[length++] = c;
            text[length] = '\0';
            dirty = 1;
        }
        int redrew_content = dirty;
        if (dirty) {
            draw_app_header(slot->name, slot->description);
            uint32_t x, y, w, h;
            app_area(&x, &y, &w, &h);
            (void)h;
            gfx_fill_card(x, y + 96, w - 4, h - 148, gfx_palette_color(VGA_BLACK), 8);
            gfx_draw_text(x + 16, y + 116, text[0] ? text : "Start typing...",
                          gfx_palette_color(VGA_WHITE), gfx_palette_color(VGA_BLACK), 1);
            draw_app_footer("Type to write   Backspace: delete   Esc: save and close");
            dirty = 0;
        }
        if (redrew_content || moved) redraw_cursor();
        mouse_clear_events();
    }
}

static void run_vapp_unsupported(const struct app_slot* slot) {
    draw_app_header(slot->name, "VoidOS doesn't have a runtime for this package's \"kind\" yet.");
    draw_app_footer("Esc: close");
    redraw_cursor();
    while (1) {
        ps2_poll();
        if (keyboard_poll_key() == KEY_ESC) { mouse_clear_events(); return; }
        mouse_clear_events();
    }
}

static void run_vapp(const struct app_slot* slot) {
    if (str_equal(slot->kind, "text_editor")) run_vapp_text_editor(slot);
    else run_vapp_unsupported(slot);
}

static void run_app(int index) {
    if (index < 0 || index >= app_total) return;
    const struct app_slot* slot = &app_slots[index];
    if (slot->is_builtin) {
        if (index == 0) run_calculator();
        else if (index == 1) run_terminal();
        return;
    }
    run_vapp(slot);
}

void apps_run_launcher(void) {
    refresh_app_slots();
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
            selected = (selected + app_total - 1) % app_total;
            dirty = 1;
        } else if (key == KEY_RIGHT) {
            selected = (selected + 1) % app_total;
            dirty = 1;
        } else if (key == KEY_ENTER) {
            mouse_clear_events();
            run_app(selected);
            refresh_app_slots();
            if (selected >= app_total) selected = app_total - 1;
            dirty = 1;
        }

        if (clicked) {
            for (int i = 0; i < app_total; i++) {
                if (card_hit((uint32_t)ms->x, (uint32_t)ms->y, i)) {
                    selected = i;
                    mouse_clear_events();
                    run_app(i);
                    refresh_app_slots();
                    if (selected >= app_total) selected = app_total - 1;
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
        if (moved || redrew_content) redraw_cursor();
    }
}
