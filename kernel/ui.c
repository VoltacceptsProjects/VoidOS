#include "ui.h"
#include "vga.h"

/* ---- layout constants ---------------------------------------------- */

#define TOPBAR_H   64
#define SIDEBAR_W  208
#define PAD        16
#define ITEM_H     52
#define CARD_RADIUS 10

static uint32_t screen_w, screen_h;

static void layout_metrics(void) {
    screen_w = gfx_screen_width();
    screen_h = gfx_screen_height();
}

void ui_content_viewport(uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h) {
    layout_metrics();
    uint32_t area_x = SIDEBAR_W + PAD;
    uint32_t area_y = TOPBAR_H + PAD;
    uint32_t area_w = (screen_w > SIDEBAR_W + 2 * PAD) ? screen_w - SIDEBAR_W - 2 * PAD : 0;
    uint32_t area_h = (screen_h > TOPBAR_H + 2 * PAD) ? screen_h - TOPBAR_H - 2 * PAD : 0;
    /* inset a little further for text padding inside the card itself */
    *x = area_x + 14;
    *y = area_y + 12;
    *w = (area_w > 28) ? area_w - 28 : 0;
    *h = (area_h > 24) ? area_h - 24 : 0;
}

/* ---- tiny icon set, drawn with plain rects/circles ------------------ */

static void draw_icon(uint32_t x, uint32_t y, int icon, uint32_t color) {
    /* Each icon is drawn inside a nominal 20x20 box at (x,y). */
    switch (icon) {
        case UI_ICON_GRID:
            gfx_fill_rect(x, y, 8, 8, color);
            gfx_fill_rect(x + 12, y, 8, 8, color);
            gfx_fill_rect(x, y + 12, 8, 8, color);
            gfx_fill_rect(x + 12, y + 12, 8, 8, color);
            break;
        case UI_ICON_CPU:
            gfx_rect_outline(x + 3, y + 3, 14, 14, color);
            gfx_fill_rect(x + 6, y + 6, 8, 8, color);
            /* pins */
            gfx_hline(x, y + 6, 3, color);
            gfx_hline(x, y + 13, 3, color);
            gfx_hline(x + 17, y + 6, 3, color);
            gfx_hline(x + 17, y + 13, 3, color);
            gfx_vline(x + 6, y, 3, color);
            gfx_vline(x + 13, y, 3, color);
            gfx_vline(x + 6, y + 17, 3, color);
            gfx_vline(x + 13, y + 17, 3, color);
            break;
        case UI_ICON_MEMORY:
            gfx_rect_outline(x + 1, y + 4, 18, 12, color);
            gfx_vline(x + 6, y + 4, 12, color);
            gfx_vline(x + 10, y + 4, 12, color);
            gfx_vline(x + 14, y + 4, 12, color);
            break;
        case UI_ICON_DISPLAY:
            gfx_rect_outline(x + 1, y + 2, 18, 12, color);
            gfx_fill_rect(x + 7, y + 15, 6, 2, color);
            gfx_hline(x + 4, y + 18, 12, color);
            break;
        case UI_ICON_SYSTEM:
            gfx_fill_circle(x + 10, y + 10, 8, color);
            gfx_fill_circle(x + 10, y + 10, 4, gfx_palette_color(VGA_DARK_GREY));
            break;
        case UI_ICON_BATTERY:
            gfx_rect_outline(x + 1, y + 4, 15, 12, color);
            gfx_fill_rect(x + 16, y + 7, 3, 6, color);   /* terminal nub */
            gfx_fill_rect(x + 4, y + 7, 9, 6, color);    /* charge level */
            break;
        case UI_ICON_APPS:
            gfx_rect_outline(x + 2, y + 2, 16, 16, color);
            gfx_fill_rect(x + 6, y + 6, 3, 3, color);
            gfx_fill_rect(x + 12, y + 6, 3, 3, color);
            gfx_fill_rect(x + 6, y + 12, 3, 3, color);
            gfx_fill_rect(x + 12, y + 12, 3, 3, color);
            break;
        case UI_ICON_FILES:
            gfx_fill_rect(x + 1, y + 4, 18, 14, color);
            gfx_fill_rect(x + 3, y + 2, 8, 3, color);
            gfx_fill_rect(x + 4, y + 8, 12, 2, gfx_palette_color(VGA_DARK_GREY));
            gfx_fill_rect(x + 4, y + 12, 9, 2, gfx_palette_color(VGA_DARK_GREY));
            break;
        default:
            break;
    }
}

/* ---- top bar ---------------------------------------------------------- */

static void draw_topbar(void) {
    uint32_t bg = gfx_palette_color(VGA_DARK_GREY);
    uint32_t accent = gfx_palette_color(VGA_LIGHT_BLUE);
    uint32_t sub = gfx_palette_color(VGA_LIGHT_GREY);
    uint32_t bright = gfx_palette_color(VGA_WHITE);

    gfx_fill_rect(0, 0, screen_w, TOPBAR_H, bg);
    gfx_hline(0, TOPBAR_H - 2, screen_w, accent);

    /* Little logo mark: a rounded accent square with a void "ring" in it. */
    gfx_fill_card(PAD, 16, 32, 32, accent, 8);
    gfx_fill_circle(PAD + 16, 16 + 16, 9, bg);
    gfx_fill_circle(PAD + 16, 16 + 16, 4, accent);

    /* "VoidOS" at scale 2 is 32px tall; the subtitle below it needs to
     * start after those 32px end or it paints its own background over
     * the wordmark's bottom rows, clipping it. */
    gfx_draw_text(PAD + 44, 10, "VoidOS", bright, bg, 2);
    gfx_draw_text(PAD + 44, 46, "hardware inspector", sub, bg, 1);
}

/* ---- sidebar ----------------------------------------------------------- */

static void draw_sidebar(const struct ui_section* sections, int count, int selected) {
    uint32_t bg = gfx_palette_color(VGA_BLACK);
    uint32_t item_bg = gfx_palette_color(VGA_DARK_GREY);
    uint32_t accent = gfx_palette_color(VGA_LIGHT_BLUE);
    uint32_t text = gfx_palette_color(VGA_LIGHT_GREY);
    uint32_t text_selected = gfx_palette_color(VGA_WHITE);

    gfx_fill_rect(0, TOPBAR_H, SIDEBAR_W, screen_h - TOPBAR_H, bg);

    uint32_t y = TOPBAR_H + PAD;
    for (int i = 0; i < count; i++) {
        int is_sel = (i == selected);
        if (is_sel) {
            gfx_fill_card(10, y, SIDEBAR_W - 20, ITEM_H - 8, item_bg, 8);
            gfx_fill_rect(0, y + 6, 4, ITEM_H - 20, accent);
        }
        draw_icon(28, y + (ITEM_H - 8) / 2 - 10, sections[i].icon, is_sel ? accent : text);
        gfx_draw_text(60, y + (ITEM_H - 8) / 2 - 8, sections[i].label,
                      is_sel ? text_selected : text, is_sel ? item_bg : bg, 1);
        y += ITEM_H;
    }

    /* footer hint, pinned to the bottom of the sidebar */
    uint32_t hint_y = screen_h - 40;
    gfx_hline(16, hint_y - 8, SIDEBAR_W - 32, gfx_palette_color(VGA_DARK_GREY));
    gfx_draw_text(16, hint_y, "Left/Right: sections", gfx_palette_color(VGA_DARK_GREY), bg, 1);
    gfx_draw_text(16, hint_y + 16, "Esc: shut down", gfx_palette_color(VGA_DARK_GREY), bg, 1);
}

/* ---- content card -------------------------------------------------- */

static void draw_card(void) {
    uint32_t area_x = SIDEBAR_W + PAD;
    uint32_t area_y = TOPBAR_H + PAD;
    uint32_t area_w = (screen_w > SIDEBAR_W + 2 * PAD) ? screen_w - SIDEBAR_W - 2 * PAD : 0;
    uint32_t area_h = (screen_h > TOPBAR_H + 2 * PAD) ? screen_h - TOPBAR_H - 2 * PAD : 0;

    /* app background behind everything to the right of the sidebar */
    gfx_fill_rect(SIDEBAR_W, TOPBAR_H, screen_w - SIDEBAR_W, screen_h - TOPBAR_H,
                  gfx_palette_color(VGA_BLACK));

    gfx_fill_card(area_x, area_y, area_w, area_h, gfx_palette_color(VGA_DARK_GREY), CARD_RADIUS);
}

void ui_draw_shell(const struct ui_section* sections, int count, int selected) {
    if (!gfx_available()) return;
    layout_metrics();

    gfx_fill_rect(0, 0, screen_w, screen_h, gfx_palette_color(VGA_BLACK));
    draw_topbar();
    draw_sidebar(sections, count, selected);
    draw_card();
}

int ui_hit_test_sidebar(uint32_t mx, uint32_t my, int count) {
    if (!gfx_available()) return -1;
    if (mx >= SIDEBAR_W || my < TOPBAR_H) return -1;
    uint32_t rel = my - TOPBAR_H;
    if (rel < PAD) return -1;
    rel -= PAD;
    int idx = (int)(rel / ITEM_H);
    if (idx < 0 || idx >= count) return -1;
    /* also reject the gap between items so hovering the seam doesn't count */
    if (rel - (uint32_t)idx * ITEM_H >= ITEM_H - 4) return -1;
    return idx;
}

/* Cursor backing store: the arrow silhouette below is at most 10px wide
 * (row 8..13 hold steady at width 9, plus the 1px outline pixel) and
 * 14px tall, so that's all we need to save/restore per move instead of
 * repainting the whole screen just to erase the old cursor. */
#define CURSOR_W 10
#define CURSOR_H 14

static uint32_t cursor_backing[CURSOR_H][CURSOR_W];
static int32_t  cursor_bx, cursor_by;
static int      cursor_backing_valid = 0;

void ui_cursor_invalidate(void) {
    cursor_backing_valid = 0;
}

static void cursor_restore(void) {
    if (!cursor_backing_valid) return;
    for (uint32_t r = 0; r < CURSOR_H; r++)
        for (uint32_t c = 0; c < CURSOR_W; c++)
            gfx_fill_rect((uint32_t)cursor_bx + c, (uint32_t)cursor_by + r, 1, 1, cursor_backing[r][c]);
    cursor_backing_valid = 0;
}

void ui_draw_cursor(int32_t x, int32_t y) {
    if (!gfx_available()) return;

    /* Erase the cursor from its old spot (if we have one saved) before
     * touching anything else - this is the only screen update a plain
     * mouse move needs, which is what kills the whole-screen flicker. */
    cursor_restore();
    if (x < 0 || y < 0) return;
    uint32_t ux = (uint32_t)x, uy = (uint32_t)y;

    for (uint32_t r = 0; r < CURSOR_H; r++)
        for (uint32_t c = 0; c < CURSOR_W; c++)
            cursor_backing[r][c] = gfx_get_pixel(ux + c, uy + r);
    cursor_bx = x;
    cursor_by = y;
    cursor_backing_valid = 1;

    uint32_t fg = gfx_palette_color(VGA_WHITE);
    uint32_t outline = gfx_rgb(10, 10, 20);

    /* Classic arrow silhouette: a triangle that grows for a few rows
     * then holds steady, drawn as stacked horizontal spans with a
     * one-pixel outline down the right edge for definition against
     * light backgrounds. */
    for (int row = 0; row < 14; row++) {
        int width = (row < 8) ? row + 2 : 9;
        gfx_hline(ux, uy + (uint32_t)row, (uint32_t)width, fg);
        gfx_fill_rect(ux + (uint32_t)width, uy + (uint32_t)row, 1, 1, outline);
    }
    gfx_vline(ux, uy, 14, outline);
}
