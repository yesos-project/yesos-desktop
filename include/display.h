#ifndef DISPLAY_H
#define DISPLAY_H

#include "types.h"

// Linear framebuffer on QEMU's bochs-display (PCI 1234:1111): 32 bits per
// pixel, 0x00RRGGBB. The mode is set through the Bochs VBE "DISPI" registers
// in BAR2 (+0x500); the framebuffer is BAR0. Integer arithmetic only.

#define DISPLAY_DEFAULT_W  1024
#define DISPLAY_DEFAULT_H  768

typedef struct {
    Bool      present, ready;
    U8        slot;
    U16       dispi_id;             // 0xB0C0..0xB0C5
    U32*      fb;
    U64       fb_size;              // bytes of video memory (BAR0)
    U32       width, height, pitch; // pitch in pixels
} DisplayState;

Bool          display_init(U0);    // find the device and set the default mode
DisplayState* display_state(U0);
Bool          display_set_mode(U32 w, U32 h);  // 32 bpp; FALSE if not possible
U0            display_fill(U32 x, U32 y, U32 w, U32 h, U32 rgb);   // clipped
U0            display_test_pattern(U0);

// Text in the core's YesOS 8x16 font (font.h):
// UTF-8 up to U+00FF (printable ASCII and Latin-1 for Spanish); anything the
// font lacks is drawn as '?'. '\n' starts a new line at x. With `opaque`
// the cell background is painted `bg`, else left as it is. Clipped to the
// screen. Returns the x after the last character of the last line.
U32           display_text(U32 x, U32 y, const char* utf8, U32 fg, U32 bg, Bool opaque);
// One character cell, background painted (the on-screen terminal).
U0            display_glyph(U32 x, U32 y, U32 cp, U32 fg, U32 bg);

// Rows y+dy .. y+h-1 move up to y; the dy rows freed at the bottom are
// painted bg (the on-screen terminal scrolls with it).
U0            display_scroll(U32 y, U32 h, U32 dy, U32 bg);

// Mouse pointer: an arrow drawn on top of the picture. The pixels under it
// are saved and put back when it moves; every display_* call above draws
// under it. (x, y) is the tip.
#define DISPLAY_POINTER_W  11
#define DISPLAY_POINTER_H  19
U0            display_pointer(I32 x, I32 y, Bool visible);
// Bracket several drawing calls so the pointer is lifted once, not per call.
U0            display_begin(U0);
U0            display_end(U0);

U0            display_shell_register(U0);

#endif // DISPLAY_H
