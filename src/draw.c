/* VoidOS window manager -- drawing implementation
 *
 * All bar/dock/titlebar chrome is drawn here with cairo + pango.
 * Nothing here loads bitmap assets: panels are rounded rects, and
 * the only "icon" is a procedurally drawn battery outline. Real
 * frosted-glass blur comes from a running compositor (picom) acting
 * on the ARGB visual -- see config.h USE_ARGB_VISUAL.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "voidwm.h"
#include "config.h"
#include "draw.h"

static PangoFontDescription *font_regular = NULL;
static PangoFontDescription *font_bold = NULL;

void
draw_init(void)
{
    char spec[128];
    snprintf(spec, sizeof spec, "%s 10", FONT_FAMILY);
    font_regular = pango_font_description_from_string(spec);

    snprintf(spec, sizeof spec, "%s Bold 10", FONT_FAMILY);
    font_bold = pango_font_description_from_string(spec);
}

void
draw_cleanup(void)
{
    if (font_regular) pango_font_description_free(font_regular);
    if (font_bold)    pango_font_description_free(font_bold);
    font_regular = font_bold = NULL;
}

void
ctx_bind(Ctx *c, Window w, int width, int height)
{
    if (c->cr)   cairo_destroy(c->cr);
    if (c->surf) cairo_surface_destroy(c->surf);

    Visual *vis   = argb_visual ? argb_visual : DefaultVisual(dpy, screen);
    int     depth = argb_visual ? argb_depth  : DefaultDepth(dpy, screen);

    c->win  = w;
    c->w    = width;
    c->h    = height;
    c->surf = cairo_xlib_surface_create(dpy, w, vis, width, height);
    (void)depth;
    c->cr   = cairo_create(c->surf);
    cairo_set_antialias(c->cr, CAIRO_ANTIALIAS_GOOD);
}

void
ctx_free(Ctx *c)
{
    if (c->cr)   cairo_destroy(c->cr);
    if (c->surf) cairo_surface_destroy(c->surf);
    c->cr = NULL;
    c->surf = NULL;
}

void
ctx_flush(Ctx *c)
{
    cairo_surface_flush(c->surf);
    XFlush(dpy);
}

void
draw_set_source(cairo_t *cr, unsigned int argb)
{
    double a = ((argb >> 24) & 0xFF) / 255.0;
    double r = ((argb >> 16) & 0xFF) / 255.0;
    double g = ((argb >> 8)  & 0xFF) / 255.0;
    double b = ( argb        & 0xFF) / 255.0;
    cairo_set_source_rgba(cr, r, g, b, a);
}

void
draw_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double radius)
{
    if (radius <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    double r = radius;
    if (r > w / 2.0) r = w / 2.0;
    if (r > h / 2.0) r = h / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r,     r, -M_PI_2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI_2);
    cairo_arc(cr, x + r,     y + h - r, r, M_PI_2, M_PI);
    cairo_arc(cr, x + r,     y + r,     r, M_PI, 3 * M_PI_2);
    cairo_close_path(cr);
}

void
draw_panel(Ctx *c, unsigned int bg_argb, unsigned int border_argb, double radius)
{
    cairo_save(c->cr);
    cairo_set_operator(c->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(c->cr, 0, 0, 0, 0);
    cairo_paint(c->cr);
    cairo_restore(c->cr);

    cairo_set_operator(c->cr, CAIRO_OPERATOR_OVER);

    draw_rounded_rect(c->cr, 0.5, 0.5, c->w - 1.0, c->h - 1.0, radius);
    draw_set_source(c->cr, bg_argb);
    cairo_fill_preserve(c->cr);

    if (((border_argb >> 24) & 0xFF) != 0) {
        draw_set_source(c->cr, border_argb);
        cairo_set_line_width(c->cr, 1.0);
        cairo_stroke(c->cr);
    } else {
        cairo_new_path(c->cr);
    }
}

static PangoLayout *
make_layout(cairo_t *cr, const char *utf8, double size, int bold)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_copy(bold ? font_bold : font_regular);
    pango_font_description_set_absolute_size(desc, size * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_layout_set_text(layout, utf8, -1);
    pango_font_description_free(desc);
    return layout;
}

void
measure_text(const char *utf8, double font_size, int bold, int *w, int *h)
{
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    cairo_t *cr = cairo_create(tmp);
    PangoLayout *layout = make_layout(cr, utf8, font_size, bold);
    pango_layout_get_pixel_size(layout, w, h);
    g_object_unref(layout);
    cairo_destroy(cr);
    cairo_surface_destroy(tmp);
}

int
draw_text(Ctx *c, double x, const char *utf8, unsigned int argb, double font_size, int bold)
{
    PangoLayout *layout = make_layout(c->cr, utf8, font_size, bold);
    int tw, th;
    pango_layout_get_pixel_size(layout, &tw, &th);

    double y = (c->h - th) / 2.0;
    cairo_save(c->cr);
    draw_set_source(c->cr, argb);
    cairo_move_to(c->cr, x, y);
    pango_cairo_show_layout(c->cr, layout);
    cairo_restore(c->cr);

    g_object_unref(layout);
    return tw;
}

int
draw_text_centered(Ctx *c, double x, double w, const char *utf8, unsigned int argb, double font_size, int bold)
{
    int tw, th;
    measure_text(utf8, font_size, bold, &tw, &th);
    double tx = x + (w - tw) / 2.0;
    if (tx < x) tx = x;
    return draw_text(c, tx, utf8, argb, font_size, bold);
}

void
draw_battery_glyph(Ctx *c, double x, double y, double w, double h, double pct, int charging)
{
    if (pct < 0) return; /* no battery on this machine */
    double nub_w = 2.0, nub_h = h * 0.4;

    cairo_save(c->cr);
    draw_rounded_rect(c->cr, x, y, w - nub_w, h, 2.0);
    draw_set_source(c->cr, COL_TEXT_MUTED);
    cairo_set_line_width(c->cr, 1.2);
    cairo_stroke(c->cr);

    cairo_rectangle(c->cr, x + w - nub_w, y + (h - nub_h) / 2.0, nub_w, nub_h);
    draw_set_source(c->cr, COL_TEXT_MUTED);
    cairo_fill(c->cr);

    double pad = 2.0;
    double fill_w = (w - nub_w - pad * 2.0) * (pct / 100.0);
    if (fill_w < 0) fill_w = 0;
    unsigned int fill_col = charging ? COL_DOT_MIN
                           : (pct <= 20.0 ? COL_DOT_CLOSE : COL_TEXT_PRIMARY);
    draw_rounded_rect(c->cr, x + pad, y + pad, fill_w, h - pad * 2.0, 1.0);
    draw_set_source(c->cr, fill_col);
    cairo_fill(c->cr);
    cairo_restore(c->cr);
}

/* ---- procedural dock glyphs -------------------------------------
 * Everything below is stroked/filled with cairo primitives; there
 * are no bitmap/svg assets anywhere in this codebase. Each glyph is
 * drawn inside a `size`x`size` box centered at (cx, cy).
 * ------------------------------------------------------------------ */

static void
glyph_terminal(cairo_t *cr, double x, double y, double s, unsigned int argb)
{
    double lw = s * 0.09;
    cairo_save(cr);
    cairo_set_line_width(cr, lw);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    draw_set_source(cr, argb);

    /* screen outline */
    cairo_new_sub_path(cr);
    cairo_rectangle(cr, x + lw / 2, y + lw / 2, s - lw, s - lw);
    cairo_stroke(cr);

    /* chevron ">" */
    double px = x + s * 0.22, py = y + s * 0.32;
    cairo_move_to(cr, px, py);
    cairo_line_to(cr, px + s * 0.20, y + s * 0.5);
    cairo_line_to(cr, px, y + s * 0.68);
    cairo_stroke(cr);

    /* cursor "_" */
    cairo_move_to(cr, x + s * 0.5, y + s * 0.68);
    cairo_line_to(cr, x + s * 0.76, y + s * 0.68);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void
glyph_folder(cairo_t *cr, double x, double y, double s, unsigned int argb)
{
    cairo_save(cr);
    draw_set_source(cr, argb);
    double bw = s * 0.82, bh = s * 0.58;
    double bx = x + (s - bw) / 2.0, by = y + s * 0.34;
    /* tab */
    draw_rounded_rect(cr, bx, by - s * 0.10, bw * 0.42, s * 0.16, s * 0.03);
    cairo_fill(cr);
    /* body */
    draw_rounded_rect(cr, bx, by, bw, bh, s * 0.05);
    cairo_fill(cr);
    cairo_restore(cr);
}

static void
glyph_globe(cairo_t *cr, double x, double y, double s, unsigned int argb)
{
    double lw = s * 0.07, r = s * 0.38;
    double cx = x + s / 2.0, cy = y + s / 2.0;
    cairo_save(cr);
    cairo_set_line_width(cr, lw);
    draw_set_source(cr, argb);

    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_move_to(cr, cx - r, cy);
    cairo_line_to(cr, cx + r, cy);
    cairo_stroke(cr);

    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, 0.42, 1.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, 0, 0, r, 0, 2 * M_PI);
    cairo_restore(cr);
    cairo_set_line_width(cr, lw);
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void
glyph_editor(cairo_t *cr, double x, double y, double s, unsigned int argb)
{
    cairo_save(cr);
    draw_set_source(cr, argb);
    double pw = s * 0.62, ph = s * 0.78;
    double px = x + s * 0.14, py = y + (s - ph) / 2.0;
    cairo_set_line_width(cr, s * 0.07);
    draw_rounded_rect(cr, px, py, pw, ph, s * 0.06);
    cairo_stroke(cr);

    cairo_set_line_width(cr, s * 0.055);
    for (int i = 0; i < 3; i++) {
        double ly = py + ph * (0.32 + i * 0.2);
        cairo_move_to(cr, px + pw * 0.18, ly);
        cairo_line_to(cr, px + pw * 0.82, ly);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static void
glyph_gear(cairo_t *cr, double x, double y, double s, unsigned int argb)
{
    double cx = x + s / 2.0, cy = y + s / 2.0;
    double r_out = s * 0.40, r_in = s * 0.24, r_hole = s * 0.13;
    int teeth = 8;
    cairo_save(cr);
    draw_set_source(cr, argb);

    cairo_new_path(cr);
    for (int i = 0; i < teeth * 2; i++) {
        double ang = (M_PI * 2.0) * i / (teeth * 2);
        double r = (i % 2 == 0) ? r_out : r_in;
        double px = cx + r * cos(ang), py = cy + r * sin(ang);
        if (i == 0) cairo_move_to(cr, px, py);
        else        cairo_line_to(cr, px, py);
    }
    cairo_close_path(cr);
    cairo_fill(cr);

    /* center hole, punched out */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_new_sub_path(cr);
    cairo_arc(cr, cx, cy, r_hole, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_restore(cr);
}

void
draw_dock_glyph(cairo_t *cr, int glyph, double cx, double cy, double size, unsigned int argb)
{
    double x = cx - size / 2.0, y = cy - size / 2.0;
    switch (glyph) {
        case 0: glyph_terminal(cr, x, y, size, argb); break; /* DOCK_GLYPH_TERMINAL */
        case 1: glyph_folder(cr, x, y, size, argb);   break; /* DOCK_GLYPH_FILES    */
        case 2: glyph_globe(cr, x, y, size, argb);    break; /* DOCK_GLYPH_BROWSER  */
        case 3: glyph_editor(cr, x, y, size, argb);   break; /* DOCK_GLYPH_EDITOR   */
        case 4: glyph_gear(cr, x, y, size, argb);     break; /* DOCK_GLYPH_SETTINGS */
        default: break;
    }
}