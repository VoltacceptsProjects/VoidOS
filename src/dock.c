/* VoidOS window manager -- auto-hide dock
 *
 * A macOS-style dock of launcher buttons along the bottom edge. It
 * stays mapped at all times but is translated off the bottom of the
 * screen when hidden, and slid into view when the pointer nears the
 * bottom hot-zone (see DOCK_HOTZONE_PX / DOCK_LINGER_MS in config.h).
 * Icons are procedurally drawn vector glyphs (draw_dock_glyph) --
 * no bitmap assets.
 */
#include <string.h>

#include <X11/Xlib.h>

#include "voidwm.h"
#include "config.h"
#include "draw.h"

static Window dock_win = 0;
static Ctx    dock_ctx;
static int    dock_w, dock_h;
static int    dock_x, dock_y_shown, dock_y_hidden;
static int    dock_visible = 0;
static long   last_hover_ms = 0;
static int    hovered_index = -1;

/* small floating label shown above a hovered icon */
static Window tip_win = 0;
static Ctx    tip_ctx;
#define TIP_H 22
#define TIP_PAD 10

static int
icon_x0(int i)
{
    return DOCK_H_PADDING + i * (DOCK_ICON_SIZE + DOCK_ICON_GAP);
}

void
dock_create(void)
{
    dock_w = NUM_DOCKAPPS * DOCK_ICON_SIZE + (NUM_DOCKAPPS - 1) * DOCK_ICON_GAP + 2 * DOCK_H_PADDING;
    dock_h = DOCK_HEIGHT;
    dock_x = (scr_w - dock_w) / 2;
    dock_y_shown  = scr_h - dock_h - DOCK_BOTTOM_MARGIN;
    dock_y_hidden = scr_h; /* fully off-screen */

    XSetWindowAttributes swa;
    Visual *vis   = argb_visual ? argb_visual : DefaultVisual(dpy, screen);
    int     depth = argb_visual ? argb_depth  : DefaultDepth(dpy, screen);

    swa.colormap          = argb_visual ? argb_cmap : DefaultColormap(dpy, screen);
    swa.border_pixel      = 0;
    swa.background_pixel  = 0;
    swa.event_mask        = ExposureMask | ButtonPressMask | PointerMotionMask | LeaveWindowMask;
    swa.override_redirect = True;

    dock_win = XCreateWindow(dpy, root, dock_x, dock_y_hidden, dock_w, dock_h, 0, depth,
                              InputOutput, vis,
                              CWColormap | CWBorderPixel | CWBackPixel |
                              CWEventMask | CWOverrideRedirect, &swa);

    ctx_bind(&dock_ctx, dock_win, dock_w, dock_h);
    XMapWindow(dpy, dock_win);

    /* tooltip window: sized generously, repositioned/redrawn per-hover */
    swa.event_mask = ExposureMask;
    tip_win = XCreateWindow(dpy, root, -1000, -1000, 160, TIP_H, 0, depth,
                             InputOutput, vis,
                             CWColormap | CWBorderPixel | CWBackPixel |
                             CWEventMask | CWOverrideRedirect, &swa);
    ctx_bind(&tip_ctx, tip_win, 160, TIP_H);
    XMapWindow(dpy, tip_win);
}

Window dock_window(void) { return dock_win; }

/* Called after the root window's geometry changes (VM guest-tools /
 * RandR resize). scr_w/scr_h are read once at startup by dock_create();
 * without this, the hot-zone check and hidden/shown Y coordinates stay
 * pinned to the stale size, so the dock can end up positioned off the
 * (new, bigger) screen entirely and the pointer can never reach a
 * hot-zone that no longer lines up with the real bottom edge. */
void
dock_resize(void)
{
    if (!dock_win) return;

    dock_x        = (scr_w - dock_w) / 2;
    dock_y_shown  = scr_h - dock_h - DOCK_BOTTOM_MARGIN;
    dock_y_hidden = scr_h;

    XMoveWindow(dpy, dock_win, dock_x, dock_visible ? dock_y_shown : dock_y_hidden);
    dock_redraw();
}

int
dock_is_window(Window w)
{
    return w != 0 && w == dock_win;
}

void
dock_redraw(void)
{
    if (!dock_win) return;
    draw_panel(&dock_ctx, COL_DOCK_BG, COL_DOCK_BORDER, DOCK_RADIUS);

    for (int i = 0; i < NUM_DOCKAPPS; i++) {
        int x0 = icon_x0(i);
        double cx = x0 + DOCK_ICON_SIZE / 2.0;
        double cy = dock_h / 2.0;

        if (i == hovered_index) {
            draw_rounded_rect(dock_ctx.cr, x0 - 4, cy - DOCK_ICON_SIZE / 2.0 - 4,
                               DOCK_ICON_SIZE + 8, DOCK_ICON_SIZE + 8, 10.0);
            draw_set_source(dock_ctx.cr, COL_DOCK_ICON_HOVER);
            cairo_fill(dock_ctx.cr);
        }

        draw_dock_glyph(dock_ctx.cr, dockapps[i].glyph, cx, cy,
                         DOCK_ICON_SIZE * 0.62, COL_TEXT_PRIMARY);
    }

    ctx_flush(&dock_ctx);
}

/* Position + draw the floating tooltip above icon `i`, or hide it
 * (move off-screen) when i < 0. */
static void
update_tooltip(int i)
{
    if (i < 0 || !dock_visible) {
        XMoveWindow(dpy, tip_win, -1000, -1000);
        return;
    }
    int tw, th;
    measure_text(dockapps[i].label, FONT_SIZE_DOCK, 0, &tw, &th);
    int w = tw + 2 * TIP_PAD;
    if (w < 40) w = 40;

    XResizeWindow(dpy, tip_win, w, TIP_H);
    ctx_bind(&tip_ctx, tip_win, w, TIP_H);
    draw_panel(&tip_ctx, COL_DOCK_BG, COL_DOCK_BORDER, 8.0);
    draw_text_centered(&tip_ctx, 0, w, dockapps[i].label, COL_TEXT_PRIMARY, FONT_SIZE_DOCK, 0);
    ctx_flush(&tip_ctx);

    int icon_cx = dock_x + icon_x0(i) + DOCK_ICON_SIZE / 2;
    int tx = icon_cx - w / 2;
    int ty = dock_y_shown - TIP_H - 8;
    XMoveWindow(dpy, tip_win, tx, ty);
    XRaiseWindow(dpy, tip_win);
}

/* Called every event-loop tick (~POLL_INTERVAL_MS) with the current
 * pointer position and a monotonic millisecond clock. Handles both
 * the reveal hot-zone and the auto-hide linger timer, and tracks
 * which icon is hovered for the highlight effect. */
void
dock_poll(int pointer_x, int pointer_y, long now_ms)
{
    int near_edge = pointer_y >= scr_h - DOCK_HOTZONE_PX;
    int over_dock = dock_visible &&
                    pointer_x >= dock_x && pointer_x <= dock_x + dock_w &&
                    pointer_y >= dock_y_shown && pointer_y <= dock_y_shown + dock_h;

    if (near_edge || over_dock) {
        last_hover_ms = now_ms;
        if (!dock_visible) {
            dock_visible = 1;
            XMoveWindow(dpy, dock_win, dock_x, dock_y_shown);
            XRaiseWindow(dpy, dock_win);
        }
    }

    int new_hover = -1;
    if (over_dock) {
        int lx = pointer_x - dock_x;
        for (int i = 0; i < NUM_DOCKAPPS; i++) {
            int x0 = icon_x0(i);
            if (lx >= x0 - 6 && lx <= x0 + DOCK_ICON_SIZE + 6) { new_hover = i; break; }
        }
    }
    if (new_hover != hovered_index) {
        hovered_index = new_hover;
        if (dock_visible) dock_redraw();
        update_tooltip(hovered_index);
    }

    if (dock_visible && !over_dock && !near_edge &&
        (now_ms - last_hover_ms) >= DOCK_LINGER_MS) {
        dock_visible = 0;
        hovered_index = -1;
        XMoveWindow(dpy, dock_win, dock_x, dock_y_hidden);
        update_tooltip(-1);
    }
}

int
dock_handle_click(Window w, int x, int y)
{
    if (w != dock_win) return 0;
    (void)y;
    for (int i = 0; i < NUM_DOCKAPPS; i++) {
        int x0 = icon_x0(i);
        if (x >= x0 - 6 && x <= x0 + DOCK_ICON_SIZE + 6) {
            spawn(dockapps[i].cmd);
            return 1;
        }
    }
    return 0;
}
