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
// Windows always stay whole on the screen. Keys go to the focused window
// if it has a program that takes them (the file browser), else to the
// console.

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

// Keys for window programs: Unicode code points, or these.
#define WKEY_ENTER      0x110001
#define WKEY_BACKSPACE  0x110002
#define WKEY_UP         0x110003
#define WKEY_DOWN       0x110004
#define WKEY_LEFT       0x110005
#define WKEY_RIGHT      0x110006
#define WKEY_HOME       0x110007
#define WKEY_END        0x110008
#define WKEY_DELETE     0x110009
#define WKEY_PAGEUP     0x11000A
#define WKEY_PAGEDOWN   0x11000B

#define DOUBLE_CLICK_MS 400

typedef struct Window Window;
struct Window {
    Bool    used;
    U32     id;
    I32     x, y;                           // outer top-left corner
    U32     w, h;                           // outer size: 2*border + text, + title bar
    U32     cols, rows;                     // text area in characters
    Bool    closable;
    char    title[WIN_TITLE_MAX];
    Surface content;                        // cols*8 x rows*16 pixels, off the screen
    // The window's program (all optional):
    Bool  (*on_key)(Window* w, U32 key);    // TRUE: taken
    U0    (*on_click)(Window* w, I32 x, I32 y, Bool double_click);   // in the contents
    U0    (*on_close)(Window* w);           // free what `data` holds
    U0*     data;
};

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
Bool          desktop_key(U32 key);         // TRUE if the focused window's program took it
U0            desktop_show(Window* w);      // its contents were redrawn: compose them
U0            desktop_set_title(Window* w, const char* title);
U0            desktop_shell_register(U0);

#endif // DESKTOP_H
