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
// console. A resizable window has a grip in the bottom-right corner of its
// contents: dragging it (or `window size`) changes its size in whole
// characters; the pixels that still fit are kept and its program re-lays
// itself out (on_resize). The box left of the close box minimises a window;
// the task bar along the bottom of the screen has a button for every window
// (in the order they were opened): a press restores a minimised window,
// minimises the focused one, or brings another to the front. Windows never
// cover the task bar.

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
#define WIN_GRIP            12              // the resize grip: the contents' last 12 x 12 pixels
#define WIN_MIN_RGB         0x6A7A8A        // the minimise box (a white bar in it)
#define TASKBAR_H           24
#define TASKBAR_BG          0x14283C
#define TASK_BTN_W          150             // at most; narrower when many windows are open
#define TASK_BTN_H          20
#define TASK_GAP            4
#define TASK_ON_BG          0x2B4C7E        // the focused window's button
#define TASK_BG             0x3A4A5C
#define TASK_MIN_BG         0x24303C        // a minimised window's button
#define TASK_TEXT           0xFFFFFF
#define TASK_MIN_TEXT       0x8899AA
#define WIN_MIN_COLS        16
#define WIN_MIN_ROWS        3

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
#define WKEY_ESCAPE     0x11000C
#define WKEY_F2         0x11000D
#define WKEY_F5         0x11000E
#define WKEY_F7         0x11000F

#define DOUBLE_CLICK_MS 400

typedef struct Window Window;
struct Window {
    Bool    used;
    U32     id;
    I32     x, y;                           // outer top-left corner
    U32     w, h;                           // outer size: 2*border + text, + title bar
    U32     cols, rows;                     // text area in characters
    Bool    closable;
    Bool    resizable;
    Bool    minimized;                      // not drawn, no presses; its task bar button restores it
    U32     paper;                          // colour of contents newly exposed by a resize
    char    title[WIN_TITLE_MAX];
    Surface content;                        // cols*8 x rows*16 pixels, off the screen
    // The window's program (all optional):
    Bool  (*on_key)(Window* w, U32 key);    // TRUE: taken
    U0    (*on_click)(Window* w, I32 x, I32 y, Bool double_click);   // in the contents
    U0    (*on_close)(Window* w);           // free what `data` holds
    // cols/rows/content changed (content filled with `paper`): redraw, taking
    // what it wants from `old`. Without it the old pixels are kept top-left.
    U0    (*on_resize)(Window* w, const Surface* old);
    U0*     data;
};

typedef struct {
    Bool on;
    U32  count;                             // windows open
    U32  focus;                             // id of the focused window
    Bool dragging, resizing;
    U32  drag_id;
    I32  grab_dx, grab_dy;                  // pointer - corner when the drag started
    U64  moves, drags, resizes, composed_px;   // composed_px: pixels composed onto the screen
    U32  con_cols, con_rows;                // the console's size, as last chosen
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
Bool          desktop_resize(U32 id, U32 cols, U32 rows);   // FALSE if not resizable; sizes clamped
Bool          desktop_minimize(U32 id);     // FALSE if there is no such window
Bool          desktop_restore(U32 id);      // shown again, in front, focused
U0            desktop_pointer(I32 x, I32 y, U32 buttons);   // from the input driver
Bool          desktop_key(U32 key);         // TRUE if the focused window's program took it
U0            desktop_show(Window* w);      // its contents were redrawn: compose them
U0            desktop_set_title(Window* w, const char* title);
U0            desktop_shell_register(U0);

#endif // DESKTOP_H
