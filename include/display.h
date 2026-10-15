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
U0            display_shell_register(U0);

#endif // DISPLAY_H
