#ifndef DESKTOP_H
#define DESKTOP_H

#include "types.h"
#include "surface.h"

// The desktop: a plain background and windows that overlap. Window 0 holds
// the console (the on-screen terminal) and cannot be closed; others are
// made with `window new`. Each window keeps its contents in its own buffer
// off the screen; the screen is composed from them, only where something
// changed: background, the windows from the bottom up, the pointer on top.
// A press on a window brings it to the front and gives it the focus; its
// title bar drags it; the box at the right of the title bar closes it.
// Windows always stay whole on the screen. Keys still go to the console.

#define DESKTOP_BG          0x1F3B57
#define WIN_BORDER          2
#define WIN_TITLE_H         20
#define WIN_BORDER_RGB      0x8899AA
#define WIN_TITLE_RGB       0x2B4C7E        // focused
#define WIN_TITLE_TEXT      0xFFFFFF
#define WIN_TITLE_OFF_RGB   0x5B6878        // not focused
#define WIN_TITLE_OFF_TEXT  0xC8CCD0
#define WIN_CLOSE_RGB       0xC04040
#define WIN_CLOSE_SIZE      14
#define WIN_PAPER           0xF4F1E8        // contents of a new window
#define WIN_TITLE_MAX       40
#define WIN_MAX             8
#define WIN_TITLE           "YesOS Console"
#define WIN_COLS            100             // console size wanted; less if the mode is small
#define WIN_ROWS            40
#define WIN_X0              40              // the console's first position
#define WIN_Y0              40

typedef struct {
    Bool    used;
    U32     id;
    I32     x, y;                           // outer top-left corner
    U32     w, h;                           // outer size: 2*border + text, + title bar
    U32     cols, rows;                     // text area in characters
    Bool    closable;
    char    title[WIN_TITLE_MAX];
    Surface content;                        // cols*8 x rows*16 pixels, off the screen
} Window;

typedef struct {
    Bool on;
    U32  count;                             // windows open
    U32  focus;                             // id of the focused window
    Bool dragging;
    U32  drag_id;
    I32  grab_dx, grab_dy;                  // pointer - corner when the drag started
    U64  moves, drags, composed_px;         // composed_px: pixels composed onto the screen
    U32  next_id, created;
} DesktopState;

U0            desktop_init(U0);             // after terminal_init; on if the display is ready
DesktopState* desktop_state(U0);
U0            desktop_enable(Bool on);      // off: the console takes the whole screen
Window*       desktop_window(U32 id);       // NULL if there is none
Window*       desktop_new_window(U32 cols, U32 rows, const char* title);
Bool          desktop_close(U32 id);        // FALSE for the console or an unknown id
U0            desktop_front(U32 id);        // to the top, focused
U0            desktop_move(U32 id, I32 x, I32 y);   // clamped so it stays on the screen
U0            desktop_pointer(I32 x, I32 y, U32 buttons);   // from the input driver
U0            desktop_shell_register(U0);

#endif // DESKTOP_H
