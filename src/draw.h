/* VoidOS window manager -- internal drawing helpers (cairo + pango)
 *
 * Not part of the public voidwm.h contract; only .c files in this
 * project include it.
 */
#ifndef VOIDWM_DRAW_H
#define VOIDWM_DRAW_H

#include <X11/Xlib.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <pango/pangocairo.h>

/* One drawing surface bound to an X window. Created once per
 * long-lived window (bar, dock, each client frame) and reused --
 * cheaper than recreating cairo/pango state every redraw. */
typedef struct {
    Window          win;
    int             w, h;
    cairo_surface_t *surf;
    cairo_t         *cr;
} Ctx;

/* Must be called once after the display/argb visual are known. */
void draw_init(void);
void draw_cleanup(void);

/* Bind/rebind a Ctx to a window of the given pixel size. Safe to
 * call again on resize (recreates the surface). */
void ctx_bind(Ctx *c, Window w, int width, int height);
void ctx_free(Ctx *c);

/* Present: flush cairo drawing to the X window. */
void ctx_flush(Ctx *c);

/* 0xAARRGGBB -> straight-alpha RGBA in cairo's 0..1 space. */
void draw_set_source(cairo_t *cr, unsigned int argb);

/* Fills the whole ctx with a rounded rect of the given color.
 * radius <= 0 draws plain square corners. */
void draw_panel(Ctx *c, unsigned int bg_argb, unsigned int border_argb, double radius);

/* Draws a filled rounded rect at an arbitrary offset within the ctx. */
void draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double radius);

/* Text, left-anchored at (x,y-baseline-ish via pango), vertically
 * centered on the ctx's full height. Returns rendered width in px. */
int draw_text(Ctx *c, double x, const char *utf8, unsigned int argb,
              double font_size, int bold);

/* Text centered horizontally within [x, x+w]. Returns rendered width. */
int draw_text_centered(Ctx *c, double x, double w, const char *utf8,
                        unsigned int argb, double font_size, int bold);

/* Measures text width/height without drawing, for layout. */
void measure_text(const char *utf8, double font_size, int bold, int *w, int *h);

/* Small vector glyphs used in the bar (battery outline, dot). No
 * bitmap assets anywhere in this codebase. */
void draw_battery_glyph(Ctx *c, double x, double y, double w, double h,
                         double pct, int charging);

/* Procedural dock-app glyphs (terminal, folder, globe, editor, gear).
 * `glyph` is one of the DOCK_GLYPH_* constants from config.h. Drawn
 * into a `size`x`size` box centered at (cx, cy). No bitmap assets. */
void draw_dock_glyph(cairo_t *cr, int glyph, double cx, double cy,
                      double size, unsigned int argb);

#endif /* VOIDWM_DRAW_H */