#ifndef UI_H
#define UI_H

#include <stdint.h>

/* VoidOS GUI shell: a top bar with the wordmark, a left sidebar you
 * navigate with Left/Right, and a rounded content card where section
 * text (CPU/Memory/etc.) is rendered via the existing terminal_*
 * scrollback API, positioned with terminal_set_viewport(). Everything
 * here is a no-op if gfx_available() is false (legacy VGA text mode),
 * so kernel.c can call it unconditionally. */

struct ui_section {
    const char* label; /* shown in the sidebar */
    int icon;           /* UI_ICON_* below */
};

enum {
    UI_ICON_GRID = 0,
    UI_ICON_CPU,
    UI_ICON_MEMORY,
    UI_ICON_DISPLAY,
    UI_ICON_SYSTEM,
    UI_ICON_BATTERY,
    UI_ICON_APPS,
    UI_ICON_FILES,
};

/* Draws the full shell (background, top bar, sidebar with the given
 * section highlighted, and an empty content card). Call once at
 * startup and again each time the selected section changes. */
void ui_draw_shell(const struct ui_section* sections, int count, int selected);

/* Pixel rectangle of the *inside* of the content card, with a little
 * padding already applied, in a form ready for terminal_set_viewport(). */
void ui_content_viewport(uint32_t* x, uint32_t* y, uint32_t* w, uint32_t* h);

/* Returns the index of the sidebar item under (mx, my), or -1 if the
 * point isn't over any of them (e.g. it's in the content card, or off
 * the sidebar entirely). Used to turn a left click into navigation. */
int ui_hit_test_sidebar(uint32_t mx, uint32_t my, int count);

/* Draws the mouse pointer at (x, y). Call last, after the shell and
 * content, so it's never drawn over by anything else. Internally saves
 * whatever pixels sit under the new position and restores whatever it
 * saved under the previous position first, so a plain mouse move only
 * touches the small cursor-sized rect instead of needing a full-screen
 * repaint to erase the old one. No-op if gfx_available() is false. */
void ui_draw_cursor(int32_t x, int32_t y);

/* Erases the currently drawn cursor and invalidates its backing pixels.
 * Call before repainting any region that may overlap the cursor. */
void ui_cursor_erase(void);

/* Call after anything else repaints pixels under the cursor (e.g. a
 * full ui_draw_shell()) so the next ui_draw_cursor() re-captures fresh
 * backing pixels instead of restoring stale ones over the new content. */
void ui_cursor_invalidate(void);

#endif
