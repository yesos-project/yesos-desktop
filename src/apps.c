// ============================================================================
// YesOS Desktop - apps in windows: a HolyQ program runs in a PMP cell (an
// isolated hart, kernel/cell.c in the core) and its 320x240 canvas is shown
// in a window of its own. The app never touches the screen: at each
// gfx_present the core hands its canvas to frame_hook, which copies it into
// the window's contents. Keys and presses in the focused app window are
// queued for the app, which reads them with input(). Closing the window
// stops the app; when the app ends on its own the window stays, with its
// last frame, until it is closed.
// ============================================================================

#include "apps.h"
#include "cell.h"
#include "desktop.h"
#include "files.h"
#include "kprintf.h"
#include "memory.h"
#include "shell.h"
#include "yesmath.h"

typedef struct {
    U32  cell;
    Bool running;
    char name[24];
} AppWin;

static U32 s_win_of_cell[CELL_MAX];         // window id showing each cell's app, 0 if none

static Window* window_of_cell(U32 cell) {
    if (cell >= CELL_MAX || !s_win_of_cell[cell]) return NULL;
    return desktop_window(s_win_of_cell[cell]);
}

static U0 frame_hook(U32 cell, const U32* canvas, U32 frame) {
    (U0)frame;
    Window* w = window_of_cell(cell);
    if (!w) return;
    ym_memcpy(w->content.px, canvas, (U64)CELL_CANVAS_W * CELL_CANVAS_H * 4);   // pitch == width
    desktop_show(w);
}

static U0 end_hook(U32 cell, const CellResult* r) {
    Window* w = window_of_cell(cell);
    s_win_of_cell[cell] = 0;
    if (!w) return;                             // closed by the user: already gone
    AppWin* a = (AppWin*)w->data;
    a->running = FALSE;
    char title[WIN_TITLE_MAX];
    const char* how = r->code == CELL_RESULT_OK ? "finished" : r->code == CELL_RESULT_FAULT ? "fault"
                    : r->code == CELL_RESULT_TIMEOUT ? "watchdog" : "stopped";
    ksnprintf(title, sizeof(title), "%s (%s)", a->name, how);
    desktop_set_title(w, title);
}

static Bool app_key(Window* w, U32 key) {
    AppWin* a = (AppWin*)w->data;
    if (!a->running) return FALSE;
    U32 ev = 0;
    switch (key) {
        case WKEY_ENTER:     ev = 10; break;
        case WKEY_BACKSPACE: ev = 8; break;
        case WKEY_ESCAPE:    ev = 27; break;
        case WKEY_UP:        ev = CELL_IN_UP; break;
        case WKEY_DOWN:      ev = CELL_IN_DOWN; break;
        case WKEY_LEFT:      ev = CELL_IN_LEFT; break;
        case WKEY_RIGHT:     ev = CELL_IN_RIGHT; break;
        default:             ev = key < 0x110000 ? key : 0; break;
    }
    if (!ev) return FALSE;
    cell_app_input(a->cell, ev);                // a full queue drops the event
    return TRUE;
}

static U0 app_click(Window* w, I32 x, I32 y, Bool double_click) {
    (U0)double_click;
    AppWin* a = (AppWin*)w->data;
    if (a->running && x >= 0 && y >= 0 && x < CELL_CANVAS_W && y < CELL_CANVAS_H)
        cell_app_input(a->cell, CELL_IN_PRESS | ((U32)x << 16) | (U32)y);
}

static U0 app_close(Window* w) {
    AppWin* a = (AppWin*)w->data;
    if (a->running) cell_app_stop(a->cell);
    if (a->cell < CELL_MAX && s_win_of_cell[a->cell] == w->id) s_win_of_cell[a->cell] = 0;
    Free(a);
    w->data = NULL;
}

Window* apps_run(const char* path, char* why, U32 why_max) {
    size_t size = 0;
    U8* data = files_read(path, &size);
    if (!data) {
        StrNCpy(why, "no such file", why_max);
        return NULL;
    }
    char* src = (char*)Malloc(size + 1);
    AppWin* a = (AppWin*)Malloc(sizeof(AppWin));
    if (!src || !a) {
        if (src) Free(src);
        if (a) Free(a);
        Free(data);
        StrNCpy(why, "out of memory", why_max);
        return NULL;
    }
    MemCpy(src, data, size);
    src[size] = '\0';
    Free(data);
    const char* base = path;
    for (const char* p = path; *p; p++) if (*p == '/') base = p + 1;
    MemSet(a, 0, sizeof(*a));
    StrNCpy(a->name, base, sizeof(a->name) - 1);

    CellResult r;
    I32 cell = cell_app_start(src, a->name, APPS_TIMEOUT_MS, &r);
    Free(src);
    if (cell < 0) {
        StrNCpy(why, r.reason[0] ? r.reason : "no cell available", why_max);
        Free(a);
        return NULL;
    }
    char title[WIN_TITLE_MAX];
    ksnprintf(title, sizeof(title), "%s - cell %u", a->name, (U32)(CELL_FIRST_HART + cell));
    Window* w = desktop_new_window(CELL_CANVAS_W / 8, CELL_CANVAS_H / 16, title);
    if (!w) {
        cell_app_stop((U32)cell);
        StrNCpy(why, "no room for another window", why_max);
        Free(a);
        return NULL;
    }
    a->cell = (U32)cell;
    a->running = TRUE;
    ym_fill32(w->content.px, 0x000000, (U64)CELL_CANVAS_W * CELL_CANVAS_H);
    w->data = a;
    w->on_key = app_key;
    w->on_click = app_click;
    w->on_close = app_close;
    s_win_of_cell[cell] = w->id;
    desktop_show(w);
    return w;
}

static Bool apps_run_cmd(const char* cmd) {
    if (StrCmp(cmd, "app") == 0 || StrCmp(cmd, "app list") == 0) {
        U32 n = 0;
        for (U32 k = 0; k < CELL_MAX; k++) {
            Window* w = window_of_cell(k);
            if (!w) continue;
            AppWin* a = (AppWin*)w->data;
            kprintf("  cell %u: %s in window %u\n", (U32)(CELL_FIRST_HART + k), a->name, w->id);
            n++;
        }
        if (!n) kprintf("No app is running. %u cell(s) available.\n", cell_available_count());
    } else if (shell_is_prefix("app run ", cmd)) {
        const char* p = cmd + 8;
        while (*p == ' ') p++;
        char why[96];
        Window* w = *p ? apps_run(p, why, sizeof(why)) : NULL;
        if (w) kprintf("[APP] %s running in window %u\n", ((AppWin*)w->data)->name, w->id);
        else kprintf("app: %s\n", *p ? why : "usage: app run <file.HQ>");
    } else if (shell_is_prefix("app", cmd) && (cmd[3] == ' ' || cmd[3] == '\0')) {
        kprintf("usage: app [list] | app run <file.HQ>\n");
    } else {
        return FALSE;
    }
    return TRUE;
}

static U0 apps_help(U0) {
    kprintf("  Apps (HolyQ programs in isolated PMP cells, each in its own window)\n");
    kprintf("    app [list]   app run <file.HQ>   (or Enter on a .HQ file in the file browser)\n");
}

static const ShellExtension s_apps_shell = { apps_run_cmd, apps_help, NULL, NULL };

U0 apps_init(U0) {
    cell_set_app_hooks(frame_hook, end_hook);
    shell_register(&s_apps_shell);
}
