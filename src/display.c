// ============================================================================
// YesOS Desktop - bochs-display linear framebuffer.
// The lab's driver (drivers/vga_fb.c) wrote fixed BAR addresses; here the
// core's PCI allocator assigns them.
// ============================================================================

#include "display.h"
#include "pci.h"
#include "ports.h"
#include "kprintf.h"
#include "memory.h"
#include "shell.h"
#include "font.h"

#define BOCHS_VENDOR        0x1234
#define BOCHS_DEVICE        0x1111
#define DISPI_OFFSET        0x500   // in BAR2: 16-bit registers, index * 2
#define DISPI_ID            0
#define DISPI_XRES          1
#define DISPI_YRES          2
#define DISPI_BPP           3
#define DISPI_ENABLE        4
#define DISPI_VIRT_WIDTH    6
#define DISPI_VIRT_HEIGHT   7
#define DISPI_X_OFFSET      8
#define DISPI_Y_OFFSET      9
#define DISPI_ENABLED       0x01
#define DISPI_LFB_ENABLED   0x40
#define DISPI_ID_MIN        0xB0C0
#define DISPI_ID_MAX        0xB0C5
#define DISPLAY_MAX_W       4096
#define DISPLAY_MAX_H       4096

static DisplayState g;
static uintptr_t s_dispi;

// ---- pointer ---------------------------------------------------------------------
// 'X' black outline, '.' white fill, ' ' transparent. tools/test_display.py
// and test_input.py draw the same arrow.
static const char* const s_arrow[DISPLAY_POINTER_H] = {
    "X          ",
    "XX         ",
    "X.X        ",
    "X..X       ",
    "X...X      ",
    "X....X     ",
    "X.....X    ",
    "X......X   ",
    "X.......X  ",
    "X........X ",
    "X.....XXXXX",
    "X..X..X    ",
    "X.X X..X   ",
    "XX  X..X   ",
    "X    X..X  ",
    "     X..X  ",
    "      X..X ",
    "      X..X ",
    "       XX  ",
};
static U32  s_under[DISPLAY_POINTER_H][DISPLAY_POINTER_W];
static I32  s_ptr_x, s_ptr_y;
static Bool s_ptr_visible, s_ptr_drawn;
static U32  s_draw_depth;

static U0 pointer_erase(U0) {
    if (!s_ptr_drawn) return;
    for (I32 r = 0; r < DISPLAY_POINTER_H; r++)
        for (I32 c = 0; c < DISPLAY_POINTER_W; c++) {
            I32 x = s_ptr_x + c, y = s_ptr_y + r;
            if (s_arrow[r][c] != ' ' && x >= 0 && y >= 0 && x < (I32)g.width && y < (I32)g.height)
                g.fb[(U64)y * g.pitch + x] = s_under[r][c];
        }
    s_ptr_drawn = FALSE;
}

static U0 pointer_paint(U0) {
    if (!s_ptr_visible || !g.ready || s_ptr_drawn) return;
    for (I32 r = 0; r < DISPLAY_POINTER_H; r++)
        for (I32 c = 0; c < DISPLAY_POINTER_W; c++) {
            I32 x = s_ptr_x + c, y = s_ptr_y + r;
            if (s_arrow[r][c] == ' ' || x < 0 || y < 0 || x >= (I32)g.width || y >= (I32)g.height) continue;
            U32* px = &g.fb[(U64)y * g.pitch + x];
            s_under[r][c] = *px;
            *px = s_arrow[r][c] == 'X' ? 0x000000 : 0xFFFFFF;
        }
    s_ptr_drawn = TRUE;
}

// Every drawing call is bracketed by these: the pointer is lifted off the
// picture first and put back on top afterwards (calls may nest).
static U0 draw_begin(U0) { if (s_draw_depth++ == 0) pointer_erase(); }
static U0 draw_end(U0) { if (--s_draw_depth == 0) pointer_paint(); }

U0 display_begin(U0) { draw_begin(); }
U0 display_end(U0) { draw_end(); }

U0 display_pointer(I32 x, I32 y, Bool visible) {
    draw_begin();
    s_ptr_x = x;
    s_ptr_y = y;
    s_ptr_visible = visible;
    draw_end();
}

static inline U0 dispi_write(U32 reg, U16 v) { mmio_write16(s_dispi + reg * 2, v); }
static inline U16 dispi_read(U32 reg) { return mmio_read16(s_dispi + reg * 2); }

DisplayState* display_state(U0) { return &g; }

Bool display_set_mode(U32 w, U32 h) {
    if (!g.present || w == 0 || h == 0 || w > DISPLAY_MAX_W || h > DISPLAY_MAX_H) return FALSE;
    if ((U64)w * h * 4 > g.fb_size) return FALSE;
    s_ptr_drawn = FALSE;                    // what was under it belongs to the old mode
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (U16)w);
    dispi_write(DISPI_YRES, (U16)h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, (U16)w);
    dispi_write(DISPI_VIRT_HEIGHT, (U16)h);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED);
    // The device clamps what it cannot do: read the mode back.
    if (dispi_read(DISPI_XRES) != w || dispi_read(DISPI_YRES) != h || dispi_read(DISPI_BPP) != 32) {
        g.ready = FALSE;
        return FALSE;
    }
    g.width = w;
    g.height = h;
    g.pitch = dispi_read(DISPI_VIRT_WIDTH);
    g.ready = TRUE;
    g.mode_serial++;
    return TRUE;
}

Bool display_init(U0) {
    MemSet(&g, 0, sizeof(g));
    PciDevice dev;
    if (!pci_find_id(BOCHS_VENDOR, BOCHS_DEVICE, 0, &dev)) return FALSE;
    g.present = TRUE;
    g.slot = dev.slot;
    if (!pci_enable(&dev) || !dev.bar[0] || !dev.bar[2]) {
        kprintf("[DISPLAY] BARs not assigned\n");
        return FALSE;
    }
    g.fb = (U32*)dev.bar[0];
    g.fb_size = dev.bar_size[0];
    s_dispi = dev.bar[2] + DISPI_OFFSET;
    g.dispi_id = dispi_read(DISPI_ID);
    if (g.dispi_id < DISPI_ID_MIN || g.dispi_id > DISPI_ID_MAX) {
        kprintf("[DISPLAY] unexpected DISPI id 0x%x\n", g.dispi_id);
        return FALSE;
    }
    if (!display_set_mode(DISPLAY_DEFAULT_W, DISPLAY_DEFAULT_H)) return FALSE;
    display_fill(0, 0, g.width, g.height, 0x000000);
    return TRUE;
}

U0 display_fill(U32 x, U32 y, U32 w, U32 h, U32 rgb) {
    if (!g.ready || x >= g.width || y >= g.height) return;
    if (w > g.width - x) w = g.width - x;
    if (h > g.height - y) h = g.height - y;
    draw_begin();
    for (U32 r = 0; r < h; r++) {
        U32* row = g.fb + (U64)(y + r) * g.pitch + x;
        for (U32 c = 0; c < w; c++) row[c] = rgb;
    }
    draw_end();
}

U0 display_scroll(U32 x, U32 y, U32 w, U32 h, U32 dy, U32 bg) {
    if (!g.ready || x >= g.width || y >= g.height) return;
    if (w > g.width - x) w = g.width - x;
    if (h > g.height - y) h = g.height - y;
    if (dy > h) dy = h;
    draw_begin();
    for (U32 r = 0; r + dy < h; r++) {                     // top to bottom: the source is below
        U32* dst = g.fb + (U64)(y + r) * g.pitch + x;
        const U32* src = dst + (U64)dy * g.pitch;
        for (U32 c = 0; c < w; c++) dst[c] = src[c];
    }
    display_fill(x, y + h - dy, w, dy, bg);
    draw_end();
}

U0 display_copy(U32 sx, U32 sy, U32 w, U32 h, U32 dx, U32 dy) {
    if (!g.ready || sx + w > g.width || dx + w > g.width || sy + h > g.height || dy + h > g.height) return;
    draw_begin();
    // Rows in the order that never overwrites a source row before it is read,
    // and within a row the same for the columns.
    for (U32 i = 0; i < h; i++) {
        U32 r = dy > sy ? h - 1 - i : i;
        U32* dst = g.fb + (U64)(dy + r) * g.pitch + dx;
        const U32* src = g.fb + (U64)(sy + r) * g.pitch + sx;
        if (dst > src) for (U32 c = w; c-- > 0;) dst[c] = src[c];
        else for (U32 c = 0; c < w; c++) dst[c] = src[c];
    }
    draw_end();
}

// Test pattern, reproduced exactly by tools/test_display.py:
// - top half: 8 colour bars (white, yellow, cyan, green, magenta, red,
//   blue, black), each width/8 wide, the last one taking the remainder;
// - bottom quarter-band: a horizontal grey ramp, value = x * 255 / (width - 1);
// - last quarter: a 16 x 16 pixel checkerboard of white and black;
// - a 1-pixel red frame around the whole screen.
U0 display_test_pattern(U0) {
    if (!g.ready) return;
    static const U32 bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000 };
    U32 w = g.width, h = g.height, bw = w / 8;
    draw_begin();
    for (U32 i = 0; i < 8; i++) display_fill(i * bw, 0, i == 7 ? w - 7 * bw : bw, h / 2, bars[i]);
    for (U32 y = h / 2; y < h * 3 / 4; y++) {
        U32* row = g.fb + (U64)y * g.pitch;
        for (U32 x = 0; x < w; x++) {
            U32 v = x * 255 / (w - 1);
            row[x] = (v << 16) | (v << 8) | v;
        }
    }
    for (U32 y = h * 3 / 4; y < h; y++) {
        U32* row = g.fb + (U64)y * g.pitch;
        for (U32 x = 0; x < w; x++) row[x] = (((x >> 4) ^ (y >> 4)) & 1) ? 0x000000 : 0xFFFFFF;
    }
    display_fill(0, 0, w, 1, 0xFF0000);
    display_fill(0, h - 1, w, 1, 0xFF0000);
    display_fill(0, 0, 1, h, 0xFF0000);
    display_fill(w - 1, 0, 1, h, 0xFF0000);
    draw_end();
}

// ---- text ----------------------------------------------------------------------

static U0 draw_glyph(U32 x, U32 y, U32 cp, U32 fg, U32 bg, Bool opaque) {
    const U8* rows = font_glyph(cp);
    for (U32 r = 0; r < FONT_HEIGHT && y + r < g.height; r++) {
        U32* px = g.fb + (U64)(y + r) * g.pitch + x;
        U8 bits = rows[r];
        for (U32 c = 0; c < FONT_WIDTH && x + c < g.width; c++) {
            if (bits & (0x80 >> c)) px[c] = fg;
            else if (opaque) px[c] = bg;
        }
    }
}

U0 display_glyph(U32 x, U32 y, U32 cp, U32 fg, U32 bg) {
    if (!g.ready || x >= g.width || y >= g.height) return;
    draw_begin();
    draw_glyph(x, y, cp, fg, bg, TRUE);
    draw_end();
}

U32 display_text(U32 x, U32 y, const char* s, U32 fg, U32 bg, Bool opaque) {
    if (!g.ready || !s) return x;
    U32 cx = x;
    draw_begin();
    while (*s) {
        U32 cp = font_utf8_next(&s);
        if (cp == '\n') {
            cx = x;
            y += FONT_HEIGHT;
            continue;
        }
        if (cx < g.width && y < g.height) draw_glyph(cx, y, cp, fg, bg, opaque);
        cx += FONT_WIDTH;
    }
    draw_end();
    return cx;
}

// ---- console: `display` ----------------------------------------------------------

static const char* next_word(const char* p) {
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    return p;
}

static U64 parse_hex(const char* p) {
    U64 v = 0;
    for (; *p; p++) {
        U8 c = (U8)*p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d;
    }
    return v;
}

static Bool display_run(const char* cmd) {
    if (StrCmp(cmd, "display") == 0 || StrCmp(cmd, "display status") == 0) {
        if (!g.present) kprintf("Display: no bochs-display on the PCI bus\n");
        else if (!g.ready) kprintf("Display: bochs-display at PCI %02x.0, NOT ready\n", g.slot);
        else kprintf("Display: bochs-display at PCI %02x.0 (DISPI 0x%x), %ux%u, 32 bpp, pitch %u, "
                     "framebuffer 0x%x (%u KiB)\n", g.slot, g.dispi_id, g.width, g.height, g.pitch,
                     (U32)(uintptr_t)g.fb, (U32)(g.fb_size / 1024));
        return TRUE;
    }
    if (!shell_is_prefix("display", cmd)) return FALSE;
    if (!g.present || (!g.ready && !shell_is_prefix("display mode ", cmd))) {
        kprintf("display: no usable display\n");
        return TRUE;
    }
    if (StrCmp(cmd, "display test") == 0) {
        display_test_pattern();
        kprintf("[DISPLAY] test pattern drawn (%ux%u)\n", g.width, g.height);
    } else if (shell_is_prefix("display clear", cmd)) {
        const char* p = next_word(cmd + 8);
        U32 rgb = *p ? (U32)parse_hex(p) & 0xFFFFFF : 0;
        display_fill(0, 0, g.width, g.height, rgb);
        kprintf("[DISPLAY] cleared to %06x\n", rgb);
    } else if (shell_is_prefix("display print ", cmd)) {
        // display print <x> <y> <rrggbb> <text>   (transparent background)
        const char* p = cmd + 14;
        U32 x = (U32)shell_parse_uint(p);
        p = next_word(p);
        U32 y = (U32)shell_parse_uint(p);
        p = next_word(p);
        U32 fg = (U32)parse_hex(p) & 0xFFFFFF;
        p = next_word(p);
        U32 end = display_text(x, y, p, fg, 0, FALSE);
        kprintf("[DISPLAY] text at %u,%u, ends at x=%u\n", x, y, end);
    } else if (shell_is_prefix("display mode ", cmd)) {
        const char* p = cmd + 13;
        U32 w = (U32)shell_parse_uint(p);
        p = next_word(p);
        U32 h = (U32)shell_parse_uint(p);
        if (display_set_mode(w, h)) {
            display_fill(0, 0, w, h, 0);
            kprintf("[DISPLAY] mode %ux%u\n", w, h);
        } else {
            kprintf("display: mode %ux%u refused\n", w, h);
            display_set_mode(DISPLAY_DEFAULT_W, DISPLAY_DEFAULT_H);
        }
    } else {
        kprintf("usage: display [status] | display test | display clear [rrggbb] | display mode <w> <h>\n"
                "       display print <x> <y> <rrggbb> <text>\n");
    }
    return TRUE;
}

static U0 display_help(U0) {
    kprintf("  Display\n");
    kprintf("    display [status]   display test   display clear [rrggbb]   display mode <w> <h>\n");
    kprintf("    display print <x> <y> <rrggbb> <text>\n");
}

static const ShellExtension s_display_shell = { display_run, display_help, NULL, NULL };

U0 display_shell_register(U0) {
    shell_register(&s_display_shell);
}
