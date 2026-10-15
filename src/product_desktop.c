// ============================================================================
// YesOS Desktop - product glue: the YesOS core, its serial console and, one
// feature at a time, the desktop (today: audio output). No network services
// and no optional modules.
// ============================================================================

#include "product.h"
#include "kprintf.h"
#include "hda.h"

const char* const PRODUCT_NAME = "YesOS Desktop";

U0 product_early_init(U0) {
}

U0 product_init(U0) {
    hda_init();
    audio_shell_register();
}

U0 product_boot_summary(U0) {
    kprintf(" [ OK ] DESKTOP          : core + serial console (no network services)\n");
    HdaState* a = hda_state();
    if (a->ready) kprintf(" [ OK ] AUDIO            : HD Audio, 48 kHz 16-bit stereo (codec %u)\n", a->cad);
    else kprintf(" [ -- ] AUDIO            : %s\n", a->present ? "HD Audio controller not usable" : "no HD Audio controller");
}

U0 product_poll(U0) {
}
