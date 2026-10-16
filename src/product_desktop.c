// ============================================================================
// YesOS Desktop - product glue: the YesOS core, its serial console and, one
// feature at a time, the desktop (today: audio output, a framebuffer with
// the console on it, keyboard and pointer). No network services
// and no optional modules.
// ============================================================================

#include "product.h"
#include "kprintf.h"
#include "hda.h"
#include "display.h"
#include "terminal.h"
#include "input.h"
#include "desktop.h"
#include "files.h"

const char* const PRODUCT_NAME = "YesOS Desktop";

U0 product_early_init(U0) {
}

U0 product_init(U0) {
    hda_init();
    audio_shell_register();
    display_init();
    display_shell_register();
    terminal_init();
    terminal_shell_register();
    desktop_init();
    desktop_shell_register();
    files_shell_register();
    input_init();
    input_shell_register();
}

U0 product_boot_summary(U0) {
    kprintf(" [ OK ] DESKTOP          : core + serial console (no network services)\n");
    HdaState* a = hda_state();
    if (a->ready) kprintf(" [ OK ] AUDIO            : HD Audio, 48 kHz 16-bit stereo (codec %u)\n", a->cad);
    else kprintf(" [ -- ] AUDIO            : %s\n", a->present ? "HD Audio controller not usable" : "no HD Audio controller");
    DisplayState* d = display_state();
    if (d->ready) kprintf(" [ OK ] DISPLAY          : bochs-display, %ux%u, 32 bpp\n", d->width, d->height);
    else kprintf(" [ -- ] DISPLAY          : %s\n", d->present ? "bochs-display not usable" : "no display");
    if (terminal_state()->on) kprintf(" [ OK ] TERMINAL         : console on the screen, %ux%u characters\n",
                                      (U64)terminal_state()->cols, (U64)terminal_state()->rows);
    if (desktop_state()->on) kprintf(" [ OK ] WINDOWS          : desktop, the console in a window at %d,%d\n",
                                     (I64)desktop_window(0)->x, (I64)desktop_window(0)->y);
    InputState* in = input_state();
    if (in->devices) kprintf(" [ OK ] INPUT            : virtio-input, %u keyboard(s), %u pointer(s), layout %s\n",
                             (U64)in->keyboards, (U64)in->pointers, in->layout == LAYOUT_ES ? "es" : "us");
    else kprintf(" [ -- ] INPUT            : no virtio-input devices (keyboard: the serial console)\n");
}

U0 product_poll(U0) {
    hda_service();                      // keep a background sound's ring filled
    input_poll();
}
