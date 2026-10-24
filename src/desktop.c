// ============================================================================
// YesOS Desktop - the desktop and its windows (see desktop.h).
// ============================================================================

#include "desktop.h"
#include "display.h"
#include "terminal.h"
#include "font.h"
#include "kprintf.h"
#include "memory.h"
#include "shell.h"
#include "timer.h"

static DesktopState g;
static Window s_win[WIN_MAX];
static U32    s_z[WIN_MAX];                 // indices into s_win, bottom to top
static U32    s_nz;
static U32    s_last_buttons;
static U32    s_click_id = 0xFFFFFFFF;      // the last press in a window's contents, for double clicks
static I32    s_click_x, s_click_y;
static U64    s_click_ms;

DesktopState* desktop_state(U0) { return &g; }

Window* desktop_window(U32 id) {
    for (U32 i = 0; i < WIN_MAX; i++)
        if (s_win[i].used && s_win[i].id == id) return &s_win[i];
    return NULL;
}

static Window* top(U0) { return s_nz ? &s_win[s_z[s_nz - 1]] : NULL; }

static U0 clamp(const Window* w, I32* x, I32* y) {
    DisplayState* d = display_state();
    I32 maxx = (I32)d->width - (I32)w->w, maxy = (I32)d->height - (I32)w->h;
    if (*x > maxx) *x = maxx;
    if (*y > maxy) *y = maxy;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

// The largest text area that fits the mode with a 16-pixel margin.
static U0 fit(U32* cols, U32* rows) {
    DisplayState* d = display_state();
    U32 fc = d->width > 2 * WIN_BORDER + 32 ? (d->width - 2 * WIN_BORDER - 32) / FONT_WIDTH : 1;
    U32 fr = d->height > 2 * WIN_BORDER + WIN_TITLE_H + 32
           ? (d->height - 2 * WIN_BORDER - WIN_TITLE_H - 32) / FONT_HEIGHT : 1;
    if (*cols > fc) *cols = fc;
    if (*rows > fr) *rows = fr;
    if (*cols == 0) *cols = 1;
    if (*rows == 0) *rows = 1;
}

static U0 set_size(Window* w, U32 cols, U32 rows) {
    w->cols = cols;
    w->rows = rows;
    w->w = 2 * WIN_BORDER + cols * FONT_WIDTH;
    w->h = 2 * WIN_BORDER + WIN_TITLE_H + rows * FONT_HEIGHT;
}

static U32 close_x(const Window* w) { return w->w - WIN_BORDER - WIN_CLOSE_SIZE - 3; }   // relative to the window
static U32 close_y(U0) { return WIN_BORDER + 3; }

// ---- composing -----------------------------------------------------------------

// Window w drawn on view v, whose (0, 0) is the screen's (rx, ry).
static U0 draw_window(Surface* v, const Window* w, I32 rx, I32 ry) {
    I32 X = w->x - rx, Y = w->y - ry, W = (I32)w->w, H = (I32)w->h;
    if (X >= v->w || Y >= v->h || X + W <= 0 || Y + H <= 0) return;
    Bool focused = w->id == g.focus;
    surf_fill(v, X, Y, W, WIN_BORDER, WIN_BORDER_RGB);
    surf_fill(v, X, Y + H - WIN_BORDER, W, WIN_BORDER, WIN_BORDER_RGB);
    surf_fill(v, X, Y + WIN_BORDER, WIN_BORDER, H - 2 * WIN_BORDER, WIN_BORDER_RGB);
    surf_fill(v, X + W - WIN_BORDER, Y + WIN_BORDER, WIN_BORDER, H - 2 * WIN_BORDER, WIN_BORDER_RGB);
    surf_fill(v, X + WIN_BORDER, Y + WIN_BORDER, W - 2 * WIN_BORDER, WIN_TITLE_H,
              focused ? WIN_TITLE_RGB : WIN_TITLE_OFF_RGB);
    // The title: as many whole characters as fit before the close box.
    I32 room = W - 2 * WIN_BORDER - 6 - (w->closable ? WIN_CLOSE_SIZE + 6 : 6);
    const char* t = w->title;
    I32 tx = X + WIN_BORDER + 6, ty = Y + WIN_BORDER + (WIN_TITLE_H - FONT_HEIGHT) / 2;
    for (I32 n = room / FONT_WIDTH; n > 0 && *t; n--, tx += FONT_WIDTH)
        surf_glyph(v, tx, ty, font_utf8_next(&t), focused ? WIN_TITLE_TEXT : WIN_TITLE_OFF_TEXT, 0, FALSE);
    if (w->closable) {
        I32 bx = X + (I32)close_x(w), by = Y + (I32)close_y();
        surf_fill(v, bx, by, WIN_CLOSE_SIZE, WIN_CLOSE_SIZE, WIN_CLOSE_RGB);
        for (I32 i = 3; i <= 10; i++) {             // a white X, two pixels wide
            surf_fill(v, bx + i, by + i, 2, 1, 0xFFFFFF);
            surf_fill(v, bx + 12 - i, by + i, 2, 1, 0xFFFFFF);
        }
    }
    surf_blit(v, X + WIN_BORDER, Y + WIN_BORDER + WIN_TITLE_H, &w->content);
}

// Put the screen rectangle (x, y, w, h) together again from the windows.
static U0 compose(I32 x, I32 y, I32 w, I32 h) {
    DisplayState* d = display_state();
    if (!g.on || !d->ready) return;
    Surface v = surf_view(display_surface(), x, y, w, h);
    if (!v.px) return;
    if (x < 0) x = 0;                               // the view's (0, 0) on the screen
    if (y < 0) y = 0;
    display_begin();
    surf_fill(&v, 0, 0, v.w, v.h, DESKTOP_BG);
    for (U32 i = 0; i < s_nz; i++) draw_window(&v, &s_win[s_z[i]], x, y);
    display_end();
    g.composed_px += (U64)v.w * v.h;
}

static U0 compose_window(const Window* w) { compose(w->x, w->y, (I32)w->w, (I32)w->h); }
static U0 compose_title(const Window* w) { compose(w->x, w->y, (I32)w->w, WIN_BORDER + WIN_TITLE_H); }

// A window's contents changed: show that part, under whatever covers it.
static U0 content_damage(Surface* s, I32 x, I32 y, I32 w, I32 h) {
    const Window* win = (const Window*)s->owner;
    compose(win->x + WIN_BORDER + x, win->y + WIN_BORDER + WIN_TITLE_H + y, w, h);
}

static Bool alloc_content(Window* w) {
    U32* px = (U32*)Malloc((size_t)w->cols * FONT_WIDTH * w->rows * FONT_HEIGHT * 4);
    if (!px) return FALSE;
    if (w->content.px) Free(w->content.px);
    w->content = (Surface){ px, (I32)(w->cols * FONT_WIDTH), (I32)(w->rows * FONT_HEIGHT),
                            w->cols * FONT_WIDTH, content_damage, w };
    return TRUE;
}

// ---- windows ---------------------------------------------------------------------

U0 desktop_show(Window* w) {
    if (w && w->used) compose(w->x + WIN_BORDER, w->y + WIN_BORDER + WIN_TITLE_H, w->content.w, w->content.h);
}

U0 desktop_set_title(Window* w, const char* title) {
    StrNCpy(w->title, title, WIN_TITLE_MAX);
    w->title[WIN_TITLE_MAX - 1] = '\0';
    if (w->used) compose_title(w);
}

Bool desktop_key(U32 key) {
    if (!g.on) return FALSE;
    Window* w = desktop_window(g.focus);
    return w && w->on_key && w->on_key(w, key);
}

U0 desktop_front(U32 id) {
    Window* w = desktop_window(id);
    if (!w) return;
    U32 k = (U32)(w - s_win), at = 0;
    while (s_z[at] != k) at++;
    for (; at + 1 < s_nz; at++) s_z[at] = s_z[at + 1];
    s_z[s_nz - 1] = k;
    Window* old = desktop_window(g.focus);
    g.focus = id;
    if (old && old != w) compose_title(old);
    compose_window(w);
}

U0 desktop_move(U32 id, I32 x, I32 y) {
    Window* w = desktop_window(id);
    if (!w) return;
    clamp(w, &x, &y);
    if (x == w->x && y == w->y) return;
    I32 ox = w->x, oy = w->y;
    w->x = x;
    w->y = y;
    g.moves++;
    display_begin();                                // one pointer lift for both parts
    compose(ox, oy, (I32)w->w, (I32)w->h);
    compose_window(w);
    display_end();
}

Window* desktop_new_window(U32 cols, U32 rows, const char* title) {
    if (!g.on) return NULL;
    Window* w = NULL;
    for (U32 i = 0; i < WIN_MAX && !w; i++) if (!s_win[i].used) w = &s_win[i];
    if (!w) return NULL;
    MemSet(w, 0, sizeof(*w));
    fit(&cols, &rows);
    set_size(w, cols, rows);
    if (!alloc_content(w)) return NULL;
    w->used = TRUE;
    w->id = g.next_id++;
    w->closable = TRUE;
    StrNCpy(w->title, title, WIN_TITLE_MAX);
    w->title[WIN_TITLE_MAX - 1] = '\0';
    U32 k = g.created++ % 8;                        // cascade
    w->x = 120 + 32 * (I32)k;
    w->y = 80 + 32 * (I32)k;
    clamp(w, &w->x, &w->y);
    SurfaceDamageFn dmg = w->content.damage;        // fill without composing piece by piece
    w->content.damage = NULL;
    surf_fill(&w->content, 0, 0, w->content.w, w->content.h, WIN_PAPER);
    w->content.damage = dmg;
    s_z[s_nz++] = (U32)(w - s_win);
    g.count++;
    desktop_front(w->id);
    return w;
}

Bool desktop_close(U32 id) {
    Window* w = desktop_window(id);
    if (!w || !w->closable) return FALSE;
    U32 k = (U32)(w - s_win), at = 0;
    while (s_z[at] != k) at++;
    for (; at + 1 < s_nz; at++) s_z[at] = s_z[at + 1];
    s_nz--;
    g.count--;
    if (g.dragging && g.drag_id == id) g.dragging = FALSE;
    I32 ox = w->x, oy = w->y, ow = (I32)w->w, oh = (I32)w->h;
    if (w->on_close) w->on_close(w);
    Free(w->content.px);
    MemSet(w, 0, sizeof(*w));
    display_begin();
    if (g.focus == id && top()) {
        g.focus = top()->id;
        compose_title(top());
    }
    compose(ox, oy, ow, oh);
    display_end();
    return TRUE;
}

// The console's window: as large as wanted and as fits the mode, then
// everything clamped and the whole screen composed.
static U0 layout(U0) {
    DisplayState* d = display_state();
    Window* con = desktop_window(0);
    if (!d->ready || !con) return;
    U32 cols = WIN_COLS, rows = WIN_ROWS;
    fit(&cols, &rows);
    if (cols != con->cols || rows != con->rows || !con->content.px) {
        set_size(con, cols, rows);
        alloc_content(con);
    }
    for (U32 i = 0; i < WIN_MAX; i++) if (s_win[i].used) clamp(&s_win[i], &s_win[i].x, &s_win[i].y);
    g.dragging = FALSE;
    display_begin();
    terminal_set_area(&con->content, 0, 0, con->cols, con->rows, TRUE);
    compose(0, 0, (I32)d->width, (I32)d->height);
    display_end();
}

U0 desktop_enable(Bool on) {
    if (on && !display_state()->ready) return;
    g.on = on;
    g.dragging = FALSE;
    terminal_set_layout(on ? layout : NULL);
}

U0 desktop_init(U0) {
    MemSet(&g, 0, sizeof(g));
    MemSet(s_win, 0, sizeof(s_win));
    Window* con = &s_win[0];
    con->used = TRUE;
    con->id = 0;
    con->x = WIN_X0;
    con->y = WIN_Y0;
    StrNCpy(con->title, WIN_TITLE, WIN_TITLE_MAX);
    s_z[0] = 0;
    s_nz = 1;
    g.count = 1;
    g.next_id = 1;
    g.focus = 0;
    desktop_enable(TRUE);
}

// ---- pointer ---------------------------------------------------------------------

static Window* window_at(I32 x, I32 y) {
    for (U32 i = s_nz; i-- > 0;) {
        Window* w = &s_win[s_z[i]];
        if (x >= w->x && y >= w->y && x < w->x + (I32)w->w && y < w->y + (I32)w->h) return w;
    }
    return NULL;
}

U0 desktop_pointer(I32 x, I32 y, U32 buttons) {
    Bool left = (buttons & 1) != 0, was = (s_last_buttons & 1) != 0;
    s_last_buttons = buttons;
    if (!g.on) return;
    if (left && !was) {
        Window* w = window_at(x, y);
        if (!w) return;
        if (w != top() || g.focus != w->id) desktop_front(w->id);
        I32 rx = x - w->x, ry = y - w->y;
        if (w->closable && rx >= (I32)close_x(w) && rx < (I32)close_x(w) + WIN_CLOSE_SIZE &&
            ry >= (I32)close_y() && ry < (I32)close_y() + WIN_CLOSE_SIZE) {
            desktop_close(w->id);
        } else if (rx >= WIN_BORDER && rx < (I32)w->w - WIN_BORDER &&
                   ry >= WIN_BORDER && ry < WIN_BORDER + WIN_TITLE_H) {
            g.dragging = TRUE;
            g.drag_id = w->id;
            g.grab_dx = rx;
            g.grab_dy = ry;
            g.drags++;
        } else if (rx >= WIN_BORDER && rx < (I32)w->w - WIN_BORDER &&
                   ry >= WIN_BORDER + WIN_TITLE_H && ry < (I32)w->h - WIN_BORDER) {
            // In the contents: the program's click; a second press soon
            // after, close by, in the same window, is a double click.
            I32 cx = rx - WIN_BORDER, cy = ry - WIN_BORDER - WIN_TITLE_H;
            U64 now = timer_get_uptime_ms();
            Bool dbl = s_click_id == w->id && now - s_click_ms <= DOUBLE_CLICK_MS &&
                       cx - s_click_x <= 4 && s_click_x - cx <= 4 && cy - s_click_y <= 4 && s_click_y - cy <= 4;
            s_click_id = dbl ? 0xFFFFFFFF : w->id;
            s_click_x = cx;
            s_click_y = cy;
            s_click_ms = now;
            if (w->on_click) w->on_click(w, cx, cy, dbl);
        }
    } else if (!left) {
        g.dragging = FALSE;
    } else if (g.dragging) {
        desktop_move(g.drag_id, x - g.grab_dx, y - g.grab_dy);
    }
}

// ---- console: `desktop`, `window` -----------------------------------------------------

static const char* next_word(const char* p) {
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    return p;
}

static U32 parse_hex(const char* p) {
    U32 v = 0;
    for (; *p; p++) {
        U8 c = (U8)*p, d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d;
    }
    return v & 0xFFFFFF;
}

static U0 list_windows(U0) {
    for (U32 i = s_nz; i-- > 0;) {
        const Window* w = &s_win[s_z[i]];
        kprintf("  %u: \"%s\" at %d,%d, %ux%u pixels, %ux%u characters%s%s\n", (U64)w->id, w->title,
                (I64)w->x, (I64)w->y, (U64)w->w, (U64)w->h, (U64)w->cols, (U64)w->rows,
                w->id == g.focus ? ", focused" : "", i == s_nz - 1 ? ", top" : "");
    }
}

static Window* window_arg(const char* p) {
    Window* w = desktop_window((U32)shell_parse_uint(p));
    if (!w) kprintf("window: no window %u\n", shell_parse_uint(p));
    return w;
}

static Bool window_run(const char* cmd) {
    if (!g.on) {
        kprintf("window: the desktop is off\n");
        return TRUE;
    }
    if (StrCmp(cmd, "window") == 0 || StrCmp(cmd, "window list") == 0) {
        kprintf("Windows: %u open, top first\n", (U64)g.count);
        list_windows();
    } else if (shell_is_prefix("window new ", cmd)) {
        const char* p = cmd + 11;
        U32 cols = (U32)shell_parse_uint(p);
        p = next_word(p);
        U32 rows = (U32)shell_parse_uint(p);
        p = next_word(p);
        Window* w = desktop_new_window(cols, rows, *p ? p : "Window");
        if (w) kprintf("[WINDOW] %u \"%s\" at %d,%d, %ux%u characters\n", (U64)w->id, w->title,
                       (I64)w->x, (I64)w->y, (U64)w->cols, (U64)w->rows);
        else kprintf("window: cannot open another window (at most %u, or out of memory)\n", (U64)WIN_MAX);
    } else if (shell_is_prefix("window text ", cmd)) {
        // window text <id> <col> <row> <rrggbb> <text>
        const char* p = cmd + 12;
        Window* w = window_arg(p);
        if (!w) return TRUE;
        p = next_word(p);
        I32 col = (I32)shell_parse_uint(p);
        p = next_word(p);
        I32 row = (I32)shell_parse_uint(p);
        p = next_word(p);
        U32 fg = parse_hex(p);
        p = next_word(p);
        if (w->id == 0) kprintf("window: the console's window holds the console\n");
        else surf_text(&w->content, col * FONT_WIDTH, row * FONT_HEIGHT, p, fg);
    } else if (shell_is_prefix("window front ", cmd)) {
        Window* w = window_arg(cmd + 13);
        if (w) desktop_front(w->id);
    } else if (shell_is_prefix("window move ", cmd)) {
        const char* p = cmd + 12;
        Window* w = window_arg(p);
        if (!w) return TRUE;
        p = next_word(p);
        I32 x = (I32)shell_parse_uint(p);
        I32 y = (I32)shell_parse_uint(next_word(p));
        desktop_move(w->id, x, y);
        kprintf("[WINDOW] %u at %d,%d\n", (U64)w->id, (I64)w->x, (I64)w->y);
    } else if (shell_is_prefix("window close ", cmd)) {
        U32 id = (U32)shell_parse_uint(cmd + 13);
        if (id == 0) kprintf("window: the console's window cannot be closed (desktop off hides the desktop)\n");
        else if (!desktop_close(id)) kprintf("window: no window %u\n", (U64)id);
        else kprintf("[WINDOW] %u closed\n", (U64)id);
    } else {
        kprintf("usage: window [list] | window new <cols> <rows> <title> | window text <id> <col> <row> <rrggbb> <text>\n"
                "       window front <id> | window move <id> <x> <y> | window close <id>\n");
    }
    return TRUE;
}

static inline U64 instret(U0) { U64 v; __asm__ __volatile__("csrr %0, minstret" : "=r"(v)); return v; }

// Retired instructions of 16 compositions of the whole screen and of 16
// scrolls of the console window's contents by 0 lines (every row copied onto
// itself, the contents unchanged). Only meaningful under QEMU -icount with one
// hart (-smp 1), or on a board: without -icount QEMU's minstret follows the
// clock, and with several harts it counts the instructions of all of them.
static U0 desktop_bench(U0) {
    DisplayState* d = display_state();
    Window* con = desktop_window(0);
    if (!g.on || !d->ready || !con) { kprintf("desktop bench: the desktop is off\n"); return; }
    U64 t0 = instret();
    for (U32 i = 0; i < 16; i++) compose(0, 0, (I32)d->width, (I32)d->height);
    U64 tc = (instret() - t0) / 16;
    SurfaceDamageFn dmg = con->content.damage;
    con->content.damage = 0;                        // the copies only, no composing
    t0 = instret();
    for (U32 i = 0; i < 16; i++) surf_scroll(&con->content, 0, 0, con->content.w, con->content.h, 0, 0);
    U64 ts = (instret() - t0) / 16;
    con->content.damage = dmg;
    U64 px = (U64)d->width * d->height, cpx = (U64)con->content.w * con->content.h;
    kprintf("desktop bench: compose %ux%u: %u instructions (%u.%02u per pixel)\n", (U64)d->width, (U64)d->height,
            tc, tc / px, tc * 100 / px % 100);
    kprintf("desktop bench: scroll the console contents %ux%u: %u instructions (%u.%02u per pixel)\n",
            (U64)con->content.w, (U64)con->content.h, ts, ts / cpx, ts * 100 / cpx % 100);
    kprintf("desktop bench: retired instructions (minstret): meaningful only under QEMU -icount -smp 1, or on a board\n");
}

static Bool desktop_run(const char* cmd) {
    if (shell_is_prefix("window", cmd) && (cmd[6] == ' ' || cmd[6] == '\0')) return window_run(cmd);
    if (StrCmp(cmd, "desktop") == 0 || StrCmp(cmd, "desktop status") == 0) {
        Window* con = desktop_window(0);
        if (!g.on) kprintf("Desktop: off (the console takes the whole screen)\n");
        else kprintf("Desktop: on, console window at %d,%d, %ux%u pixels, %ux%u characters; "
                     "%u moves, %u drags; %u window(s), focus %u\n", (I64)con->x, (I64)con->y,
                     (U64)con->w, (U64)con->h, (U64)con->cols, (U64)con->rows, g.moves, g.drags,
                     (U64)g.count, (U64)g.focus);
    } else if (StrCmp(cmd, "desktop on") == 0) {
        if (!display_state()->ready) kprintf("desktop: no usable display\n");
        desktop_enable(TRUE);
    } else if (StrCmp(cmd, "desktop off") == 0) {
        desktop_enable(FALSE);
    } else if (shell_is_prefix("desktop move ", cmd)) {
        const char* p = cmd + 13;
        I32 x = (I32)shell_parse_uint(p);
        I32 y = (I32)shell_parse_uint(next_word(p));
        if (!g.on) kprintf("desktop: off\n");
        else {
            desktop_move(0, x, y);
            kprintf("[DESKTOP] window at %d,%d\n", (I64)desktop_window(0)->x, (I64)desktop_window(0)->y);
        }
    } else if (StrCmp(cmd, "desktop bench") == 0) {
        desktop_bench();
    } else if (shell_is_prefix("desktop", cmd) && (cmd[7] == ' ' || cmd[7] == '\0')) {
        kprintf("usage: desktop [status] | desktop on | desktop off | desktop move <x> <y> | desktop bench\n");
    } else {
        return FALSE;
    }
    return TRUE;
}

static U0 desktop_help(U0) {
    kprintf("  Desktop (windows: drag by the title bar, close with the box, a press brings to the front)\n");
    kprintf("    desktop [status]   desktop on   desktop off   desktop move <x> <y>\n");
    kprintf("    window [list]   window new <cols> <rows> <title>   window text <id> <col> <row> <rrggbb> <text>\n");
    kprintf("    window front <id>   window move <id> <x> <y>   window close <id>\n");
}

static const ShellExtension s_desktop_shell = { desktop_run, desktop_help, NULL, NULL };

U0 desktop_shell_register(U0) {
    shell_register(&s_desktop_shell);
}
