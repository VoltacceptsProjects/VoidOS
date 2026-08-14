#!/usr/bin/env python3
"""
VoidDock -- a from-scratch macOS-style magnifying dock for VoidOS.

Behavior is ported from a reference HTML/CSS/JS macOS-dock demo (continuous
hover magnification, glass pill container, per-icon name tooltip, running-app
indicator dot, separator + trash), but every asset is VoidOS's own: icons
come from the system icon theme (Papirus), not Apple's Finder/Siri/FaceTime
artwork, and there's no Apple logo or San Francisco font anywhere. This is a
real GTK3 application drawn with Cairo -- not an embedded web page -- so it
integrates with actual running windows via libwnck (click to focus/minimize,
a dot under running apps) the way a native desktop dock has to.

Magnification here is continuous (Gaussian falloff by pixel distance from the
cursor) rather than the reference's discrete +/-2-neighbor steps, which reads
closer to real macOS.
"""
import gi
gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Wnck", "3.0")
from gi.repository import Gtk, Gdk, GLib, GdkPixbuf, Wnck
import cairo
import math
import subprocess
import time

# --- Look & feel -------------------------------------------------------
BASE_SIZE = 48          # icon size at rest
MAX_SIZE = 76           # icon size directly under the cursor
MAGNIFY_SPREAD = 90.0   # px: how far the magnification falloff reaches
LIFT = 22               # px: how far a fully-magnified icon rises
PADDING = 14            # pill padding around the icon row
GAP = 10                # base gap between icon centers beyond their size
BOTTOM_MARGIN = 12
CORNER_RADIUS = 20
GLASS_RGBA = (28 / 255, 18 / 255, 16 / 255, 0.55)   # matches VoidGlass @glass_bg
BORDER_RGBA = (255 / 255, 200 / 255, 170 / 255, 0.16)
ACCENT_RGBA = (255 / 255, 106 / 255, 61 / 255, 1.0)  # VoidGlass @accent
TEXT_RGBA = (1, 1, 1, 0.95)
TOOLTIP_BG = (0, 0, 0, 0.55)
BOUNCE_MS = 420

# --- Pinned apps: (icon name, exec command, label, match substrings) ---
# Icon names resolve through the active GTK icon theme (Papirus-Dark on
# VoidOS) -- no bundled artwork required. `match` is used to associate a
# running Wnck window with a pinned slot via its WM class, best-effort.
PINNED_APPS = [
    ("system-file-manager", "thunar", "Files", ["thunar"]),
    ("firefox-esr", "firefox-esr", "Web", ["firefox"]),
    ("utilities-terminal", "xfce4-terminal", "Terminal", ["xfce4-terminal"]),
    ("accessories-text-editor", "mousepad", "Notes", ["mousepad"]),
    ("multimedia-photo-viewer", "ristretto", "Photos", ["ristretto"]),
    ("multimedia-player", "parole", "Music", ["parole"]),
    ("view-app-grid", "xfce4-appfinder", "Launchpad", ["xfce4-appfinder"]),
    ("preferences-system", "xfce4-settings-manager", "Settings", ["xfce4-settings-manager"]),
]
TRASH_ICON = "user-trash"
TRASH_LABEL = "Bin"


class DockIcon:
    """Runtime state for one dock slot (pinned app, extra running app, or trash)."""

    def __init__(self, icon_name, exec_cmd, label, match=None, is_trash=False):
        self.icon_name = icon_name
        self.exec_cmd = exec_cmd
        self.label = label
        self.match = match or []
        self.is_trash = is_trash
        self.pixbuf_cache = {}
        self.wnck_window = None       # set per-frame if a matching window is running
        self.scale = 1.0              # current animated scale (eased toward target)
        self.bounce_start = None      # time.monotonic() when a launch bounce began
        self.center_x = 0.0           # updated each layout pass

    def pixbuf(self, size):
        pb = self.pixbuf_cache.get(size)
        if pb is None:
            theme = Gtk.IconTheme.get_default()
            try:
                pb = theme.load_icon(self.icon_name, size, Gtk.IconLookupFlags.FORCE_SIZE)
            except GLib.Error:
                pb = theme.load_icon("application-x-executable", size, Gtk.IconLookupFlags.FORCE_SIZE)
            self.pixbuf_cache[size] = pb
        return pb


class VoidDock(Gtk.Window):
    def __init__(self):
        super().__init__(type=Gtk.WindowType.TOPLEVEL)
        GLib.set_prgname("voiddock")
        self.set_title("VoidDock")
        self.set_decorated(False)
        self.set_resizable(False)
        self.set_skip_taskbar_hint(True)
        self.set_skip_pager_hint(True)
        self.set_type_hint(Gdk.WindowTypeHint.DOCK)
        self.set_accept_focus(False)
        self.set_keep_above(True)
        self.stick()

        screen = self.get_screen()
        visual = screen.get_rgba_visual()
        if visual and screen.is_composited():
            self.set_visual(visual)
        self.set_app_paintable(True)

        self.icons = [DockIcon(*a) for a in PINNED_APPS]
        self.icons.append(DockIcon(TRASH_ICON, None, TRASH_LABEL, is_trash=True))

        self.hover_x = None
        self.hovered = None

        self.area = Gtk.DrawingArea()
        self.area.set_events(
            Gdk.EventMask.POINTER_MOTION_MASK
            | Gdk.EventMask.LEAVE_NOTIFY_MASK
            | Gdk.EventMask.BUTTON_PRESS_MASK
        )
        self.area.connect("draw", self.on_draw)
        self.area.connect("motion-notify-event", self.on_motion)
        self.area.connect("leave-notify-event", self.on_leave)
        self.area.connect("button-press-event", self.on_click)
        self.add(self.area)

        self.resize_to_content()
        self.reposition()

        screen.connect("size-changed", lambda *_: self.reposition())
        screen.connect("monitors-changed", lambda *_: self.reposition())

        self.wnck_screen = Wnck.Screen.get_default()
        self.wnck_screen.connect("window-opened", lambda *_: self.refresh_running())
        self.wnck_screen.connect("window-closed", lambda *_: self.refresh_running())
        self.refresh_running()

        # Animation clock: eases icon scale toward its hover target and
        # drives the launch-bounce, redrawing at ~60fps only while needed.
        GLib.timeout_add(16, self.tick)

    # -- layout -----------------------------------------------------------
    def content_width(self):
        return int(len(self.icons) * (BASE_SIZE + GAP) + PADDING * 2 + 20)

    def content_height(self):
        return int(MAX_SIZE + LIFT + PADDING * 2 + 24)

    def resize_to_content(self):
        self.set_size_request(self.content_width(), self.content_height())

    def reposition(self):
        screen = self.get_screen()
        monitor = screen.get_display().get_primary_monitor() or screen.get_display().get_monitor(0)
        geo = monitor.get_geometry()
        w, h = self.content_width(), self.content_height()
        self.move(geo.x + (geo.width - w) // 2, geo.y + geo.height - h - BOTTOM_MARGIN)

    # -- running-window tracking -------------------------------------------
    def refresh_running(self):
        windows = [
            w for w in self.wnck_screen.get_windows()
            if w.get_window_type() == Wnck.WindowType.NORMAL
        ]
        for icon in self.icons:
            icon.wnck_window = None
            if icon.is_trash or not icon.match:
                continue
            for w in windows:
                cls = (w.get_class_group_name() or "").lower()
                if any(m in cls for m in icon.match):
                    icon.wnck_window = w
                    break
        self.area.queue_draw()

    # -- interaction --------------------------------------------------------
    def on_motion(self, _widget, event):
        self.hover_x = event.x
        self.area.queue_draw()
        return True

    def on_leave(self, _widget, _event):
        self.hover_x = None
        self.area.queue_draw()
        return True

    def on_click(self, _widget, event):
        for icon in self.icons:
            half = BASE_SIZE * max(icon.scale, 1.0) / 2
            if icon.center_x - half <= event.x <= icon.center_x + half:
                self.activate(icon)
                break
        return True

    def activate(self, icon):
        icon.bounce_start = time.monotonic()
        if icon.is_trash:
            subprocess.Popen(["exo-open", "--launch", "FileManager", "trash:///"])
            return
        if icon.wnck_window is not None:
            w = icon.wnck_window
            if w.is_active():
                w.minimize()
            else:
                w.unminimize(0)
                w.activate(0)
            return
        subprocess.Popen(icon.exec_cmd.split())

    # -- animation ------------------------------------------------------
    def target_scale(self, icon):
        if self.hover_x is None:
            return 1.0
        dx = self.hover_x - icon.center_x
        falloff = math.exp(-(dx * dx) / (2 * MAGNIFY_SPREAD * MAGNIFY_SPREAD))
        return 1.0 + (MAX_SIZE / BASE_SIZE - 1.0) * falloff

    def tick(self):
        moving = False
        for icon in self.icons:
            target = self.target_scale(icon)
            if abs(icon.scale - target) > 0.002:
                icon.scale += (target - icon.scale) * 0.35
                moving = True
            else:
                icon.scale = target
            if icon.bounce_start is not None:
                moving = True
                if (time.monotonic() - icon.bounce_start) * 1000 > BOUNCE_MS:
                    icon.bounce_start = None
        if moving or self.hover_x is not None:
            self.area.queue_draw()
        return True

    def bounce_offset(self, icon):
        if icon.bounce_start is None:
            return 0.0
        t = (time.monotonic() - icon.bounce_start) * 1000 / BOUNCE_MS
        if t >= 1.0:
            return 0.0
        return math.sin(t * math.pi) * 16.0 * (1 - t)

    # -- drawing --------------------------------------------------------
    def rounded_rect(self, cr, x, y, w, h, r):
        cr.new_sub_path()
        cr.arc(x + w - r, y + r, r, -math.pi / 2, 0)
        cr.arc(x + w - r, y + h - r, r, 0, math.pi / 2)
        cr.arc(x + r, y + h - r, r, math.pi / 2, math.pi)
        cr.arc(x + r, y + r, r, math.pi, 3 * math.pi / 2)
        cr.close_path()

    def on_draw(self, widget, cr):
        alloc = widget.get_allocation()
        w, h = alloc.width, alloc.height

        # layout pass: assign each icon's resting center_x first (equal
        # spacing at base size), magnification only affects draw size/lift.
        n = len(self.icons)
        total = n * (BASE_SIZE + GAP) - GAP
        start_x = (w - total) / 2 + BASE_SIZE / 2
        for i, icon in enumerate(self.icons):
            icon.center_x = start_x + i * (BASE_SIZE + GAP)

        pill_y = h - BASE_SIZE - PADDING * 2 - 4
        pill_h = BASE_SIZE + PADDING * 2
        pill_x = min(i.center_x for i in self.icons) - BASE_SIZE / 2 - PADDING
        pill_w = (max(i.center_x for i in self.icons) - min(i.center_x for i in self.icons)
                  + BASE_SIZE + PADDING * 2)

        # glass pill (real blur comes from picom behind this ARGB window)
        cr.save()
        self.rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, CORNER_RADIUS)
        cr.set_source_rgba(*GLASS_RGBA)
        cr.fill_preserve()
        cr.set_source_rgba(*BORDER_RGBA)
        cr.set_line_width(1)
        cr.stroke()
        cr.restore()

        baseline = pill_y + pill_h - PADDING

        for icon in self.icons:
            if icon.is_trash:
                sep_x = icon.center_x - (BASE_SIZE + GAP) / 2
                cr.save()
                cr.set_source_rgba(1, 1, 1, 0.25)
                cr.set_line_width(1)
                cr.move_to(sep_x, pill_y + 8)
                cr.line_to(sep_x, pill_y + pill_h - 8)
                cr.stroke()
                cr.restore()

            size = BASE_SIZE * icon.scale
            lift = LIFT * (icon.scale - 1.0) / (MAX_SIZE / BASE_SIZE - 1.0) if MAX_SIZE != BASE_SIZE else 0
            lift += self.bounce_offset(icon)
            pb = icon.pixbuf(min(int(size), 128) or 1)

            cr.save()
            cx = icon.center_x
            cy = baseline - size / 2 - lift
            cr.translate(cx - pb.get_width() / 2, cy - pb.get_height() / 2)
            Gdk.cairo_set_source_pixbuf(cr, pb, 0, 0)
            cr.paint()
            cr.restore()

            if icon.wnck_window is not None:
                cr.save()
                cr.arc(icon.center_x, baseline + 6, 2.2, 0, 2 * math.pi)
                cr.set_source_rgba(*ACCENT_RGBA)
                cr.fill()
                cr.restore()

            if self.hover_x is not None and abs(self.hover_x - icon.center_x) < BASE_SIZE / 2 and icon.scale > 1.05:
                self.draw_tooltip(cr, icon, cy - size / 2 - 14)

        return False

    def draw_tooltip(self, cr, icon, y):
        cr.save()
        cr.select_font_face("sans-serif", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        cr.set_font_size(12)
        extents = cr.text_extents(icon.label)
        pad_x, pad_y = 10, 5
        box_w = extents.width + pad_x * 2
        box_h = extents.height + pad_y * 2
        x = icon.center_x - box_w / 2
        top = y - box_h - 8

        self.rounded_rect(cr, x, top, box_w, box_h, 5)
        cr.set_source_rgba(*TOOLTIP_BG)
        cr.fill()

        cr.move_to(icon.center_x - 5, top + box_h)
        cr.line_to(icon.center_x + 5, top + box_h)
        cr.line_to(icon.center_x, top + box_h + 6)
        cr.close_path()
        cr.set_source_rgba(*TOOLTIP_BG)
        cr.fill()

        cr.set_source_rgba(*TEXT_RGBA)
        cr.move_to(x + pad_x, top + pad_y + extents.height)
        cr.show_text(icon.label)
        cr.restore()


def main():
    win = VoidDock()
    win.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
