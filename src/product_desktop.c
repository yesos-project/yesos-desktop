// ============================================================================
// YesOS Desktop - product glue. For now the desktop is the YesOS core and its
// serial console only: no network services, no optional modules. Features
// (display, input, windows, audio, apps) are added here one at a time.
// ============================================================================

#include "product.h"
#include "kprintf.h"

const char* const PRODUCT_NAME = "YesOS Desktop";

U0 product_early_init(U0) {
}

U0 product_init(U0) {
}

U0 product_boot_summary(U0) {
    kprintf(" [ OK ] DESKTOP          : core + serial console (no network services yet)\n");
}

U0 product_poll(U0) {
}
