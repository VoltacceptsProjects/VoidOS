/* VoidOS window manager -- shared internal declarations */
#ifndef VOIDWM_H
#define VOIDWM_H

#include <X11/Xlib.h>

/* ---- globals owned by voidwm.c, used by draw.c/bar.c/dock.c ---- */
extern Display *dpy;
extern int      screen;
extern Window   root;
extern int      scr_w, scr_h;
extern Visual  *argb_visual;   /* 32-bit TrueColor visual, or NULL if unavailable */
extern int      argb_depth;
extern Colormap argb_cmap;

/* ---- process control ---- */
void spawn(const char *shell_cmd);
void wm_quit(void);

/* ---- key-binding actions -- defined in voidwm.c, bound from the
 * table in config.h. `arg` is action-specific (e.g. workspace index,
 * unused for most). ---- */
void act_spawn_terminal(int arg);
void act_kill_focused(int arg);
void act_view_workspace(int arg);
void act_move_focused_to_workspace(int arg);
void act_focus_cycle(int arg);
void act_toggle_fullscreen(int arg);
void act_quit(int arg);

/* ---- state queries used by bar.c ---- */
const char *wm_focused_title(void);
int         wm_current_workspace(void);   /* 0-based */
int         wm_num_workspaces(void);
int         wm_workspace_occupied(int ws); /* has >=1 window? for dot indicator */

/* ---- called by voidwm.c's event loop ---- */
void bar_create(void);
void bar_redraw(void);
void bar_tick(void);           /* called ~1/sec: clock + battery refresh */
void bar_resize(void);         /* re-reads scr_w/scr_h and repositions the bar */
Window bar_window(void);
int  bar_handle_click(int x, int y); /* returns 1 if handled (e.g. workspace dot) */

void dock_create(void);
void dock_redraw(void);
void dock_poll(int pointer_x, int pointer_y, long now_ms);
void dock_resize(void);        /* re-reads scr_w/scr_h and repositions the dock */
Window dock_window(void);
int  dock_handle_click(Window w, int x, int y); /* returns 1 if handled */
int  dock_is_window(Window w);

#endif /* VOIDWM_H */