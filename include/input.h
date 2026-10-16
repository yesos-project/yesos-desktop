#ifndef INPUT_H
#define INPUT_H

#include "types.h"

// virtio-input devices (QEMU virtio-keyboard-pci, virtio-tablet-pci,
// virtio-mouse-pci), polled from the main loop. Keys are translated with a
// keyboard layout (US or Spanish, dead keys for accents) into UTF-8 and
// typed into the console; arrows, Home, End and Delete as the escape
// sequences a terminal sends (the core's shell edits the line). The layout
// is kept in /input.cfg on the RedSea disk. The pointer is kept in screen
// pixels and drawn on the display.

#define INPUT_MAX_DEVICES  4

typedef enum { INPUT_KEYBOARD, INPUT_TABLET, INPUT_MOUSE } InputKind;
typedef enum { LAYOUT_US, LAYOUT_ES } InputLayout;

typedef struct {
    U32  devices;
    U32  keyboards, pointers;
    U64  events;                    // all events taken from the queues
    U64  key_presses, chars_typed;  // keyboard presses; UTF-8 characters sent to the console
    I32  x, y;                      // pointer, screen pixels
    U32  buttons;                   // bit 0 left, 1 right, 2 middle
    U64  clicks[3];                 // presses of each button
    I32  click_x, click_y;          // where the last press happened
    I64  wheel;                     // sum of the wheel steps
    InputLayout layout;
    Bool        layout_saved;       // read from or written to /input.cfg on the disk
} InputState;

Bool        input_init(U0);         // find and start every virtio-input device
U0          input_poll(U0);         // main loop: take the events
InputState* input_state(U0);
U0          input_shell_register(U0);

#endif // INPUT_H
