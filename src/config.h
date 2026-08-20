/* VoidOS window manager -- user configuration
 *
 * This is the one file you're meant to edit. Change colors, metrics,
 * keybindings, or dock apps here, then `make` again.
 */
#ifndef VOIDWM_CONFIG_H
#define VOIDWM_CONFIG_H

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include "voidwm.h"

/* ---------------------------------------------------------------- *
 * Modifier key -- Mod4Mask is the Super/Windows key.
 * ---------------------------------------------------------------- */
#define MODKEY Mod4Mask

/* ---------------------------------------------------------------- *
 * Layout metrics (pixels)
 * ---------------------------------------------------------------- */
#define BAR_HEIGHT          26
#define DOCK_HEIGHT         60
#define DOCK_BOTTOM_MARGIN  10
#define DOCK_ICON_SIZE      42
#define DOCK_ICON_GAP       16
#define DOCK_H_PADDING      14
#define DOCK_RADIUS         16.0
#define BAR_RADIUS          0.0      /* full-width bar, square corners */

#define TITLEBAR_HEIGHT     26
#define BORDER_WIDTH        1
#define GAP                 10        /* cascade offset for new windows */

/* Rounded window corners, applied via the X Shape extension to each
 * client's frame (border + titlebar + content, cut together). 0
 * disables rounding. Automatically squared off while maximized/
 * fullscreen. Requires the server to support the Shape extension
 * (virtually all X servers do); silently skipped otherwise. */
#define WINDOW_RADIUS       12.0

#define MIN_WIN_W           160
#define MIN_WIN_H           100

/* Auto-hide dock: how close (px) the pointer must get to the bottom
 * edge to reveal it, and how long (ms) it stays up once revealed. */
#define DOCK_HOTZONE_PX     8
#define DOCK_LINGER_MS      1400
#define POLL_INTERVAL_MS    120

/* ---------------------------------------------------------------- *
 * Fonts -- bundled Ubuntu Sans is loaded at runtime via fontconfig,
 * see draw.c:draw_init(). No system font install required.
 * ---------------------------------------------------------------- */
#define FONT_FAMILY         "Ubuntu Sans"
#define FONT_SIZE_BAR        12.5
#define FONT_SIZE_DOCK        10.0
#define FONT_SIZE_TITLEBAR    11.5

/* ---------------------------------------------------------------- *
 * Palette -- "nightfall" theme: dark indigo/violet glass, styled
 * after BazziteOS's GNOME desktop (0xAARRGGBB). Set USE_ARGB_VISUAL
 * to 1 to let a compositor (picom) render true frosted-glass blur/
 * transparency on the bar + dock, matching the reference. Works
 * fine with it at 0 too -- just flat panels instead of glass, no
 * compositor required.
 * ---------------------------------------------------------------- */
#define USE_ARGB_VISUAL      1

#define COL_ROOT_BG          0xFF0C0A14u
#define COL_BAR_BG           0xCC17131Fu
#define COL_BAR_BORDER       0x40A78BFAu
#define COL_DOCK_BG          0xCC1B1626u
#define COL_DOCK_BORDER      0x40A78BFAu
#define COL_DOCK_ICON_HOVER  0x338B7CF6u

#define COL_TEXT_PRIMARY     0xFFF3F1FAu
#define COL_TEXT_MUTED       0xC8D6D0F0u
#define COL_TEXT_FAINT       0x80B3A9D9u

#define COL_ACCENT           0xFF9D7BF6u
#define COL_ACCENT_HOVER     0xFFBBA0FBu

#define COL_TITLEBAR_FOCUS   0xFF241D38u
#define COL_TITLEBAR_UNFOCUS 0xFF17131Fu
#define COL_BORDER_FOCUS     0xFF9D7BF6u
#define COL_BORDER_UNFOCUS   0xFF2B2440u
#define COL_CLIENT_BG        0xFF0F0C18u  /* shown briefly before client paints */

#define COL_DOT_CLOSE        0xFFFF5F57u
#define COL_DOT_MAX          0xFFFEBC2Eu
#define COL_DOT_MIN          0xFF5AD16Bu

/* ---------------------------------------------------------------- *
 * Workspaces
 * ---------------------------------------------------------------- */
#define NUM_WORKSPACES 5

/* ---------------------------------------------------------------- *
 * Dock applications.
 * `cmd` is run through `/bin/sh -c`, so shell fallbacks with `||`
 * work. `glyph` selects a procedurally-drawn vector icon in dock.c
 * (no bitmap assets) -- see DOCK_GLYPH_* below.
 * ---------------------------------------------------------------- */
#define DOCK_GLYPH_TERMINAL  0
#define DOCK_GLYPH_FILES     1
#define DOCK_GLYPH_BROWSER   2
#define DOCK_GLYPH_EDITOR    3
#define DOCK_GLYPH_SETTINGS  4

typedef struct {
    const char *label;
    const char *cmd;
    int glyph;
} DockApp;

static const DockApp dockapps[] = {
    { "Terminal", "x-terminal-emulator || xterm",                                   DOCK_GLYPH_TERMINAL },
    { "Files",    "xdg-open \"$HOME\" || pcmanfm || nautilus || thunar",             DOCK_GLYPH_FILES    },
    { "Browser",  "$BROWSER || xdg-open https:// || firefox || chromium",           DOCK_GLYPH_BROWSER  },
    { "VoidDocs", "voiddocs || x-terminal-emulator -e nano || xterm -e vi",          DOCK_GLYPH_EDITOR   },
    { "Settings", "voidwm-settings || x-terminal-emulator -e nmtui || xterm",       DOCK_GLYPH_SETTINGS },
};
#define NUM_DOCKAPPS ((int)(sizeof(dockapps) / sizeof(dockapps[0])))

/* ---------------------------------------------------------------- *
 * Terminal used by Mod+Return
 * ---------------------------------------------------------------- */
#define SPAWN_TERMINAL "x-terminal-emulator || xterm"

/* ---------------------------------------------------------------- *
 * Keybindings -- MODKEY is Super. Add/remove rows freely; `arg` is
 * only meaningful for act_view_workspace / act_move_focused_to_workspace
 * (0-based workspace index) and is ignored otherwise.
 * ---------------------------------------------------------------- */
typedef void (*KeyFunc)(int arg);

typedef struct {
    unsigned int mod;
    KeySym       keysym;
    KeyFunc      func;
    int          arg;
} Key;

static const Key keys[] = {
    /* modifier              key         action                          arg */
    { MODKEY,                XK_Return,  act_spawn_terminal,              0 },
    { MODKEY|ShiftMask,      XK_q,       act_kill_focused,                0 },
    { MODKEY|ShiftMask,      XK_e,       act_quit,                        0 },
    { MODKEY,                XK_Tab,     act_focus_cycle,                 0 },
    { MODKEY,                XK_f,       act_toggle_fullscreen,           0 },
    { MODKEY,                XK_1,       act_view_workspace,              0 },
    { MODKEY,                XK_2,       act_view_workspace,              1 },
    { MODKEY,                XK_3,       act_view_workspace,              2 },
    { MODKEY,                XK_4,       act_view_workspace,              3 },
    { MODKEY,                XK_5,       act_view_workspace,              4 },
    { MODKEY|ShiftMask,      XK_1,       act_move_focused_to_workspace,   0 },
    { MODKEY|ShiftMask,      XK_2,       act_move_focused_to_workspace,   1 },
    { MODKEY|ShiftMask,      XK_3,       act_move_focused_to_workspace,   2 },
    { MODKEY|ShiftMask,      XK_4,       act_move_focused_to_workspace,   3 },
    { MODKEY|ShiftMask,      XK_5,       act_move_focused_to_workspace,   4 },
};
#define NUM_KEYS ((int)(sizeof(keys) / sizeof(keys[0])))

#endif /* VOIDWM_CONFIG_H */