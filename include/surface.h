#ifndef SURFACE_H
#define SURFACE_H

#include "types.h"

// A rectangle of 32-bit pixels (0x00RRGGBB) to draw on: the framebuffer, a
// window's own contents kept off the screen, or a view into either. Every
// call clips to the surface, coordinates may be negative, and `damage` (if
// set) is told which part changed, so the desktop can show it.

typedef struct Surface Surface;
typedef U0 (*SurfaceDamageFn)(Surface* s, I32 x, I32 y, I32 w, I32 h);

struct Surface {
    U32*            px;
    I32             w, h;
    U32             pitch;              // in pixels
    SurfaceDamageFn damage;             // NULL: nobody to tell
    U0*             owner;              // for the damage callback
};

// A view of the part (x, y, w, h) of s, already clipped; no damage callback.
Surface surf_view(const Surface* s, I32 x, I32 y, I32 w, I32 h);
U0      surf_fill(Surface* s, I32 x, I32 y, I32 w, I32 h, U32 rgb);
// One character of the core's font; bg painted when opaque.
U0      surf_glyph(Surface* s, I32 x, I32 y, U32 cp, U32 fg, U32 bg, Bool opaque);
// UTF-8 text on one line, transparent background. Returns the x after it.
I32     surf_text(Surface* s, I32 x, I32 y, const char* utf8, U32 fg);
// In (x, y, w, h), move the rows up by dy; the freed rows are painted bg.
U0      surf_scroll(Surface* s, I32 x, I32 y, I32 w, I32 h, I32 dy, U32 bg);
// Copy all of src to (x, y) on dst (different buffers).
U0      surf_blit(Surface* dst, I32 x, I32 y, const Surface* src);

#endif // SURFACE_H
