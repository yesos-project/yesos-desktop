// ============================================================================
// YesOS Desktop - the console on the screen (see terminal.h).
// ============================================================================

#include "terminal.h"
#include "display.h"
#include "surface.h"
#include "font.h"
#include "ports.h"
#include "kprintf.h"
#include "memory.h"
#include "shell.h"

// ANSI SGR 30-37 (the core prints directory names in cyan and green).
static const U32 s_palette[8] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, TERMINAL_FG,
};

static TerminalState g;
static U32  s_fg = TERMINAL_FG;
static Bool s_wrap_pending;         // the last column was written: wrap on the next character
static Bool s_busy;
static U8   s_esc;                  // 0 none, 1 after ESC, 2 inside "ESC ["
static U32  s_params[4], s_nparams;
static U32  s_utf8_cp, s_utf8_left;
static U32  s_mode_serial;          // display mode the layout was made for
static TerminalLayoutFn s_layout;   // places the terminal (the desktop's window), or NULL: full screen
static Surface* s_surf;             // where the text goes (the screen or the window's contents)

TerminalState* terminal_state(U0) { return &g; }

static U0 cursor(Bool show) {
    if (!g.on || !s_surf) return;
    surf_fill(s_surf, (I32)(g.x + g.col * FONT_WIDTH), (I32)(g.y + g.row * FONT_HEIGHT + FONT_HEIGHT - 2),
              FONT_WIDTH, 2, show ? s_fg : TERMINAL_BG);
}

static U0 clear(U0) {
    g.col = g.row = 0;
    s_wrap_pending = FALSE;
    if (g.on && s_surf) surf_fill(s_surf, (I32)g.x, (I32)g.y, (I32)(g.cols * FONT_WIDTH), (I32)(g.rows * FONT_HEIGHT), TERMINAL_BG);
}

U0 terminal_set_area(Surface* s, U32 x, U32 y, U32 cols, U32 rows, Bool clear_it) {
    Bool other = s != s_surf;
    s_surf = s;
    g.x = x;
    g.y = y;
    if (clear_it || other || cols != g.cols || rows != g.rows) {
        g.cols = cols ? cols : 1;
        g.rows = rows ? rows : 1;
        display_begin();
        clear();
        cursor(TRUE);
        display_end();
    }
}

U0 terminal_set_layout(TerminalLayoutFn fn) {
    s_layout = fn;
    terminal_relayout();
}

U0 terminal_relayout(U0) {
    DisplayState* d = display_state();
    s_mode_serial = d->mode_serial;
    if (!d->ready) return;
    if (s_layout) s_layout();
    else terminal_set_area(display_surface(), 0, 0, d->width / FONT_WIDTH, d->height / FONT_HEIGHT, TRUE);
}

static U0 newline(U0) {
    g.col = 0;
    s_wrap_pending = FALSE;
    if (g.row + 1 < g.rows) {
        g.row++;
    } else {
        surf_scroll(s_surf, (I32)g.x, (I32)g.y, (I32)(g.cols * FONT_WIDTH), (I32)(g.rows * FONT_HEIGHT),
                    FONT_HEIGHT, TERMINAL_BG);
        g.scrolls++;
    }
}

static U0 put_glyph(U32 cp) {
    if (s_wrap_pending) newline();
    surf_glyph(s_surf, (I32)(g.x + g.col * FONT_WIDTH), (I32)(g.y + g.row * FONT_HEIGHT), cp, s_fg, TERMINAL_BG, TRUE);
    g.chars++;
    if (g.col + 1 < g.cols) g.col++;
    else s_wrap_pending = TRUE;
}

static U0 csi(U8 final) {
    if (final == 'm') {
        if (s_nparams == 0) s_fg = TERMINAL_FG;
        for (U32 i = 0; i < s_nparams; i++) {
            if (s_params[i] == 0 || s_params[i] == 39) s_fg = TERMINAL_FG;
            else if (s_params[i] >= 30 && s_params[i] <= 37) s_fg = s_palette[s_params[i] - 30];
        }
    } else if (final == 'J' && s_nparams >= 1 && s_params[0] == 2) {
        clear();
    } else if (final == 'H') {
        g.col = g.row = 0;                              // only "home" is used
        s_wrap_pending = FALSE;
    }
}

static U0 put(U8 b) {
    if (s_esc == 1) {
        s_esc = b == '[' ? 2 : 0;
        s_nparams = 0;
        MemSet(s_params, 0, sizeof(s_params));
        return;
    }
    if (s_esc == 2) {
        if (b >= '0' && b <= '9') {
            if (s_nparams == 0) s_nparams = 1;
            if (s_nparams <= 4) s_params[s_nparams - 1] = s_params[s_nparams - 1] * 10 + (b - '0');
        } else if (b == ';') {
            if (s_nparams == 0) s_nparams = 1;
            if (s_nparams < 4) s_nparams++;
        } else if (b >= 0x40 && b <= 0x7E) {
            if (s_nparams > 4) s_nparams = 4;
            csi(b);
            s_esc = 0;
        }
        return;
    }
    if (s_utf8_left) {
        if ((b & 0xC0) == 0x80) {
            s_utf8_cp = (s_utf8_cp << 6) | (b & 0x3F);
            if (--s_utf8_left == 0) put_glyph(s_utf8_cp);
            return;
        }
        s_utf8_left = 0;
        put_glyph('?');                                 // truncated sequence; b starts afresh
    }
    if (b >= 0x80) {
        if (b >= 0xC0 && b < 0xF8) {
            s_utf8_left = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
            s_utf8_cp = b & (0x3F >> s_utf8_left);
        } else {
            put_glyph('?');
        }
        return;
    }
    switch (b) {
    case 0x1B: s_esc = 1; break;
    case '\n': newline(); break;
    case '\r': g.col = 0; s_wrap_pending = FALSE; break;
    case '\b':
        if (s_wrap_pending) s_wrap_pending = FALSE;
        else if (g.col > 0) g.col--;
        else if (g.row > 0) { g.row--; g.col = g.cols - 1; }
        break;
    case '\t': do put_glyph(' '); while (g.col % 8 && !s_wrap_pending); break;
    default:
        if (b >= 32 && b < 127) put_glyph(b);
        break;
    }
}

static U0 hook(char c) {
    if (!g.on) return;
    if (smp_hart_id() != 0 || s_busy || !display_state()->ready || !s_surf) {
        g.dropped++;
        return;
    }
    s_busy = TRUE;
    DisplayState* d = display_state();
    display_begin();
    if (d->mode_serial != s_mode_serial) terminal_relayout();   // new mode: lay out again
    cursor(FALSE);
    put((U8)c);
    cursor(TRUE);
    display_end();
    s_busy = FALSE;
}

U0 terminal_enable(Bool on) {
    if (on && !display_state()->ready) return;
    g.on = on;
    if (on) {
        s_fg = TERMINAL_FG;
        s_esc = 0;
        s_utf8_left = 0;
        terminal_relayout();
    }
}

U0 terminal_init(U0) {
    MemSet(&g, 0, sizeof(g));
    kprintf_set_hook(hook);
    terminal_enable(TRUE);
}

// ---- console: `term` -------------------------------------------------------------------

static Bool term_run(const char* cmd) {
    if (StrCmp(cmd, "term") == 0 || StrCmp(cmd, "term status") == 0) {
        kprintf("Terminal: %s, %ux%u characters at %u,%u, cursor at %u,%u; %u characters drawn, %u scrolls, "
                "%u dropped\n", g.on ? "on" : "off", (U64)g.cols, (U64)g.rows, (U64)g.x, (U64)g.y,
                (U64)g.col, (U64)g.row, g.chars, g.scrolls, g.dropped);
    } else if (StrCmp(cmd, "term on") == 0) {
        if (!display_state()->ready) kprintf("term: no usable display\n");
        terminal_enable(TRUE);
    } else if (StrCmp(cmd, "term off") == 0) {
        terminal_enable(FALSE);
        kprintf("[TERM] off: the screen is left as it is\n");
    } else if (StrCmp(cmd, "term clear") == 0) {
        if (g.on) { display_begin(); clear(); cursor(TRUE); display_end(); }
    } else if (shell_is_prefix("term", cmd) && (cmd[4] == ' ' || cmd[4] == '\0')) {
        kprintf("usage: term [status] | term on | term off | term clear\n");
    } else {
        return FALSE;
    }
    return TRUE;
}

static U0 term_help(U0) {
    kprintf("  Terminal on the screen\n");
    kprintf("    term [status]   term on   term off   term clear\n");
}

static const ShellExtension s_term_shell = { term_run, term_help, NULL, NULL };

U0 terminal_shell_register(U0) {
    shell_register(&s_term_shell);
}
