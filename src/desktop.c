// ============================================================================
// YesOS Desktop - the desktop and the console's window (see desktop.h).
// ============================================================================

#include "desktop.h"
#include "display.h"
#include "terminal.h"
#include "font.h"
#include "kprintf.h"
#include "memory.h"
#include "shell.h"

static DesktopState g;
static U32 s_last_buttons;

DesktopState* desktop_state(U0) { return &g; }

static U32 text_x(U0) { return (U32)g.x + WIN_BORDER; }
static U32 text_y(U0) { return (U32)g.y + WIN_BORDER + WIN_TITLE_H; }

static U0 clamp(I32* x, I32* y) {
    DisplayState* d = display_state();
    I32 maxx = (I32)d->width - (I32)g.w, maxy = (I32)d->height - (I32)g.h;
    if (*x > maxx) *x = maxx;
    if (*y > maxy) *y = maxy;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

// Border and title bar; the text area is the terminal's.
static U0 draw_frame(U0) {
    U32 x = (U32)g.x, y = (U32)g.y;
    display_fill(x, y, g.w, WIN_BORDER, WIN_BORDER_RGB);
    display_fill(x, y + g.h - WIN_BORDER, g.w, WIN_BORDER, WIN_BORDER_RGB);
    display_fill(x, y, WIN_BORDER, g.h, WIN_BORDER_RGB);
    display_fill(x + g.w - WIN_BORDER, y, WIN_BORDER, g.h, WIN_BORDER_RGB);
    display_fill(x + WIN_BORDER, y + WIN_BORDER, g.w - 2 * WIN_BORDER, WIN_TITLE_H, WIN_TITLE_RGB);
    display_text(x + WIN_BORDER + 6, y + WIN_BORDER + (WIN_TITLE_H - FONT_HEIGHT) / 2, WIN_TITLE,
                 WIN_TITLE_TEXT, 0, FALSE);
}

// Layout for the current mode: the console as large as wanted and as fits,
// the window where it was (clamped), everything repainted.
static U0 layout(U0) {
    DisplayState* d = display_state();
    if (!d->ready) return;
    U32 fit_cols = d->width > 2 * WIN_BORDER + 32 ? (d->width - 2 * WIN_BORDER - 32) / FONT_WIDTH : 1;
    U32 fit_rows = d->height > 2 * WIN_BORDER + WIN_TITLE_H + 32
                 ? (d->height - 2 * WIN_BORDER - WIN_TITLE_H - 32) / FONT_HEIGHT : 1;
    g.cols = fit_cols < WIN_COLS ? fit_cols : WIN_COLS;
    g.rows = fit_rows < WIN_ROWS ? fit_rows : WIN_ROWS;
    g.w = 2 * WIN_BORDER + g.cols * FONT_WIDTH;
    g.h = 2 * WIN_BORDER + WIN_TITLE_H + g.rows * FONT_HEIGHT;
    clamp(&g.x, &g.y);
    g.dragging = FALSE;
    display_begin();
    display_fill(0, 0, d->width, d->height, DESKTOP_BG);
    draw_frame();
    display_fill(text_x(), text_y(), g.cols * FONT_WIDTH, g.rows * FONT_HEIGHT, TERMINAL_BG);
    terminal_set_area(text_x(), text_y(), g.cols, g.rows, TRUE);
    display_end();
}

static U0 fill_bg(I32 x0, I32 y0, I32 x1, I32 y1) {      // [x0, x1) x [y0, y1), if not empty
    if (x1 > x0 && y1 > y0) display_fill((U32)x0, (U32)y0, (U32)(x1 - x0), (U32)(y1 - y0), DESKTOP_BG);
}

U0 desktop_move(I32 x, I32 y) {
    if (!g.on || !display_state()->ready) return;
    clamp(&x, &y);
    if (x == g.x && y == g.y) return;
    I32 ox = g.x, oy = g.y, w = (I32)g.w, h = (I32)g.h;
    display_begin();
    display_copy((U32)ox, (U32)oy, g.w, g.h, (U32)x, (U32)y);
    // Background where the window was and is no longer: bands above and
    // below the new place, then left and right of it in the rows they share.
    fill_bg(ox, oy, ox + w, y < oy + h ? y : oy + h);
    fill_bg(ox, y + h > oy ? y + h : oy, ox + w, oy + h);
    I32 r0 = oy > y ? oy : y, r1 = oy + h < y + h ? oy + h : y + h;
    fill_bg(ox, r0, x < ox + w ? x : ox + w, r1);
    fill_bg(x + w > ox ? x + w : ox, r0, ox + w, r1);
    g.x = x;
    g.y = y;
    g.moves++;
    terminal_set_area(text_x(), text_y(), g.cols, g.rows, FALSE);
    display_end();
}

U0 desktop_pointer(I32 x, I32 y, U32 buttons) {
    Bool left = (buttons & 1) != 0, was = (s_last_buttons & 1) != 0;
    s_last_buttons = buttons;
    if (!g.on) return;
    if (left && !was) {
        // Press on the title bar (inside the border) starts a drag.
        if (x >= g.x + WIN_BORDER && x < g.x + (I32)g.w - WIN_BORDER &&
            y >= g.y + WIN_BORDER && y < g.y + WIN_BORDER + WIN_TITLE_H) {
            g.dragging = TRUE;
            g.grab_dx = x - g.x;
            g.grab_dy = y - g.y;
            g.drags++;
        }
    } else if (!left) {
        g.dragging = FALSE;
    } else if (g.dragging) {
        desktop_move(x - g.grab_dx, y - g.grab_dy);
    }
}

U0 desktop_enable(Bool on) {
    if (on && !display_state()->ready) return;
    g.on = on;
    g.dragging = FALSE;
    terminal_set_layout(on ? layout : NULL);
}

U0 desktop_init(U0) {
    MemSet(&g, 0, sizeof(g));
    g.x = WIN_X0;
    g.y = WIN_Y0;
    desktop_enable(TRUE);
}

// ---- console: `desktop` ----------------------------------------------------------

static Bool desktop_run(const char* cmd) {
    if (StrCmp(cmd, "desktop") == 0 || StrCmp(cmd, "desktop status") == 0) {
        if (!g.on) kprintf("Desktop: off (the console takes the whole screen)\n");
        else kprintf("Desktop: on, console window at %d,%d, %ux%u pixels, %ux%u characters; "
                     "%u moves, %u drags\n", (I64)g.x, (I64)g.y, (U64)g.w, (U64)g.h,
                     (U64)g.cols, (U64)g.rows, g.moves, g.drags);
    } else if (StrCmp(cmd, "desktop on") == 0) {
        if (!display_state()->ready) kprintf("desktop: no usable display\n");
        desktop_enable(TRUE);
    } else if (StrCmp(cmd, "desktop off") == 0) {
        desktop_enable(FALSE);
    } else if (shell_is_prefix("desktop move ", cmd)) {
        const char* p = cmd + 13;
        I32 x = (I32)shell_parse_uint(p);
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        I32 y = (I32)shell_parse_uint(p);
        if (!g.on) kprintf("desktop: off\n");
        else {
            desktop_move(x, y);
            kprintf("[DESKTOP] window at %d,%d\n", (I64)g.x, (I64)g.y);
        }
    } else if (shell_is_prefix("desktop", cmd) && (cmd[7] == ' ' || cmd[7] == '\0')) {
        kprintf("usage: desktop [status] | desktop on | desktop off | desktop move <x> <y>\n");
    } else {
        return FALSE;
    }
    return TRUE;
}

static U0 desktop_help(U0) {
    kprintf("  Desktop (the console in a window; drag it by its title bar)\n");
    kprintf("    desktop [status]   desktop on   desktop off   desktop move <x> <y>\n");
}

static const ShellExtension s_desktop_shell = { desktop_run, desktop_help, NULL, NULL };

U0 desktop_shell_register(U0) {
    shell_register(&s_desktop_shell);
}
