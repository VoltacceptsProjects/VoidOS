# VoidOS Desktop

A small, real X11 desktop environment: a floating/reparenting window
manager (`voidwm`) with a top status bar, an auto-hiding bottom dock,
per-window titlebars with traffic-light controls, and 5 virtual
workspaces. No bitmap icons anywhere — the dock buttons and battery
indicator are all drawn procedurally with cairo. Styling matches the
VoidOS "ember" web portfolio theme (dark, deep red/orange, glass
panels when a compositor is running).

This is a real Xorg window manager, written in C against Xlib +
cairo + pango. It runs on actual hardware, not just in a browser or a
mockup — install it, log into an X session, and it manages real
application windows.

**Wayland:** not included here, and it's worth being upfront about
why. A Wayland session isn't "the same WM in a different mode" — it's
a full compositor that replaces Xorg entirely (input handling, output
management, buffer compositing, the works), typically built on
wlroots. That's a separate, much larger codebase from this X11 WM,
not a config flag. This package is X11/Xorg only. If Wayland matters
to you, `voidwm` still works fine under XWayland-less setups or as
your Xorg session while you wait on a wlroots port; it is not a
first step toward one.

## What's in this package

```
src/            voidwm.c, draw.c, bar.c, dock.c + headers
apps/voiddocs/  VoidDocs, the dock's markdown editor (.vdoc files) -- see below
Makefile        builds voidwm, installs the session
voidwm-session  session launcher (wallpaper, optional picom, exec voidwm)
voidwm.desktop  xsession entry so display managers (LightDM/GDM/SDDM) list it
wallpapers/     4 wallpaper choices from the VoidOS theme (dark/light/red/solacium)
examples/xinitrc  drop-in ~/.xinitrc if you use startx instead of a DM
```

## Build & install (real hardware, any systemd-less or systemd distro)

Install dependencies first:

```sh
# Debian / Ubuntu
sudo apt install build-essential pkg-config libx11-dev libxext-dev \
                  libcairo2-dev libpango1.0-dev

# Fedora
sudo dnf install gcc make pkgconf-pkg-config libX11-devel libXext-devel \
                  cairo-devel pango-devel

# Arch
sudo pacman -S base-devel pkgconf libx11 libxext cairo pango
```

Then:

```sh
make
sudo make install
```

This installs:
- `voidwm` and `voidwm-session` to `/usr/local/bin`
- `voidwm.desktop` to `/usr/share/xsessions` (so it shows up on your
  login screen's session picker, next to GNOME/KDE/etc.)
- the wallpapers to `/usr/local/share/voidos/wallpapers`

Log out, pick **VoidOS** from your display manager's session menu,
and log in.

### No display manager? (startx)

```sh
cp examples/xinitrc ~/.xinitrc
chmod +x ~/.xinitrc
startx
```

### Optional extras

None of these are required — `voidwm` runs standalone — but
`voidwm-session` will pick them up automatically if installed:

- **feh / nitrogen / xwallpaper** — sets the desktop wallpaper. Without
  one, the root window just stays a flat color (already matches the
  theme — see `COL_ROOT_BG` in `src/config.h`).
- **picom** — a real X compositor, needed for actual frosted-glass
  translucency/blur on the bar and dock (`USE_ARGB_VISUAL` in
  `config.h` sets up the ARGB visual `voidwm` needs; picom does the
  actual blur). Without it you still get the panels, just flat
  instead of glassy.
- **dunst** — notification daemon, launched if present.
- **a terminal emulator** (`x-terminal-emulator` or `xterm`) — the
  dock's Terminal button and `Mod+Return` need one on `$PATH`.

## VoidDocs (the dock's markdown editor)

The dock's **VoidDocs** button opens `voiddocs`, a small GTK3 editor
for VoidOS's native document type, `.vdoc` — plain markdown text
under the hood, with a live syntax-highlighted editor pane and a
toggleable rendered preview pane, styled to match the desktop theme.
It's a separate app from `voidwm` (needs GTK3, not just X11/cairo),
so it's not built by the top-level `make`/`make install`:

```sh
# Debian / Ubuntu
sudo apt install libgtk-3-dev

# Fedora
sudo dnf install gtk3-devel

# Arch
sudo pacman -S gtk3

make voiddocs
sudo make install-voiddocs
```

This installs `voiddocs` to `/usr/local/bin`, its `.desktop` entry
(so file managers list it as a launcher/handler), and a MIME-type
registration so `.vdoc` files are associated with it. If `voiddocs`
isn't installed, the dock button falls back to opening `nano`/`vi`
in a terminal instead, same as every other dock entry's fallback
chain.

Shortcuts inside VoidDocs: `Ctrl+N` new, `Ctrl+O` open, `Ctrl+S`
save, `Ctrl+Shift+S` save as, `Ctrl+P`/`Ctrl+E` toggle preview.

## Using it

| Action | Binding |
|---|---|
| Open terminal | `Super+Return` |
| Close focused window | `Super+Shift+Q` |
| Cycle focus | `Super+Tab` |
| Toggle maximize/fullscreen | `Super+F` |
| Switch to workspace 1–5 | `Super+1` … `Super+5` |
| Move focused window to workspace 1–5 | `Super+Shift+1` … `Super+Shift+5` |
| Quit voidwm | `Super+Shift+E` |
| Move any window | `Super` + drag with left click |
| Resize any window | `Super` + drag with right click |
| Titlebar dots (left→right) | close / minimize / maximize |

Click the workspace dots on the left of the bar to switch workspaces.
Move your pointer to the bottom edge of the screen to reveal the
dock; it auto-hides after ~1.4s once you move away.

All of this — colors, metrics, keybindings, dock apps, workspace
count — is one file: `src/config.h`. Edit it, `make`, restart the
session.

Windows get rounded corners by default (`WINDOW_RADIUS` in
`config.h`, applied via the X Shape extension to each window's
frame — titlebar, border and content all get cut together, and it
squares off automatically while a window is maximized/fullscreen).
Set `WINDOW_RADIUS` to `0` for classic square corners.

## Notes on what changed getting this to build clean on real hardware

The window manager logic (workspaces, framing, drag-move/resize,
focus, ICCCM delete protocol) was already complete. Getting it to
actually compile and run against a live X server surfaced a few real
bugs, now fixed:

- `on_unmap_notify` was typed as the nonexistent `XUnmapNotifyEvent`
  (Xlib's actual struct is `XUnmapEvent`) — the whole build failed on
  this before anything else could be tested.
- `dock.c` referenced `LeaveNotifyMask`, which isn't a real X11 event
  mask constant; the correct one is `LeaveWindowMask`.
- `<math.h>`'s `M_PI`/`M_PI_2` aren't exposed under strict `-std=c11`
  on glibc without `_DEFAULT_SOURCE`; added to `CFLAGS`.
- The battery percentage buffer in `bar.c` was undersized for the
  format string per the compiler's static range analysis; widened and
  clamped 0–100 defensively.
- The Makefile expected sources under `src/`, but they were flat —
  fixed here by actually laying out the tree that way.
- `voidwm-session`, `voidwm.desktop`, and the wallpaper install step
  didn't exist yet even though the Makefile's `install` target already
  referenced them — added.

After these fixes it was built and run against a live X server
(framing a real `xterm`, correct bar/dock geometry, no crashes) before
being packaged here.
