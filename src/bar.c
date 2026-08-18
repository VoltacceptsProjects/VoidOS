/* VoidOS window manager -- top status bar
 *
 * Full-width panel at the top of the screen: workspace dots on the
 * left (click to switch), focused window title in the middle, clock
 * and a procedurally-drawn battery glyph on the right.
 *
 * Battery data comes from /sys/class/power_supply (real hardware,
 * Linux only). On a machine with no battery (desktops, VMs) the
 * glyph is simply omitted.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <X11/Xlib.h>

#include "voidwm.h"
#include "config.h"
#include "draw.h"

static Window bar_win = 0;
static Ctx    bar_ctx;

#define WS_DOT_D    7
#define WS_DOT_GAP  10
#define WS_PAD_L    14
#define SIDE_PAD    16

static int ws_dot_x0[NUM_WORKSPACES];
static int ws_dot_x1[NUM_WORKSPACES];

/* ---- battery (Linux /sys, real hardware only) --------------------
 * Returns percentage 0..100, or -1 if no battery is present. */
static int
read_battery(int *charging)
{
    FILE *f;
    char line[64];
    int pct = -1;
    *charging = 0;

    f = fopen("/sys/class/power_supply/BAT0/capacity", "r");
    if (!f) f = fopen("/sys/class/power_supply/BAT1/capacity", "r");
    if (f) {
        if (fgets(line, sizeof line, f)) pct = atoi(line);
        fclose(f);
    }
    if (pct < 0) return -1;

    f = fopen("/sys/class/power_supply/BAT0/status", "r");
    if (!f) f = fopen("/sys/class/power_supply/BAT1/status", "r");
    if (f) {
        if (fgets(line, sizeof line, f))
            *charging = (strncmp(line, "Charging", 8) == 0);
        fclose(f);
    }
    return pct;
}

void
bar_create(void)
{
    XSetWindowAttributes swa;
    Visual *vis   = argb_visual ? argb_visual : DefaultVisual(dpy, screen);
    int     depth = argb_visual ? argb_depth  : DefaultDepth(dpy, screen);

    swa.colormap         = argb_visual ? argb_cmap : DefaultColormap(dpy, screen);
    swa.border_pixel     = 0;
    swa.background_pixel = 0;
    swa.event_mask       = ExposureMask | ButtonPressMask;
    swa.override_redirect = True; /* chrome, not managed */

    bar_win = XCreateWindow(dpy, root, 0, 0, scr_w, BAR_HEIGHT, 0, depth,
                             InputOutput, vis,
                             CWColormap | CWBorderPixel | CWBackPixel |
                             CWEventMask | CWOverrideRedirect, &swa);

    ctx_bind(&bar_ctx, bar_win, scr_w, BAR_HEIGHT);
    XMapWindow(dpy, bar_win);
    XRaiseWindow(dpy, bar_win);
}

Window bar_window(void) { return bar_win; }

/* Called after the root window's geometry changes (VM guest-tools /
 * RandR resize). Re-reads the now-current scr_w/scr_h and stretches
 * the bar to match, rebinding cairo to the new size. */
void
bar_resize(void)
{
    if (!bar_win) return;
    XMoveResizeWindow(dpy, bar_win, 0, 0, scr_w, BAR_HEIGHT);
    ctx_bind(&bar_ctx, bar_win, scr_w, BAR_HEIGHT);
    bar_redraw();
}

void
bar_redraw(void)
{
    if (!bar_win) return;
    draw_panel(&bar_ctx, COL_BAR_BG, COL_BAR_BORDER, BAR_RADIUS);

    /* -- left: wordmark + workspace dots -- */
    int x = SIDE_PAD;
    x += draw_text(&bar_ctx, x, "VoidOS", COL_TEXT_PRIMARY, FONT_SIZE_BAR, 1);
    x += 18;

    int n = wm_num_workspaces();
    int cur = wm_current_workspace();
    double cy = BAR_HEIGHT / 2.0;
    for (int i = 0; i < n && i < NUM_WORKSPACES; i++) {
        int occupied = wm_workspace_occupied(i);
        int active   = (i == cur);
        double r = active ? WS_DOT_D / 2.0 + 1.0 : WS_DOT_D / 2.0;
        unsigned int col = active ? COL_ACCENT : (occupied ? COL_TEXT_MUTED : COL_TEXT_FAINT);

        ws_dot_x0[i] = x - WS_DOT_GAP / 2;
        cairo_new_sub_path(bar_ctx.cr);
        cairo_arc(bar_ctx.cr, x + WS_DOT_D / 2.0, cy, r, 0, 2 * 3.14159265);
        draw_set_source(bar_ctx.cr, col);
        cairo_fill(bar_ctx.cr);
        x += WS_DOT_D;
        ws_dot_x1[i] = x + WS_DOT_GAP / 2;
        x += WS_DOT_GAP;
    }

    /* -- middle: focused window title, truncated to fit -- */
    const char *title = wm_focused_title();
    if (title && title[0]) {
        char buf[300];
        strncpy(buf, title, sizeof buf - 1);
        buf[sizeof buf - 1] = '\0';
        int tw, th;
        measure_text(buf, FONT_SIZE_BAR, 0, &tw, &th);
        while (tw > scr_w / 2 && strlen(buf) > 4) {
            buf[strlen(buf) - 1] = '\0';
            measure_text(buf, FONT_SIZE_BAR, 0, &tw, &th);
        }
        draw_text_centered(&bar_ctx, 0, scr_w, buf, COL_TEXT_MUTED, FONT_SIZE_BAR, 0);
    }

    /* -- right: battery + clock -- */
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char clock_buf[16];
    strftime(clock_buf, sizeof clock_buf, "%H:%M", &tmv);

    int cw, ch;
    measure_text(clock_buf, FONT_SIZE_BAR, 1, &cw, &ch);
    int rx = scr_w - SIDE_PAD - cw;
    draw_text(&bar_ctx, rx, clock_buf, COL_TEXT_PRIMARY, FONT_SIZE_BAR, 1);

    int charging;
    int pct = read_battery(&charging);
    if (pct >= 0) {
        double bw = 22, bh = 11;
        double bx = rx - 12 - bw;
        char pctbuf[16];
        snprintf(pctbuf, sizeof pctbuf, "%d%%", pct < 0 ? 0 : (pct > 100 ? 100 : pct));
        int pw, ph;
        measure_text(pctbuf, FONT_SIZE_BAR, 0, &pw, &ph);
        bx -= pw + 6;
        draw_text(&bar_ctx, bx + bw + 6, pctbuf, COL_TEXT_MUTED, FONT_SIZE_BAR, 0);
        draw_battery_glyph(&bar_ctx, bx, (BAR_HEIGHT - bh) / 2.0, bw, bh, pct, charging);
    }

    ctx_flush(&bar_ctx);
}

void
bar_tick(void)
{
    bar_redraw();
}

int
bar_handle_click(int x, int y)
{
    (void)y;
    int n = wm_num_workspaces();
    for (int i = 0; i < n && i < NUM_WORKSPACES; i++) {
        if (x >= ws_dot_x0[i] && x <= ws_dot_x1[i]) {
            act_view_workspace(i);
            return 1;
        }
    }
    return 0;
}
