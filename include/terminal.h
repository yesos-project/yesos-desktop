#ifndef TERMINAL_H
#define TERMINAL_H

#include "types.h"
#include "surface.h"

// The console on the screen: everything the console writes (kprintf and
// the shell's echo, through the kprintf hook) is also drawn on the display,
// on the whole screen or in the desktop's window, in the core's 8x16 font,
// with scrolling, a cursor, UTF-8 and the few
// ANSI sequences the core uses (colours, clear screen). Only the boot hart
// draws: output from other harts reaches the serial port only.

#define TERMINAL_FG  0xE0E0E0
#define TERMINAL_BG  0x000000

typedef struct {
    Bool on;
    U32  x, y;                      // top-left pixel of the text area
    U32  cols, rows, col, row;
    U64  chars, scrolls, dropped;   // dropped: other harts, or output while drawing
} TerminalState;

U0             terminal_init(U0);    // after display_init; on if the display is ready
TerminalState* terminal_state(U0);
U0             terminal_enable(Bool on);
// Where the text goes: pixel (x, y) of surface s (the screen, or the
// contents of the desktop's console window), cols x rows characters. With
// clear_it or another surface the area is cleared; a new size on the same
// surface keeps the text (scrolled up if the cursor's row is gone).
U0             terminal_set_area(Surface* s, U32 x, U32 y, U32 cols, U32 rows, Bool clear_it);
// The console's window was resized: s (same Surface, new pixels) gets the
// text of `old` that fits, the cursor's row last, and the terminal its new size.
U0             terminal_resize(const Surface* old, Surface* s, U32 cols, U32 rows);
// The function that places the terminal after a mode change or `term on`
// (the desktop's window); NULL: the whole screen.
typedef U0 (*TerminalLayoutFn)(U0);
U0             terminal_set_layout(TerminalLayoutFn fn);
U0             terminal_relayout(U0);
U0             terminal_shell_register(U0);

#endif // TERMINAL_H
