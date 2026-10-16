#ifndef DESKTOP_H
#define DESKTOP_H

#include "types.h"

// The desktop: a plain background and one window holding the console (the
// on-screen terminal). The window is dragged by its title bar with the
// pointer, or moved with `desktop move`; it always stays whole on the
// screen, so moving it is a copy of its pixels plus the background painted
// where it was. Stage 1 of the windows: one window, no overlap.

#define DESKTOP_BG        0x1F3B57
#define WIN_BORDER        2
#define WIN_TITLE_H       20
#define WIN_BORDER_RGB    0x8899AA
#define WIN_TITLE_RGB     0x2B4C7E
#define WIN_TITLE_TEXT    0xFFFFFF
#define WIN_TITLE         "YesOS Console"
#define WIN_COLS          100           // console size wanted; less if the mode is small
#define WIN_ROWS          40
#define WIN_X0            40            // first position
#define WIN_Y0            40

typedef struct {
    Bool on;
    I32  x, y;                          // outer top-left corner
    U32  w, h;                          // outer size: 2*border + text, + title bar
    U32  cols, rows;
    Bool dragging;
    I32  grab_dx, grab_dy;              // pointer - corner when the drag started
    U64  moves, drags;
} DesktopState;

U0            desktop_init(U0);         // after terminal_init; on if the display is ready
DesktopState* desktop_state(U0);
U0            desktop_enable(Bool on);  // off: the console takes the whole screen
U0            desktop_move(I32 x, I32 y);   // clamped so the window stays on the screen
U0            desktop_pointer(I32 x, I32 y, U32 buttons);   // from the input driver
U0            desktop_shell_register(U0);

#endif // DESKTOP_H
