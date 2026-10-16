#ifndef TERMINAL_H
#define TERMINAL_H

#include "types.h"

// The console on the screen: everything the console writes (kprintf and
// the shell's echo, through the kprintf hook) is also drawn on the display
// in the core's 8x16 font, with scrolling, a cursor, UTF-8 and the few
// ANSI sequences the core uses (colours, clear screen). Only the boot hart
// draws: output from other harts reaches the serial port only.

#define TERMINAL_FG  0xE0E0E0
#define TERMINAL_BG  0x000000

typedef struct {
    Bool on;
    U32  cols, rows, col, row;
    U64  chars, scrolls, dropped;   // dropped: other harts, or output while drawing
} TerminalState;

U0             terminal_init(U0);    // after display_init; on if the display is ready
TerminalState* terminal_state(U0);
U0             terminal_enable(Bool on);
U0             terminal_shell_register(U0);

#endif // TERMINAL_H
