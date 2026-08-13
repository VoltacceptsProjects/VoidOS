#include "appstore.h"
#include "net.h"
#include "fs.h"
#include "ui.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"
#include "ps2.h"
#include "serial.h"
#include <stdint.h>

#define APPSTORE_MAX_ENTRIES 12
#define APPSTORE_NAME_CAP    48
#define APPSTORE_URL_CAP     80
#define APPSTORE_STATUS_CAP  64
#define APPSTORE_JSON_CAP    8192
#define APPSTORE_PKG_CAP     4096

#define APP_CARD_H 96
#define APP_GAP    16

struct appstore_entry {
    char name[APPSTORE_NAME_CAP];
    char url[APPSTORE_URL_CAP];
    uint32_t size;
    int installed;
};

static struct appstore_entry g_entries[APPSTORE_MAX_ENTRIES];
static int g_entry_count = 0;
static char g_status[APPSTORE_STATUS_CAP] = "Not loaded yet";
static uint8_t g_json_buf[APPSTORE_JSON_CAP];
static uint8_t g_pkg_buf[APPSTORE_PKG_CAP];

/* --- small self-contained string helpers --------------------------------
 * Duplicated on purpose rather than shared with apps.c/iwlwifi.c - this
 * project keeps each file's tiny helpers local rather than growing a
 * shared string.c, same choice those two files already made. */

static int str_equal(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static void copy_text(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    if (cap == 0) return;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int has_suffix(const char* s, const char* suffix) {
    int slen = 0, suflen = 0;
    while (s[slen]) slen++;
    while (suffix[suflen]) suflen++;
    if (suflen > slen) return 0;
    for (int i = 0; i < suflen; i++)
        if (s[slen - suflen + i] != suffix[i]) return 0;
    return 1;
}

/* --- tiny JSON scanner, scoped to this endpoint's exact shape -----------
 * Not a general JSON library - it walks the "files" array, reads each
 * object's "name"/"url"/"size" values, and skips anything else (strings,
 * nested objects/arrays, numbers) without trying to represent it. That's
 * enough to survive the endpoint adding fields later without this
 * breaking, same "ignore what I don't recognise" choice apps.c's
 * manifest reader makes for unrecognised keys. Every loop below only
 * ever advances *p or stops at '\0', so a malformed/truncated body can't
 * spin forever - worst case it stops parsing early. */

static void skip_ws(const char** p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

static void skip_json_string(const char** p) {
    if (**p != '"') return;
    (*p)++;
    while (**p && **p != '"') {
        if (**p == '\\' && (*p)[1]) (*p)++;
        (*p)++;
    }
    if (**p == '"') (*p)++;
}

/* Reads a JSON string literal into out (truncated, NUL-terminated).
 * Returns 0 (and doesn't advance) if *p isn't at an opening quote. */
static int read_json_string(const char** p, char* out, unsigned int cap) {
    if (**p != '"') return 0;
    (*p)++;
    unsigned int n = 0;
    while (**p && **p != '"') {
        char c = **p;
        if (c == '\\' && (*p)[1]) { (*p)++; c = **p; }
        if (n + 1 < cap) out[n++] = c;
        (*p)++;
    }
    if (**p == '"') (*p)++;
    if (cap) out[n] = '\0';
    return 1;
}

static uint32_t read_json_uint(const char** p) {
    uint32_t v = 0;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (uint32_t)(**p - '0'); (*p)++; }
    return v;
}

/* Skips one JSON value of any kind (string/object/array/number-or-
 * literal) starting at *p, leaving *p just past it. Used for keys this
 * parser doesn't care about ("type", "modified", ...). */
static void skip_json_value(const char** p) {
    skip_ws(p);
    if (**p == '"') {
        skip_json_string(p);
    } else if (**p == '{' || **p == '[') {
        char open = **p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        do {
            if (**p == '"') { skip_json_string(p); continue; }
            if (**p == open) depth++;
            else if (**p == close) depth--;
            if (**p) (*p)++;
        } while (depth > 0 && **p);
    } else {
        while (**p && **p != ',' && **p != '}' && **p != ']') (*p)++;
    }
}

/* *p must point at the '{' of one "files" array entry. Parses it,
 * advances *p past the matching '}', and - if it's a .vapp - appends it
 * to g_entries. Anything that isn't a .vapp (e.g. "index.php", the
 * lister script itself) is parsed and dropped, same as upstream's own
 * behaviour of returning every file in the directory regardless of
 * type. */
static void parse_entry_object(const char** p) {
    struct appstore_entry e;
    e.name[0] = '\0';
    e.url[0] = '\0';
    e.size = 0;
    e.installed = 0;

    (*p)++; /* past '{' */
    skip_ws(p);
    while (**p && **p != '}') {
        char key[16];
        if (!read_json_string(p, key, sizeof(key))) { skip_json_value(p); }
        else {
            skip_ws(p);
            if (**p == ':') (*p)++;
            skip_ws(p);
            if (str_equal(key, "name")) read_json_string(p, e.name, sizeof(e.name));
            else if (str_equal(key, "url")) read_json_string(p, e.url, sizeof(e.url));
            else if (str_equal(key, "size")) e.size = read_json_uint(p);
            else skip_json_value(p);
        }
        skip_ws(p);
        if (**p == ',') (*p)++;
        skip_ws(p);
    }
    if (**p == '}') (*p)++;

    if (!has_suffix(e.name, ".vapp")) return;
    if (g_entry_count >= APPSTORE_MAX_ENTRIES) {
        serial_writestring("[appstore] more .vapp entries than "
                            "APPSTORE_MAX_ENTRIES - dropping the rest\n");
        return;
    }
    if (e.url[0] == '\0') copy_text(e.url, e.name, sizeof(e.url));

    g_entries[g_entry_count] = e;
    g_entry_count++;
}

static void parse_directory_json(const char* json) {
    g_entry_count = 0;

    /* Find the "files" key without a general key/value walk over the
     * whole object - "directory" (a plain string) is the only other
     * top-level key this endpoint sends, and skipping straight to
     * "files" is robust to that appearing in either order. */
    const char* p = json;
    const char* needle = "\"files\"";
    const char* found = 0;
    for (; *p; p++) {
        const char* a = p;
        const char* b = needle;
        while (*b && *a == *b) { a++; b++; }
        if (*b == '\0') { found = p; break; }
    }
    if (!found) {
        copy_text(g_status, "malformed listing - no \"files\" array", sizeof(g_status));
        serial_writestring("[appstore] parse failed: no \"files\" key found\n");
        return;
    }
    p = found;
    while (*p && *p != '[') p++;
    if (!*p) return;
    p++; /* past '[' */

    skip_ws(&p);
    while (*p && *p != ']') {
        skip_ws(&p);
        if (*p == '{') parse_entry_object(&p);
        else break; /* not shaped the way we expect - stop rather than spin */
        skip_ws(&p);
        if (*p == ',') p++;
        skip_ws(&p);
    }

    serial_writestring("[appstore] parsed ");
    serial_write_uint((uint32_t)g_entry_count);
    serial_writestring(" .vapp entries from the directory listing\n");
}

static int is_installed(const char* name) {
    unsigned int count = voidfs_file_count();
    for (unsigned int i = 0; i < count; i++) {
        const struct voidfs_file* f = voidfs_file_at(i);
        if (f && str_equal(f->mime, VOIDFS_VAPP_MIME) && str_equal(f->name, name))
            return 1;
    }
    return 0;
}

void appstore_refresh(struct multiboot_info* mbi) {
    uint32_t got = 0;
    serial_writestring("[appstore] refreshing directory listing from "
                        APPSTORE_HOST "\n");

    if (!net_http_get(mbi, APPSTORE_HOST, "/", g_json_buf, sizeof(g_json_buf) - 1, &got)) {
        g_entry_count = 0;
        copy_text(g_status, "Can't reach the App Store - no network yet",
                  sizeof(g_status));
        return;
    }
    g_json_buf[got] = '\0';

    parse_directory_json((const char*)g_json_buf);

    for (int i = 0; i < g_entry_count; i++)
        g_entries[i].installed = is_installed(g_entries[i].name);

    if (g_entry_count == 0 && g_status[0] == '\0')
        copy_text(g_status, "No .vapp packages listed", sizeof(g_status));
    else if (g_entry_count > 0)
        g_status[0] = '\0';
}

static int install_entry(struct multiboot_info* mbi, int index) {
    if (index < 0 || index >= g_entry_count) return 0;
    struct appstore_entry* e = &g_entries[index];

    char path[APPSTORE_URL_CAP + 1];
    path[0] = '/';
    copy_text(path + 1, e->url, sizeof(path) - 1);

    uint32_t got = 0;
    serial_writestring("[appstore] installing \"");
    serial_writestring(e->name);
    serial_writestring("\"\n");

    if (!net_http_get(mbi, APPSTORE_HOST, path, g_pkg_buf, sizeof(g_pkg_buf), &got)) {
        copy_text(g_status, "Download failed - no network yet", sizeof(g_status));
        return 0;
    }

    int rc = voidfs_install_vapp(g_pkg_buf, got, e->name);
    if (rc != 0) {
        copy_text(g_status, "Install failed - not a valid .vapp package",
                  sizeof(g_status));
        serial_writestring("[appstore]   voidfs_install_vapp() rejected it\n");
        return 0;
    }

    e->installed = 1;
    copy_text(g_status, "Installed", sizeof(g_status));
    return 1;
}

/* --- card grid, same layout scheme as apps.c's draw_launcher() ---------- */

static void app_area(uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) {
    ui_content_viewport(x, y, w, h);
}

static void fill_app_area(void) {
    uint32_t x, y, w, h;
    app_area(&x, &y, &w, &h);
    gfx_fill_rect(x - 2, y - 2, w + 4, h + 4, gfx_palette_color(VGA_DARK_GREY));
}

static void draw_footer(const char* hint) {
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

static void size_to_text(uint32_t bytes, char* out, unsigned int cap) {
    uint32_t kb = bytes / 1024;
    if (kb == 0 && bytes > 0) kb = 1;
    char digits[12];
    unsigned int n = 0;
    uint32_t v = kb;
    do { digits[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < sizeof(digits));
    unsigned int pos = 0;
    while (n && pos + 1 < cap) out[pos++] = digits[--n];
    const char* suffix = " KB";
    for (unsigned int i = 0; suffix[i] && pos + 1 < cap; i++) out[pos++] = suffix[i];
    out[pos] = '\0';
}

static void draw_appstore(int selected) {
    uint32_t x, y, w, h;
    app_area(&x, &y, &w, &h);
    fill_app_area();
    gfx_draw_text(x, y, "App Store", gfx_palette_color(VGA_WHITE),
                  gfx_palette_color(VGA_DARK_GREY), 2);
    gfx_draw_text(x, y + 34, "Every .vapp at " APPSTORE_HOST,
                  gfx_palette_color(VGA_LIGHT_GREY), gfx_palette_color(VGA_DARK_GREY), 1);

    if (g_entry_count == 0) {
        gfx_draw_text(x, y + 80, g_status[0] ? g_status : "No apps found",
                      gfx_palette_color(VGA_LIGHT_RED), gfx_palette_color(VGA_DARK_GREY), 1);
        draw_footer("Esc: return");
        return;
    }

    uint32_t card_w = (w - APP_GAP) / 2;
    for (int i = 0; i < g_entry_count; i++) {
        uint32_t col = (uint32_t)(i % 2);
        uint32_t row = (uint32_t)(i / 2);
        uint32_t cx = x + col * (card_w + APP_GAP);
        uint32_t cy = y + 68 + row * (APP_CARD_H + APP_GAP);
        uint32_t bg = (i == selected) ? gfx_palette_color(VGA_BLUE)
                                      : gfx_palette_color(VGA_BLACK);
        gfx_fill_card(cx, cy, card_w, APP_CARD_H, bg, 8);
        gfx_fill_rect(cx, cy, 5, APP_CARD_H,
                      gfx_palette_color(g_entries[i].installed ? VGA_LIGHT_GREEN : VGA_LIGHT_BLUE));

        gfx_draw_text(cx + 20, cy + 16, g_entries[i].name,
                      gfx_palette_color(VGA_WHITE), bg, 1);

        char size_text[16];
        size_to_text(g_entries[i].size, size_text, sizeof(size_text));
        gfx_draw_text(cx + 20, cy + 40, size_text,
                      gfx_palette_color(VGA_LIGHT_GREY), bg, 1);

        const char* state;
        if (g_entries[i].installed) state = "Installed";
        else if (i == selected && g_status[0]) state = g_status;
        else if (i == selected) state = "Enter to download & install";
        else state = " ";
        gfx_draw_text(cx + 20, cy + 64, state,
                      gfx_palette_color(g_entries[i].installed ? VGA_LIGHT_GREEN : VGA_LIGHT_CYAN),
                      bg, 1);
    }
    draw_footer("Left/Right: select   Enter: download & install   Esc: return");
}

void appstore_run(struct multiboot_info* mbi) {
    appstore_refresh(mbi);
    int selected = 0;
    int dirty = 1;

    mouse_clear_events(); /* don't reprocess the click that opened this section */
    while (1) {
        ps2_poll();
        enum key key = keyboard_poll_key();
        const struct mouse_state* ms = mouse_get_state();
        int moved = ms->moved;
        int clicked = ms->left_clicked;

        if (key == KEY_ESC) { mouse_clear_events(); return; }

        if (g_entry_count > 0) {
            if (key == KEY_LEFT) {
                selected = (selected + g_entry_count - 1) % g_entry_count;
                dirty = 1;
            } else if (key == KEY_RIGHT) {
                selected = (selected + 1) % g_entry_count;
                dirty = 1;
            } else if (key == KEY_ENTER) {
                mouse_clear_events();
                install_entry(mbi, selected);
                dirty = 1;
            }
        }

        if (clicked) {
            for (int i = 0; i < g_entry_count; i++) {
                if (card_hit((uint32_t)ms->x, (uint32_t)ms->y, i)) {
                    selected = i;
                    mouse_clear_events();
                    install_entry(mbi, i);
                    dirty = 1;
                    break;
                }
            }
        }
        mouse_clear_events();

        int redrew = dirty;
        if (dirty) { draw_appstore(selected); dirty = 0; }
        if (moved || redrew) redraw_cursor();
    }
}
