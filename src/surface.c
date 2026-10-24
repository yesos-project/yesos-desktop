// ============================================================================
// YesOS Desktop - drawing on surfaces (see surface.h). Integer only; rows are
// filled and copied by yesmath (core module "math": RVV when the hart has V).
// ============================================================================

#include "surface.h"
#include "font.h"
#include "yesmath.h"

// Clip (x, y, w, h) to the surface; FALSE if nothing is left.
static Bool clip(const Surface* s, I32* x, I32* y, I32* w, I32* h) {
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > s->w) *w = s->w - *x;
    if (*y + *h > s->h) *h = s->h - *y;
    return *w > 0 && *h > 0;
}

static U0 damaged(Surface* s, I32 x, I32 y, I32 w, I32 h) {
    if (s->damage) s->damage(s, x, y, w, h);
}

Surface surf_view(const Surface* s, I32 x, I32 y, I32 w, I32 h) {
    Surface v = { 0, 0, 0, s->pitch, 0, 0 };
    if (clip(s, &x, &y, &w, &h)) {
        v.px = s->px + (U64)y * s->pitch + x;
        v.w = w;
        v.h = h;
    }
    return v;
}

U0 surf_fill(Surface* s, I32 x, I32 y, I32 w, I32 h, U32 rgb) {
    if (!clip(s, &x, &y, &w, &h)) return;
    for (I32 r = 0; r < h; r++) {
        ym_fill32(s->px + (U64)(y + r) * s->pitch + x, rgb, (U64)w);
    }
    damaged(s, x, y, w, h);
}

U0 surf_glyph(Surface* s, I32 x, I32 y, U32 cp, U32 fg, U32 bg, Bool opaque) {
    const U8* rows = font_glyph(cp);
    I32 cx = x, cy = y, cw = FONT_WIDTH, ch = FONT_HEIGHT;
    if (!clip(s, &cx, &cy, &cw, &ch)) return;
    for (I32 r = cy - y; r < cy - y + ch; r++) {
        U32* px = s->px + (U64)(y + r) * s->pitch + x;
        U8 bits = rows[r];
        for (I32 c = cx - x; c < cx - x + cw; c++) {
            if (bits & (0x80 >> c)) px[c] = fg;
            else if (opaque) px[c] = bg;
        }
    }
    damaged(s, cx, cy, cw, ch);
}

I32 surf_text(Surface* s, I32 x, I32 y, const char* t, U32 fg) {
    while (t && *t) {
        surf_glyph(s, x, y, font_utf8_next(&t), fg, 0, FALSE);
        x += FONT_WIDTH;
    }
    return x;
}

U0 surf_scroll(Surface* s, I32 x, I32 y, I32 w, I32 h, I32 dy, U32 bg) {
    if (!clip(s, &x, &y, &w, &h)) return;
    if (dy > h) dy = h;
    for (I32 r = 0; r + dy < h; r++) {                    // top to bottom: the source is below
        U32* dst = s->px + (U64)(y + r) * s->pitch + x;
        ym_memcpy(dst, dst + (U64)dy * s->pitch, (U64)w * 4);      // different rows: no overlap
    }
    SurfaceDamageFn d = s->damage;                        // one report for the whole move
    s->damage = 0;
    surf_fill(s, x, y + h - dy, w, dy, bg);
    s->damage = d;
    damaged(s, x, y, w, h);
}

U0 surf_blit(Surface* dst, I32 x, I32 y, const Surface* src) {
    I32 cx = x, cy = y, cw = src->w, ch = src->h;
    if (!src->px || !clip(dst, &cx, &cy, &cw, &ch)) return;
    for (I32 r = 0; r < ch; r++) {
        ym_memcpy(dst->px + (U64)(cy + r) * dst->pitch + cx,
                  src->px + (U64)(cy - y + r) * src->pitch + (cx - x), (U64)cw * 4);
    }
    damaged(dst, cx, cy, cw, ch);
}
