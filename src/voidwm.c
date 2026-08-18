/* VoidOS window manager -- core
 *
 * A small floating, reparenting X11 window manager: bar + auto-hide
 * dock + per-window titlebar with traffic-light controls + virtual
 * workspaces. Built for real Xorg sessions on real hardware (a
 * .xsession / .desktop entry is provided in the repo root).
 *
 * Design notes:
 *  - Every client gets a "frame" top-level window (its background
 *    IS the border color) containing a titlebar child window and
 *    the reparented client window.
 *  - Compositing (blur, real translucency) is left to picom running
 *    alongside this WM against the ARGB visual -- see config.h.
 *  - No Wayland support: Wayland requires an entirely different
 *    architecture (a wlroots-based compositor is its own display
 *    server). See README.md for what that would take.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#include <X11/XKBlib.h>
#include <X11/extensions/shape.h>

#include "voidwm.h"
#include "config.h"
#include "draw.h"

/* ---- public globals (declared extern in voidwm.h) ---- */
Display *dpy;
int      screen;
Window   root;
int      scr_w, scr_h;
Visual  *argb_visual = NULL;
int      argb_depth  = 0;
Colormap argb_cmap   = 0;

/* ---- traffic-light dot geometry inside the titlebar ---- */
#define DOT_D        11
#define DOT_GAP      8
#define DOT_LEFT_PAD 12

typedef struct Client {
    Window win;        /* the application's window       */
    Window frame;       /* our top-level frame (= border)  */
    Window titlebar;    /* child of frame, top strip       */
    Ctx    tctx;         /* cairo ctx bound to titlebar      */
    int    ws;
    int    x, y, w, h;   /* frame geometry                   */
    int    cw, ch;        /* client (app) content size         */
    int    fullscreen;
    int    hidden;         /* minimized                          */
    int    sx, sy, sw, sh;  /* saved geometry for un-maximize/fs   */
    int    maximized;
    char   title[256];
    struct Client *next;
} Client;

static Client *clients  = NULL;
static Client *focused  = NULL;
static int     cur_ws   = 0;
static int     running  = 1;
static Cursor  cur_normal, cur_move, cur_resize;
static int     have_shape_ext = 0;

static Atom A_WM_DELETE_WINDOW, A_WM_PROTOCOLS, A_UTF8_STRING, A_NET_WM_NAME;

typedef enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE } DragMode;
static struct {
    DragMode mode;
    Client  *c;
    int      start_px, start_py;
    int      orig_x, orig_y, orig_w, orig_h;
} drag = { DRAG_NONE, NULL, 0, 0, 0, 0, 0, 0 };

/* ================================================================
 * Small helpers
 * ================================================================ */

static unsigned long
alloc_pixel(unsigned int argb)
{
    XColor c;
    c.red   = ((argb >> 16) & 0xFF) * 257;
    c.green = ((argb >> 8)  & 0xFF) * 257;
    c.blue  = ( argb        & 0xFF) * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, DefaultColormap(dpy, screen), &c);
    return c.pixel;
}

void
spawn(const char *shell_cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        setsid();
        execl("/bin/sh", "sh", "-c", shell_cmd, (char *)NULL);
        _exit(127);
    }
}

void
wm_quit(void)
{
    running = 0;
}

static Client *client_for_win(Window w)
{
    for (Client *c = clients; c; c = c->next)
        if (c->win == w || c->frame == w || c->titlebar == w) return c;
    return NULL;
}

static int workspace_count(int ws)
{
    int n = 0;
    for (Client *c = clients; c; c = c->next)
        if (c->ws == ws && !c->hidden) n++;
    return n;
}

/* ================================================================
 * Titlebar / chrome drawing
 * ================================================================ */

static void
draw_dot(cairo_t *cr, double cx, double cy, unsigned int argb)
{
    cairo_arc(cr, cx, cy, DOT_D / 2.0, 0, 2 * 3.14159265);
    draw_set_source(cr, argb);
    cairo_fill(cr);
}

static void
redraw_titlebar(Client *c)
{
    if (!c || c->hidden) return;
    int focus = (c == focused);

    XSetWindowBackground(dpy, c->frame, alloc_pixel(focus ? COL_BORDER_FOCUS : COL_BORDER_UNFOCUS));
    XClearWindow(dpy, c->frame);

    ctx_bind(&c->tctx, c->titlebar, c->w - 2 * BORDER_WIDTH, TITLEBAR_HEIGHT);
    draw_panel(&c->tctx, focus ? COL_TITLEBAR_FOCUS : COL_TITLEBAR_UNFOCUS, 0, 0);

    double cy = TITLEBAR_HEIGHT / 2.0;
    double cx = DOT_LEFT_PAD + DOT_D / 2.0;
    /* left->right, matching the reference design: close (red),
     * minimize (yellow), maximize/zoom (green). */
    draw_dot(c->tctx.cr, cx, cy, COL_DOT_CLOSE);
    cx += DOT_D + DOT_GAP;
    draw_dot(c->tctx.cr, cx, cy, COL_DOT_MAX);   /* yellow -> minimize */
    cx += DOT_D + DOT_GAP;
    draw_dot(c->tctx.cr, cx, cy, COL_DOT_MIN);   /* green  -> maximize */

    const char *title = c->title[0] ? c->title : "Untitled";
    draw_text_centered(&c->tctx, 0, c->tctx.w, title,
                        focus ? COL_TEXT_PRIMARY : COL_TEXT_MUTED,
                        FONT_SIZE_TITLEBAR, 1);
    ctx_flush(&c->tctx);
}

/* ================================================================
 * Focus / stacking / workspaces
 * ================================================================ */

static void
restack(void)
{
    Window bw = bar_window(), dw = dock_window();
    if (bw) XRaiseWindow(dpy, bw);
    if (dw) XRaiseWindow(dpy, dw);
}

static void
apply_workspace_visibility(void)
{
    for (Client *c = clients; c; c = c->next) {
        int show = (c->ws == cur_ws) && !c->hidden;
        if (show) XMapWindow(dpy, c->frame);
        else      XUnmapWindow(dpy, c->frame);
    }
}

static void
focus_client(Client *c)
{
    Client *prev = focused;
    focused = c;
    if (prev && prev != c) redraw_titlebar(prev);
    if (c) {
        XRaiseWindow(dpy, c->frame);
        XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
        redraw_titlebar(c);
    } else {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    }
    restack();
    bar_redraw();
}

static void
focus_first_in_workspace(int ws)
{
    for (Client *c = clients; c; c = c->next)
        if (c->ws == ws && !c->hidden) { focus_client(c); return; }
    focus_client(NULL);
}

/* ================================================================
 * Geometry: move / resize the frame + reflow children
 * ================================================================ */

/* Round the frame's corners via the X Shape extension. A window's
 * pixels are otherwise a plain rectangle, so this builds a 1-bit
 * mask (straight cross + four corner disks) and applies it as the
 * frame's bounding shape. Skipped for maximized/fullscreen windows,
 * which should read as flush with the screen edges. */
static void
apply_frame_shape(Client *c)
{
    if (!have_shape_ext) return;

    double radius = (c->maximized || WINDOW_RADIUS <= 0.0) ? 0.0 : WINDOW_RADIUS;
    if (radius <= 0.0) {
        XShapeCombineMask(dpy, c->frame, ShapeBounding, 0, 0, None, ShapeSet);
        return;
    }

    int w = c->w, h = c->h;
    int r = (int)radius;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0 || w <= 0 || h <= 0) return;

    Pixmap mask = XCreatePixmap(dpy, c->frame, w, h, 1);
    GC gc = XCreateGC(dpy, mask, 0, NULL);

    XSetForeground(dpy, gc, 0);
    XFillRectangle(dpy, mask, gc, 0, 0, w, h);

    XSetForeground(dpy, gc, 1);
    XFillRectangle(dpy, mask, gc, r, 0, w - 2 * r, h);
    XFillRectangle(dpy, mask, gc, 0, r, w, h - 2 * r);
    XFillArc(dpy, mask, gc, 0,         0,         2 * r, 2 * r, 0, 360 * 64);
    XFillArc(dpy, mask, gc, w - 2 * r, 0,         2 * r, 2 * r, 0, 360 * 64);
    XFillArc(dpy, mask, gc, 0,         h - 2 * r, 2 * r, 2 * r, 0, 360 * 64);
    XFillArc(dpy, mask, gc, w - 2 * r, h - 2 * r, 2 * r, 2 * r, 0, 360 * 64);

    XShapeCombineMask(dpy, c->frame, ShapeBounding, 0, 0, mask, ShapeSet);

    XFreeGC(dpy, gc);
    XFreePixmap(dpy, mask);
}

static void
apply_geometry(Client *c)
{
    XMoveResizeWindow(dpy, c->frame, c->x, c->y, c->w, c->h);
    apply_frame_shape(c);
    XResizeWindow(dpy, c->titlebar, c->w - 2 * BORDER_WIDTH, TITLEBAR_HEIGHT);
    c->cw = c->w - 2 * BORDER_WIDTH;
    c->ch = c->h - 2 * BORDER_WIDTH - TITLEBAR_HEIGHT;
    if (c->cw < 1) c->cw = 1;
    if (c->ch < 1) c->ch = 1;
    XMoveResizeWindow(dpy, c->win, BORDER_WIDTH, BORDER_WIDTH + TITLEBAR_HEIGHT, c->cw, c->ch);
    redraw_titlebar(c);
}

static void
client_toggle_maximize(Client *c)
{
    if (!c) return;
    if (!c->maximized) {
        c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h;
        c->x = 0; c->y = BAR_HEIGHT;
        c->w = scr_w;
        c->h = scr_h - BAR_HEIGHT;
        c->maximized = 1;
    } else {
        c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
        c->maximized = 0;
    }
    apply_geometry(c);
}

/* ================================================================
 * Framing new / removing old clients
 * ================================================================ */

static void
set_wm_protocols_delete(Window w)
{
    Atom *protocols; int n;
    if (XGetWMProtocols(dpy, w, &protocols, &n)) {
        for (int i = 0; i < n; i++)
            if (protocols[i] == A_WM_DELETE_WINDOW) { XFree(protocols); return; }
        XFree(protocols);
    }
}

static void
update_title(Client *c)
{
    char *name = NULL;
    if (XFetchName(dpy, c->win, &name) && name) {
        strncpy(c->title, name, sizeof c->title - 1);
        c->title[sizeof c->title - 1] = '\0';
        XFree(name);
    } else {
        XTextProperty tp;
        if (XGetTextProperty(dpy, c->win, &tp, A_NET_WM_NAME) && tp.value) {
            strncpy(c->title, (char *)tp.value, sizeof c->title - 1);
            c->title[sizeof c->title - 1] = '\0';
            XFree(tp.value);
        } else {
            strcpy(c->title, "Untitled");
        }
    }
}

static void
frame_client(Window w)
{
    if (client_for_win(w)) return;

    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa)) return;
    if (wa.override_redirect) return;

    Client *c = calloc(1, sizeof *c);
    c->win = w;
    c->ws  = cur_ws;

    int cw = wa.width  > 0 ? wa.width  : 640;
    int ch = wa.height > 0 ? wa.height : 400;
    if (cw < MIN_WIN_W) cw = MIN_WIN_W;
    if (ch < MIN_WIN_H) ch = MIN_WIN_H;

    int n = workspace_count(cur_ws);
    int off = (n % 8) * GAP;
    c->x = 60 + off;
    c->y = BAR_HEIGHT + 40 + off;
    c->w = cw + 2 * BORDER_WIDTH;
    c->h = ch + 2 * BORDER_WIDTH + TITLEBAR_HEIGHT;
    if (c->x + c->w > scr_w) c->x = scr_w - c->w > 0 ? scr_w - c->w : 0;
    if (c->y + c->h > scr_h) c->y = scr_h - c->h > 0 ? scr_h - c->h : BAR_HEIGHT;

    Visual *vis   = argb_visual ? argb_visual : DefaultVisual(dpy, screen);
    int     depth = argb_visual ? argb_depth  : DefaultDepth(dpy, screen);

    XSetWindowAttributes swa;
    swa.colormap          = argb_visual ? argb_cmap : DefaultColormap(dpy, screen);
    swa.border_pixel      = 0;
    swa.background_pixel  = alloc_pixel(COL_BORDER_UNFOCUS);
    swa.event_mask        = SubstructureNotifyMask | ExposureMask |
                             ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                             EnterWindowMask;

    c->frame = XCreateWindow(dpy, root, c->x, c->y, c->w, c->h, 0, depth,
                              InputOutput, vis,
                              CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &swa);

    XSetWindowAttributes tswa;
    tswa.colormap  = swa.colormap;
    tswa.border_pixel = 0;
    tswa.background_pixel = alloc_pixel(COL_TITLEBAR_UNFOCUS);
    tswa.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    c->titlebar = XCreateWindow(dpy, c->frame, BORDER_WIDTH, BORDER_WIDTH,
                                 c->w - 2 * BORDER_WIDTH, TITLEBAR_HEIGHT, 0, depth,
                                 InputOutput, vis,
                                 CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &tswa);

    XSetWindowBorderWidth(dpy, w, 0);
    XReparentWindow(dpy, w, c->frame, BORDER_WIDTH, BORDER_WIDTH + TITLEBAR_HEIGHT);
    XSelectInput(dpy, w, PropertyChangeMask | StructureNotifyMask);
    set_wm_protocols_delete(w);

    update_title(c);

    XMapWindow(dpy, c->titlebar);
    XMapWindow(dpy, w);
    if (c->ws == cur_ws) XMapWindow(dpy, c->frame);

    c->next = clients;
    clients = c;

    /* grab click-to-focus everywhere on the app window too */
    XGrabButton(dpy, Button1, AnyModifier, w, True, ButtonPressMask,
                GrabModeSync, GrabModeAsync, None, None);

    apply_geometry(c);
    focus_client(c);
}

static void
destroy_client(Client *c)
{
    Client **pp = &clients;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;

    if (focused == c) focused = NULL;
    ctx_free(&c->tctx);
    XDestroyWindow(dpy, c->titlebar);
    XDestroyWindow(dpy, c->frame);
    free(c);

    if (!focused) focus_first_in_workspace(cur_ws);
    bar_redraw();
}

/* ================================================================
 * Key-bound actions (table lives in config.h)
 * ================================================================ */

void act_spawn_terminal(int arg) { (void)arg; spawn(SPAWN_TERMINAL); }

void
act_kill_focused(int arg)
{
    (void)arg;
    if (!focused) return;
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = focused->win;
    ev.xclient.message_type = A_WM_PROTOCOLS;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = A_WM_DELETE_WINDOW;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, focused->win, False, NoEventMask, &ev);
}

void
act_view_workspace(int arg)
{
    if (arg < 0 || arg >= NUM_WORKSPACES || arg == cur_ws) return;
    cur_ws = arg;
    apply_workspace_visibility();
    focus_first_in_workspace(cur_ws);
    bar_redraw();
}

void
act_move_focused_to_workspace(int arg)
{
    if (!focused || arg < 0 || arg >= NUM_WORKSPACES) return;
    focused->ws = arg;
    XUnmapWindow(dpy, focused->frame);
    focused = NULL;
    focus_first_in_workspace(cur_ws);
    bar_redraw();
}

void
act_focus_cycle(int arg)
{
    (void)arg;
    if (!clients) return;
    /* un-hide + focus the next client in the current workspace,
     * wrapping around; this also restores minimized windows. */
    Client *start = focused;
    Client *c = start ? start->next : clients;
    for (int i = 0; i < 64; i++) {
        if (!c) c = clients;
        if (c->ws == cur_ws) {
            if (c->hidden) { c->hidden = 0; XMapWindow(dpy, c->frame); }
            focus_client(c);
            return;
        }
        if (c == start) break;
        c = c->next;
    }
}

void
act_toggle_fullscreen(int arg)
{
    (void)arg;
    if (focused) client_toggle_maximize(focused);
}

void act_quit(int arg) { (void)arg; wm_quit(); }

static void
client_minimize(Client *c)
{
    if (!c) return;
    c->hidden = 1;
    XUnmapWindow(dpy, c->frame);
    if (focused == c) { focused = NULL; focus_first_in_workspace(cur_ws); }
}

static void
client_close(Client *c)
{
    XEvent ev = {0};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = c->win;
    ev.xclient.message_type = A_WM_PROTOCOLS;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = A_WM_DELETE_WINDOW;
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, c->win, False, NoEventMask, &ev);
}

/* ================================================================
 * bar.c query API
 * ================================================================ */

const char *wm_focused_title(void) { return focused ? focused->title : ""; }
int wm_current_workspace(void)     { return cur_ws; }
int wm_num_workspaces(void)        { return NUM_WORKSPACES; }
int wm_workspace_occupied(int ws)
{
    for (Client *c = clients; c; c = c->next)
        if (c->ws == ws) return 1;
    return 0;
}

/* ================================================================
 * Event handlers
 * ================================================================ */

static void
on_map_request(XMapRequestEvent *e)
{
    frame_client(e->window);
}

static void
on_configure_request(XConfigureRequestEvent *e)
{
    Client *c = client_for_win(e->window);
    if (!c) {
        XWindowChanges wc;
        wc.x = e->x; wc.y = e->y; wc.width = e->width; wc.height = e->height;
        wc.border_width = e->border_width; wc.sibling = e->above; wc.stack_mode = e->detail;
        XConfigureWindow(dpy, e->window, e->value_mask, &wc);
        return;
    }
    /* We own geometry for framed clients; just ack with current geo. */
    XConfigureEvent ce = {0};
    ce.type = ConfigureNotify;
    ce.display = dpy; ce.event = c->win; ce.window = c->win;
    ce.x = BORDER_WIDTH; ce.y = BORDER_WIDTH + TITLEBAR_HEIGHT;
    ce.width = c->cw; ce.height = c->ch;
    ce.border_width = 0; ce.above = None; ce.override_redirect = False;
    XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

/* Fires whenever the root window's own geometry changes -- most
 * notably when VM guest tools (VirtualBox/VMware/QEMU+SPICE) or
 * `xrandr` resize the screen after the session has already started.
 * scr_w/scr_h were only read once at startup, so without this the
 * bar/dock geometry (and the dock's bottom hot-zone) silently goes
 * stale: they keep the old screen size while the real display is a
 * different size, so the dock can end up positioned off the visible
 * area, or its hot-zone no longer lines up with the actual bottom
 * edge at all. */
static void
on_root_configure_notify(XConfigureEvent *e)
{
    if (e->window != root) return;
    if (e->width == scr_w && e->height == scr_h) return;

    scr_w = e->width;
    scr_h = e->height;
    bar_resize();
    dock_resize();
}

static void
on_unmap_notify(XUnmapEvent *e)
{
    Client *c = client_for_win(e->window);
    if (!c || e->window != c->win) return;
    if (e->send_event) return; /* synthetic unmaps are not real withdrawals */
    destroy_client(c);
}

static void
on_destroy_notify(XDestroyWindowEvent *e)
{
    Client *c = client_for_win(e->window);
    if (c && e->window == c->win) destroy_client(c);
}

static void
on_property_notify(XPropertyEvent *e)
{
    Client *c = client_for_win(e->window);
    if (!c) return;
    if (e->atom == XA_WM_NAME || e->atom == A_NET_WM_NAME) {
        update_title(c);
        redraw_titlebar(c);
        if (c == focused) bar_redraw();
    }
}

/* returns 1 if (px,py) hit a dot and dispatched an action */
static int
hit_test_dots(Client *c, int lx)
{
    int base = DOT_LEFT_PAD;
    int zones[3] = { base, base + (DOT_D + DOT_GAP), base + 2 * (DOT_D + DOT_GAP) };
    for (int i = 0; i < 3; i++) {
        if (lx >= zones[i] - 4 && lx <= zones[i] + DOT_D + 4) {
            if (i == 0) client_close(c);
            else if (i == 1) client_minimize(c);
            else client_toggle_maximize(c);
            return 1;
        }
    }
    return 0;
}

static void
on_button_press(XButtonEvent *e)
{
    /* dock click? */
    if (dock_is_window(e->window)) {
        dock_handle_click(e->window, e->x, e->y);
        return;
    }

    /* bar click (workspace dots)? */
    if (e->window == bar_window()) {
        bar_handle_click(e->x, e->y);
        return;
    }

    Client *c = client_for_win(e->window);
    if (!c) {
        XAllowEvents(dpy, ReplayPointer, CurrentTime);
        return;
    }

    focus_client(c);
    XAllowEvents(dpy, ReplayPointer, CurrentTime);

    int on_titlebar = (e->window == c->titlebar);
    int is_mod_drag = (e->state & MODKEY) != 0;

    if (e->button == Button1 && (on_titlebar || is_mod_drag)) {
        if (on_titlebar && hit_test_dots(c, e->x)) return;
        drag.mode = DRAG_MOVE;
        drag.c = c;
        drag.start_px = e->x_root; drag.start_py = e->y_root;
        drag.orig_x = c->x; drag.orig_y = c->y; drag.orig_w = c->w; drag.orig_h = c->h;
        XDefineCursor(dpy, root, cur_move);
    } else if (is_mod_drag && e->button == Button3) {
        drag.mode = DRAG_RESIZE;
        drag.c = c;
        drag.start_px = e->x_root; drag.start_py = e->y_root;
        drag.orig_x = c->x; drag.orig_y = c->y; drag.orig_w = c->w; drag.orig_h = c->h;
        XDefineCursor(dpy, root, cur_resize);
    }
}

static void
on_button_release(XButtonEvent *e)
{
    (void)e;
    if (drag.mode != DRAG_NONE) {
        drag.mode = DRAG_NONE;
        drag.c = NULL;
        XDefineCursor(dpy, root, cur_normal);
    }
}

static void
on_motion_notify(XMotionEvent *e)
{
    if (drag.mode == DRAG_NONE || !drag.c) return;
    int dx = e->x_root - drag.start_px;
    int dy = e->y_root - drag.start_py;
    Client *c = drag.c;

    if (drag.mode == DRAG_MOVE) {
        c->x = drag.orig_x + dx;
        c->y = drag.orig_y + dy;
        if (c->y < 0) c->y = 0;
        XMoveWindow(dpy, c->frame, c->x, c->y);
    } else {
        c->w = drag.orig_w + dx; if (c->w < MIN_WIN_W) c->w = MIN_WIN_W;
        c->h = drag.orig_h + dy; if (c->h < MIN_WIN_H + TITLEBAR_HEIGHT) c->h = MIN_WIN_H + TITLEBAR_HEIGHT;
        c->maximized = 0;
        apply_geometry(c);
    }
}

static void
on_expose(XExposeEvent *e)
{
    if (e->count != 0) return;
    if (e->window == bar_window())  { bar_redraw(); return; }
    if (e->window == dock_window()) { dock_redraw(); return; }
    Client *c = client_for_win(e->window);
    if (c && e->window == c->titlebar) redraw_titlebar(c);
}

static void
on_key_press(XKeyEvent *e)
{
    KeySym ks = XkbKeycodeToKeysym(dpy, e->keycode, 0, 0);
    unsigned int state = e->state & ~(LockMask | Mod2Mask);
    for (int i = 0; i < NUM_KEYS; i++) {
        if (keys[i].keysym == ks && (keys[i].mod & state) == keys[i].mod &&
            (state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask)) ==
            (keys[i].mod & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask))) {
            keys[i].func(keys[i].arg);
            return;
        }
    }
}

/* ================================================================
 * Startup helpers
 * ================================================================ */

static int wm_detected = 0;
static int
xerror_start(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess) wm_detected = 1;
    return 0;
}

static int
xerror(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadWindow || e->error_code == BadDrawable) return 0;
    char buf[128];
    XGetErrorText(dpy, e->error_code, buf, sizeof buf);
    fprintf(stderr, "voidwm: X error %d (%s) request %d\n", e->error_code, buf, e->request_code);
    return 0;
}

static void
setup_argb_visual(void)
{
    if (!USE_ARGB_VISUAL) return;
    XVisualInfo vinfo;
    if (XMatchVisualInfo(dpy, screen, 32, TrueColor, &vinfo)) {
        argb_visual = vinfo.visual;
        argb_depth  = vinfo.depth;
        argb_cmap   = XCreateColormap(dpy, root, argb_visual, AllocNone);
    }
}

static void
grab_keys(void)
{
    XUngrabKey(dpy, AnyKey, AnyModifier, root);
    for (int i = 0; i < NUM_KEYS; i++) {
        KeyCode kc = XKeysymToKeycode(dpy, keys[i].keysym);
        if (!kc) continue;
        unsigned int mods[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
        for (unsigned m = 0; m < sizeof mods / sizeof *mods; m++)
            XGrabKey(dpy, kc, keys[i].mod | mods[m], root, True, GrabModeAsync, GrabModeAsync);
    }
}

static void
scan_existing(void)
{
    Window r, p, *kids; unsigned int n;
    if (!XQueryTree(dpy, root, &r, &p, &kids, &n)) return;
    for (unsigned i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, kids[i], &wa)) continue;
        if (wa.override_redirect || wa.map_state != IsViewable) continue;
        frame_client(kids[i]);
    }
    if (kids) XFree(kids);
}

int
main(void)
{
    signal(SIGCHLD, SIG_IGN);

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "voidwm: cannot open display\n"); return 1; }

    screen = DefaultScreen(dpy);
    root   = RootWindow(dpy, screen);
    scr_w  = DisplayWidth(dpy, screen);
    scr_h  = DisplayHeight(dpy, screen);

    XSetErrorHandler(xerror_start);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(dpy, False);
    if (wm_detected) {
        fprintf(stderr, "voidwm: another window manager is already running\n");
        return 1;
    }
    XSetErrorHandler(xerror);

    A_WM_PROTOCOLS      = XInternAtom(dpy, "WM_PROTOCOLS", False);
    A_WM_DELETE_WINDOW  = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    A_UTF8_STRING       = XInternAtom(dpy, "UTF8_STRING", False);
    A_NET_WM_NAME       = XInternAtom(dpy, "_NET_WM_NAME", False);

    { int shape_ev, shape_err;
      have_shape_ext = XShapeQueryExtension(dpy, &shape_ev, &shape_err); }

    setup_argb_visual();

    XSetWindowBackground(dpy, root, alloc_pixel(COL_ROOT_BG));
    XClearWindow(dpy, root);

    cur_normal = XCreateFontCursor(dpy, XC_left_ptr);
    cur_move   = XCreateFontCursor(dpy, XC_fleur);
    cur_resize = XCreateFontCursor(dpy, XC_sizing);
    XDefineCursor(dpy, root, cur_normal);

    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask |
                             StructureNotifyMask | KeyPressMask | ButtonPressMask);

    draw_init();
    bar_create();
    dock_create();
    grab_keys();
    scan_existing();
    restack();
    bar_redraw();

    int xfd = ConnectionNumber(dpy);
    struct timespec last_tick = {0, 0};
    long last_tick_ms = 0;

    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
                case MapRequest:      on_map_request(&ev.xmaprequest); break;
                case ConfigureRequest:on_configure_request(&ev.xconfigurerequest); break;
                case ConfigureNotify: on_root_configure_notify(&ev.xconfigure); break;
                case UnmapNotify:     on_unmap_notify(&ev.xunmap); break;
                case DestroyNotify:   on_destroy_notify(&ev.xdestroywindow); break;
                case PropertyNotify:  on_property_notify(&ev.xproperty); break;
                case ButtonPress:     on_button_press(&ev.xbutton); break;
                case ButtonRelease:   on_button_release(&ev.xbutton); break;
                case MotionNotify:    on_motion_notify(&ev.xmotion); break;
                case Expose:          on_expose(&ev.xexpose); break;
                case KeyPress:        on_key_press(&ev.xkey); break;
                default: break;
            }
        }

        fd_set fds; FD_ZERO(&fds); FD_SET(xfd, &fds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = POLL_INTERVAL_MS * 1000;
        select(xfd + 1, &fds, NULL, NULL, &tv);

        clock_gettime(CLOCK_MONOTONIC, &last_tick);
        long now_ms = last_tick.tv_sec * 1000L + last_tick.tv_nsec / 1000000L;

        int rx, ry, wx, wy; unsigned int mask; Window rw, cw;
        XQueryPointer(dpy, root, &rw, &cw, &rx, &ry, &wx, &wy, &mask);
        dock_poll(rx, ry, now_ms);

        if (now_ms - last_tick_ms >= 1000) {
            bar_tick();
            last_tick_ms = now_ms;
        }
    }

    draw_cleanup();
    XCloseDisplay(dpy);
    return 0;
}