// ============================================================================
// YesOS Desktop - file browser and text viewer windows (see files.h).
// ============================================================================

#include "files.h"
#include "desktop.h"
#include "surface.h"
#include "hda.h"
#include "vfs.h"
#include "ramfs.h"
#include "redsea_eml.h"
#include "font.h"
#include "kprintf.h"
#include "memory.h"
#include "shell.h"

typedef struct {
    char     path[VFS_MAX_PATH];
    VFSEntry e[FILES_MAX];
    U32      n;                           // entries from the VFS
    Bool     up;                          // ".." shown first
    U32      sel, top;                    // in the shown list (".." included)
    char     msg[96];                     // status line; empty: path and count
    U8       ask;                         // ASK_*: the status line is asking something
    char     subject[VFS_MAX_FILENAME];   // the entry the question is about
    Bool     subject_dir;
    char     input[96];                   // the name being typed
} Browser;

enum { ASK_NONE, ASK_DELETE, ASK_RENAME, ASK_COPY, ASK_MKDIR };

typedef struct {
    char  name[VFS_MAX_FILENAME];
    U8*   data;
    U32   size;
    U32*  line;                           // offset where each line starts
    U32   lines, top;
} Viewer;

// ---- drawing helpers -------------------------------------------------------------------

// Up to max code points of s at character cell (col, row).
static U0 cells(Surface* s, U32 col, U32 row, const char* t, U32 max, U32 fg) {
    I32 x = (I32)(col * FONT_WIDTH), y = (I32)(row * FONT_HEIGHT);
    for (; *t && max; max--, x += FONT_WIDTH) surf_glyph(s, x, y, font_utf8_next(&t), fg, 0, FALSE);
}

static U32 utf8_len(const char* t) {
    U32 n = 0;
    while (*t) { font_utf8_next(&t); n++; }
    return n;
}

// Draw everything with the damage callback off, then show it at once.
static U0 begin(Window* w, SurfaceDamageFn* saved) {
    *saved = w->content.damage;
    w->content.damage = NULL;
    surf_fill(&w->content, 0, 0, w->content.w, w->content.h, WIN_PAPER);
}

static U0 finish(Window* w, SurfaceDamageFn saved) {
    w->content.damage = saved;
    desktop_show(w);
}

static U0 status_line(Window* w, const char* text) {
    I32 y = (I32)((w->rows - 1) * FONT_HEIGHT);
    surf_fill(&w->content, 0, y, w->content.w, FONT_HEIGHT, FILES_STATUS_BG);
    cells(&w->content, 1, w->rows - 1, text, w->cols - 2, FILES_STATUS_TEXT);
}

// ---- browser ----------------------------------------------------------------------------

static U32 shown(const Browser* b) { return b->n + (b->up ? 1 : 0); }

static U0 browser_draw(Window* w) {
    Browser* b = (Browser*)w->data;
    SurfaceDamageFn saved;
    begin(w, &saved);
    U32 list_rows = w->rows - 1;
    for (U32 r = 0; r < list_rows && b->top + r < shown(b); r++) {
        U32 i = b->top + r;
        Bool sel = i == b->sel;
        U32 fg = sel ? FILES_SEL_TEXT : FILES_TEXT;
        if (sel) surf_fill(&w->content, 0, (I32)(r * FONT_HEIGHT), w->content.w, FONT_HEIGHT, FILES_SEL_BG);
        if (b->up && i == 0) {
            cells(&w->content, 1, r, "..", w->cols - 2, fg);
            continue;
        }
        const VFSEntry* e = &b->e[i - (b->up ? 1 : 0)];
        char size[24] = "";
        if (e->type != VFS_TYPE_DIR) ksnprintf(size, sizeof(size), "%u", e->size);
        U32 slen = (U32)StrLen(size);
        U32 room = w->cols - 2 - (slen ? slen + 1 : 0);
        cells(&w->content, 1, r, e->name, room, fg);
        if (e->type == VFS_TYPE_DIR) {
            U32 nl = utf8_len(e->name);
            if (nl < room) cells(&w->content, 1 + nl, r, "/", 1, fg);
        } else {
            cells(&w->content, w->cols - 1 - slen, r, size, slen, fg);
        }
    }
    char st[VFS_MAX_PATH + 160];
    if (b->ask == ASK_DELETE) ksnprintf(st, sizeof(st), "Delete %s? Enter: yes, Esc: no", b->subject);
    else if (b->ask == ASK_RENAME) ksnprintf(st, sizeof(st), "Rename %s to: %s_", b->subject, b->input);
    else if (b->ask == ASK_COPY) ksnprintf(st, sizeof(st), "Copy %s to: %s_", b->subject, b->input);
    else if (b->ask == ASK_MKDIR) ksnprintf(st, sizeof(st), "New folder: %s_", b->input);
    else if (b->msg[0]) StrNCpy(st, b->msg, sizeof(st));
    else ksnprintf(st, sizeof(st), "%s  %u entries", b->path, (U64)b->n);
    status_line(w, st);
    finish(w, saved);
}

static U0 browser_load(Window* w, const char* path) {
    Browser* b = (Browser*)w->data;
    StrNCpy(b->path, path, sizeof(b->path));
    b->path[sizeof(b->path) - 1] = '\0';
    b->n = vfs_list_dir(b->path, b->e, FILES_MAX);
    b->up = StrCmp(b->path, "/") != 0;
    b->sel = b->top = 0;
    b->msg[0] = '\0';
    char title[WIN_TITLE_MAX];
    ksnprintf(title, sizeof(title), "Files: %s", b->path);
    desktop_set_title(w, title);
    browser_draw(w);
}

static U0 join(char* out, U32 max, const char* dir, const char* name) {
    if (StrCmp(dir, "/") == 0) ksnprintf(out, max, "/%s", name);
    else ksnprintf(out, max, "%s/%s", dir, name);
}

static U0 parent(char* path) {
    U32 n = (U32)StrLen(path);
    while (n > 1 && path[n - 1] != '/') n--;
    path[n > 1 ? n - 1 : 1] = '\0';
}

static Bool ends_with(const char* s, const char* suffix) {     // ASCII, any case
    U32 n = (U32)StrLen(s), m = (U32)StrLen(suffix);
    if (m > n) return FALSE;
    for (U32 i = 0; i < m; i++) {
        char a = s[n - m + i], c = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != c) return FALSE;
    }
    return TRUE;
}

static Window* viewer_open(const char* path, const char* name, char* why, U32 why_max);
static Bool browser_key(Window* w, U32 key);
static U8* read_listed(const char* path, size_t* size);
static U0 browser_select(Window* w, U32 i);

// A sound started from a browser has ended: say so in that browser, if it
// is still open.
static U0 browser_audio_done(const char* path, U64 frames, Bool stopped, U0* ctx) {
    Window* w = desktop_window((U32)(uintptr_t)ctx);
    if (!w || w->on_key != browser_key) return;
    Browser* b = (Browser*)w->data;
    const char* name = path;
    for (const char* q = path; *q; q++) if (*q == '/') name = q + 1;
    if (stopped) ksnprintf(b->msg, sizeof(b->msg), "%s: stopped after %u frames", name, frames);
    else ksnprintf(b->msg, sizeof(b->msg), "%s: played, %u frames", name, frames);
    browser_draw(w);
}

static U0 browser_open(Window* w) {
    Browser* b = (Browser*)w->data;
    if (shown(b) == 0) return;
    if (b->up && b->sel == 0) {
        char p[VFS_MAX_PATH];
        StrNCpy(p, b->path, sizeof(p));
        parent(p);
        browser_load(w, p);
        return;
    }
    const VFSEntry* e = &b->e[b->sel - (b->up ? 1 : 0)];
    char p[VFS_MAX_PATH];
    join(p, sizeof(p), b->path, e->name);
    if (e->type == VFS_TYPE_DIR) {
        browser_load(w, p);
    } else if (ends_with(e->name, ".wav")) {
        // In the background: the browser and the rest of the desktop keep working.
        const char* why = audio_start_file(p, browser_audio_done, (U0*)(uintptr_t)w->id);
        if (why) ksnprintf(b->msg, sizeof(b->msg), "%s: %s", e->name, why);
        else ksnprintf(b->msg, sizeof(b->msg), "Playing %s", e->name);
        browser_draw(w);
    } else {
        char why[64];
        if (!viewer_open(p, e->name, why, sizeof(why))) {
            ksnprintf(b->msg, sizeof(b->msg), "%s: %s", e->name, why);
            browser_draw(w);
        }
    }
}

static U0 browser_select(Window* w, U32 i) {
    Browser* b = (Browser*)w->data;
    U32 total = shown(b), list_rows = w->rows - 1;
    if (total == 0) return;
    if (i >= total) i = total - 1;
    b->sel = i;
    if (b->sel < b->top) b->top = b->sel;
    if (b->sel >= b->top + list_rows) b->top = b->sel - list_rows + 1;
    b->msg[0] = '\0';
    browser_draw(w);
}

// ---- copy, rename, delete, new folder ---------------------------------------------------

static VFSEntry s_tmp[FILES_MAX];          // a directory's listing, to see if it is empty

// After an operation: the directory listed again, `select` selected if it
// is there, the result in the status line.
static U0 reload(Window* w, const char* select) {
    Browser* b = (Browser*)w->data;
    char msg[96];
    StrNCpy(msg, b->msg, sizeof(msg));
    U32 old = b->sel;
    char path[VFS_MAX_PATH];
    StrNCpy(path, b->path, sizeof(path));
    browser_load(w, path);
    U32 i = old < shown(b) ? old : (shown(b) ? shown(b) - 1 : 0);
    for (U32 k = 0; select && k < b->n; k++)
        if (StrCmp(b->e[k].name, select) == 0) i = k + (b->up ? 1 : 0);
    browser_select(w, i);
    StrNCpy(b->msg, msg, sizeof(b->msg));
    browser_draw(w);
}

// The typed name as a path: absolute if it starts with '/', else in this directory.
static Bool target(const Browser* b, char* out, U32 max, Bool allow_slash) {
    const char* t = b->input;
    if (!*t || StrCmp(t, ".") == 0 || StrCmp(t, "..") == 0) return FALSE;
    for (const char* p = t; *p; p++) {
        if (*p == '/' && !allow_slash) return FALSE;
        if (p[0] == '.' && p[1] == '.' && (p == t || p[-1] == '/')) return FALSE;
    }
    if (t[0] == '/') StrNCpy(out, t, max);
    else join(out, max, b->path, t);
    return TRUE;
}

// Exactly this path (vfs_exists also looks for the bare name elsewhere).
static Bool exists_exact(const char* path) {
    if (StrNICmp(path, "/ram/", 5) == 0) return ramfs_exists(path + 5);
    return redsea_eml_exists_path(path) || ramfs_exists(path + 1);
}

static const char* last_part(const char* path) {
    const char* n = path;
    for (const char* q = path; *q; q++) if (*q == '/' && q[1]) n = q + 1;
    return n;
}

static U0 do_ask(Window* w) {
    Browser* b = (Browser*)w->data;
    U8 ask = b->ask;
    b->ask = ASK_NONE;
    char src[VFS_MAX_PATH], dst[VFS_MAX_PATH];
    join(src, sizeof(src), b->path, b->subject);
    const char* select = NULL;
    if (ask == ASK_DELETE) {
        Bool ok;
        if (b->subject_dir && vfs_list_dir(src, s_tmp, FILES_MAX) > 0) {
            ksnprintf(b->msg, sizeof(b->msg), "Not deleted: %s is not empty", b->subject);
            reload(w, b->subject);
            return;
        }
        ok = vfs_remove(src);
        ksnprintf(b->msg, sizeof(b->msg), ok ? "%s deleted" : "%s could not be deleted", b->subject);
        reload(w, NULL);
        return;
    }
    if (!target(b, dst, sizeof(dst), ask == ASK_COPY)) {
        ksnprintf(b->msg, sizeof(b->msg), "Not a valid name: %s", b->input);
    } else if (exists_exact(dst)) {
        ksnprintf(b->msg, sizeof(b->msg), "%s already exists", b->input);
    } else if (ask == ASK_MKDIR) {
        Bool ok = vfs_mkdir(dst);
        ksnprintf(b->msg, sizeof(b->msg), ok ? "Folder %s made" : "Folder %s could not be made", b->input);
        select = ok ? last_part(dst) : NULL;
    } else if (b->subject_dir) {
        ksnprintf(b->msg, sizeof(b->msg), "Folders cannot be %s yet", ask == ASK_COPY ? "copied" : "renamed");
    } else if (ask == ASK_COPY) {
        size_t size = 0;
        U8* data = read_listed(src, &size);
        Bool ok = data && vfs_write_file(dst, data, size);
        if (data) Free(data);
        ksnprintf(b->msg, sizeof(b->msg), ok ? "%s copied to %s" : "%s could not be copied to %s", b->subject, b->input);
        select = ok ? last_part(dst) : NULL;
    } else {
        // Rename on the RedSea disk: a second name for the same data, then the
        // old name removed. Cut in between, both names are left, nothing lost.
        Bool ok = StrNICmp(src, "/ram/", 5) != 0 && redsea_eml_link(src + 1, dst + 1, 0, 0) &&
                  redsea_eml_delete_file(src + 1);
        ksnprintf(b->msg, sizeof(b->msg), ok ? "%s renamed to %s" : "%s could not be renamed to %s", b->subject, b->input);
        select = ok ? last_part(dst) : b->subject;
    }
    reload(w, select);
}

// A key while the status line is asking something.
static U0 ask_key(Window* w, U32 key) {
    Browser* b = (Browser*)w->data;
    if (key == WKEY_ESCAPE || (b->ask == ASK_DELETE && key != WKEY_ENTER)) {
        b->ask = ASK_NONE;
        StrNCpy(b->msg, "Cancelled", sizeof(b->msg));
    } else if (key == WKEY_ENTER) {
        do_ask(w);
        return;
    } else if (key == WKEY_BACKSPACE) {
        U32 n = (U32)StrLen(b->input);
        while (n > 0 && ((U8)b->input[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        b->input[n] = '\0';
    } else if (key >= 32 && key < 0x110000 && key != 127) {
        char u[4];
        U32 k = key < 0x80 ? 1 : key < 0x800 ? 2 : 3;
        if (k == 1) u[0] = (char)key;
        else if (k == 2) { u[0] = (char)(0xC0 | (key >> 6)); u[1] = (char)(0x80 | (key & 0x3F)); }
        else { u[0] = (char)(0xE0 | (key >> 12)); u[1] = (char)(0x80 | ((key >> 6) & 0x3F)); u[2] = (char)(0x80 | (key & 0x3F)); }
        U32 n = (U32)StrLen(b->input);
        if (n + k < sizeof(b->input)) {
            MemCpy(b->input + n, u, k);
            b->input[n + k] = '\0';
        }
    }
    browser_draw(w);
}

static U0 ask(Window* w, U8 what) {
    Browser* b = (Browser*)w->data;
    b->input[0] = '\0';
    b->subject[0] = '\0';
    b->subject_dir = FALSE;
    if (what != ASK_MKDIR) {
        if (shown(b) == 0 || (b->up && b->sel == 0)) return;     // nothing selected, or ".."
        const VFSEntry* e = &b->e[b->sel - (b->up ? 1 : 0)];
        StrNCpy(b->subject, e->name, sizeof(b->subject));
        b->subject_dir = e->type == VFS_TYPE_DIR;
        if (what != ASK_DELETE) StrNCpy(b->input, e->name, sizeof(b->input));
    }
    b->ask = what;
    browser_draw(w);
}

static Bool browser_key(Window* w, U32 key) {
    Browser* b = (Browser*)w->data;
    U32 page = w->rows - 1, total = shown(b);
    if (b->ask) {
        ask_key(w, key);
        return TRUE;
    }
    switch (key) {
    case WKEY_DELETE:   ask(w, ASK_DELETE); break;
    case WKEY_F2:       ask(w, ASK_RENAME); break;
    case WKEY_F5:       ask(w, ASK_COPY); break;
    case WKEY_F7:       ask(w, ASK_MKDIR); break;
    case WKEY_UP:       if (b->sel > 0) browser_select(w, b->sel - 1); break;
    case WKEY_DOWN:     browser_select(w, b->sel + 1); break;
    case WKEY_HOME:     browser_select(w, 0); break;
    case WKEY_END:      browser_select(w, total ? total - 1 : 0); break;
    case WKEY_PAGEUP:   browser_select(w, b->sel > page ? b->sel - page : 0); break;
    case WKEY_PAGEDOWN: browser_select(w, b->sel + page); break;
    case WKEY_ENTER:    browser_open(w); break;
    case WKEY_BACKSPACE:
        if (b->up) {
            char p[VFS_MAX_PATH];
            StrNCpy(p, b->path, sizeof(p));
            parent(p);
            browser_load(w, p);
        }
        break;
    default: break;                       // every key is the browser's while it has the focus
    }
    return TRUE;
}

static U0 browser_click(Window* w, I32 x, I32 y, Bool dbl) {
    (void)x;
    Browser* b = (Browser*)w->data;
    U32 row = (U32)y / FONT_HEIGHT;
    if (b->ask) {                             // a press in the list cancels the question
        b->ask = ASK_NONE;
        StrNCpy(b->msg, "Cancelled", sizeof(b->msg));
    }
    if (row >= w->rows - 1 || b->top + row >= shown(b)) { browser_draw(w); return; }
    U32 i = b->top + row;
    if (dbl && i == b->sel) browser_open(w);
    else browser_select(w, i);
}

static U0 free_data(Window* w) {
    Free(w->data);
    w->data = NULL;
}

Window* files_open(const char* path) {
    if (!vfs_is_dir(path) && StrCmp(path, "/") != 0) return NULL;
    Browser* b = (Browser*)Malloc(sizeof(Browser));
    if (!b) return NULL;
    MemSet(b, 0, sizeof(*b));
    Window* w = desktop_new_window(FILES_COLS, FILES_ROWS, "Files");
    if (!w) {
        Free(b);
        return NULL;
    }
    w->data = b;
    w->on_key = browser_key;
    w->on_click = browser_click;
    w->on_close = free_data;
    char canon[VFS_MAX_PATH];
    if (!vfs_resolve_path(path, canon, sizeof(canon))) StrNCpy(canon, "/", sizeof(canon));
    browser_load(w, canon);
    return w;
}

// ---- viewer -----------------------------------------------------------------------------

static U0 viewer_draw(Window* w) {
    Viewer* v = (Viewer*)w->data;
    SurfaceDamageFn saved;
    begin(w, &saved);
    U32 rows = w->rows - 1;
    for (U32 r = 0; r < rows && v->top + r < v->lines; r++) {
        U32 i = v->top + r, at = v->line[i];
        U32 end = i + 1 < v->lines ? v->line[i + 1] : v->size;
        if (end > at && v->data[end - 1] == '\n') end--;
        if (end > at && v->data[end - 1] == '\r') end--;
        U32 col = 0;
        I32 y = (I32)(r * FONT_HEIGHT);
        const char* p = (const char*)v->data + at;
        const char* stop = (const char*)v->data + end;
        while (p < stop && col < w->cols) {
            U32 cp;
            if (*p == '\t') { p++; col = (col / 8 + 1) * 8; continue; }
            if ((U8)*p < 0x80) cp = (U8)*p++;
            else {
                const char* q = p;
                cp = font_utf8_next(&q);
                if (q > stop) { cp = 0xFFFD; q = p + 1; }   // a character cut by the line end
                p = q;
            }
            if (cp < 32 || cp == 127) cp = '?';
            surf_glyph(&w->content, (I32)(col * FONT_WIDTH), y, cp, FILES_TEXT, 0, FALSE);
            col++;
        }
    }
    char st[VFS_MAX_FILENAME + 48];
    U32 last = v->top + rows < v->lines ? v->top + rows : v->lines;
    ksnprintf(st, sizeof(st), "%s  lines %u-%u of %u", v->name, (U64)(v->top + 1), (U64)last, (U64)v->lines);
    status_line(w, st);
    finish(w, saved);
}

static Bool viewer_key(Window* w, U32 key) {
    Viewer* v = (Viewer*)w->data;
    U32 rows = w->rows - 1, max_top = v->lines > rows ? v->lines - rows : 0, top = v->top;
    switch (key) {
    case WKEY_UP:       if (top > 0) top--; break;
    case WKEY_DOWN:     top++; break;
    case WKEY_HOME:     top = 0; break;
    case WKEY_END:      top = max_top; break;
    case WKEY_PAGEUP:   top = top > rows ? top - rows : 0; break;
    case WKEY_PAGEDOWN: top += rows; break;
    default: return TRUE;
    }
    if (top > max_top) top = max_top;
    if (top != v->top) {
        v->top = top;
        viewer_draw(w);
    }
    return TRUE;
}

static U0 viewer_close(Window* w) {
    Viewer* v = (Viewer*)w->data;
    if (!v) return;
    Free(v->data);
    Free(v->line);
    Free(v);
    w->data = NULL;
}

// A file as the browser lists it: on the RedSea disk, or in the RAM file
// system the VFS shows in the same tree (vfs_read_file_exact looks only on
// the disk and in /ram, and is left so for the NAS's network callers).
static U8* read_listed(const char* path, size_t* size) {
    U8* data = vfs_read_file_exact(path, size);
    const char* clean = path;
    while (*clean == '/') clean++;
    if (data || !ramfs_exists(clean)) return data;
    size_t n = 0;
    U8* r = ramfs_read(clean, &n);
    if (!r || !(data = (U8*)Malloc(n + 1))) return NULL;
    MemCpy(data, r, n);
    data[n] = '\0';
    *size = n;
    return data;
}

static Window* viewer_open(const char* path, const char* name, char* why, U32 why_max) {
    size_t size = 0;
    U8* data = read_listed(path, &size);
    if (!data) { ksnprintf(why, why_max, "cannot be read"); return NULL; }
    if (size > FILES_VIEW_MAX) {
        ksnprintf(why, why_max, "too large to view (%u bytes)", (U64)size);
        Free(data);
        return NULL;
    }
    U32 lines = 1;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == 0) {
            ksnprintf(why, why_max, "not text (%u bytes)", (U64)size);
            Free(data);
            return NULL;
        }
        if (data[i] == '\n' && i + 1 < size) lines++;
    }
    Viewer* v = (Viewer*)Malloc(sizeof(Viewer));
    U32* line = (U32*)Malloc(lines * sizeof(U32));
    Window* w = (v && line) ? desktop_new_window(VIEW_COLS, VIEW_ROWS, name) : NULL;
    if (!w) {
        ksnprintf(why, why_max, "no window for it");
        Free(data);
        if (v) Free(v);
        if (line) Free(line);
        return NULL;
    }
    MemSet(v, 0, sizeof(*v));
    StrNCpy(v->name, name, sizeof(v->name));
    v->data = data;
    v->size = (U32)size;
    v->line = line;
    v->lines = lines;
    U32 k = 0;
    line[k++] = 0;
    for (U32 i = 0; i + 1 < v->size; i++) if (data[i] == '\n') line[k++] = i + 1;
    w->data = v;
    w->on_key = viewer_key;
    w->on_close = viewer_close;
    viewer_draw(w);
    return w;
}

// ---- console: `files` ----------------------------------------------------------------------

static Window* browser_on_top(U0) {
    // The most recently focused browser: search the windows for one with our key handler.
    DesktopState* d = desktop_state();
    Window* f = desktop_window(d->focus);
    if (f && f->on_key == browser_key) return f;
    for (U32 id = d->next_id; id-- > 0;) {
        Window* w = desktop_window(id);
        if (w && w->on_key == browser_key) return w;
    }
    return NULL;
}

static Bool files_run(const char* cmd) {
    if (StrCmp(cmd, "files list") == 0) {
        Window* w = browser_on_top();
        if (!w) { kprintf("files: no file browser open\n"); return TRUE; }
        Browser* b = (Browser*)w->data;
        kprintf("Files %u: %s, %u entries, selected %u, status \"%s\"\n", (U64)w->id, b->path, (U64)b->n,
                (U64)b->sel, b->msg);
        if (b->up) kprintf("  ..\n");
        for (U32 i = 0; i < b->n; i++)
            kprintf("  %s%s %u\n", b->e[i].name, b->e[i].type == VFS_TYPE_DIR ? "/" : "", b->e[i].size);
        return TRUE;
    }
    if (StrCmp(cmd, "files") == 0 || shell_is_prefix("files ", cmd)) {
        const char* p = cmd[5] ? cmd + 6 : "/";
        while (*p == ' ') p++;
        if (!desktop_state()->on) { kprintf("files: the desktop is off\n"); return TRUE; }
        Window* w = files_open(*p ? p : "/");
        if (w) kprintf("[FILES] window %u: %s\n", (U64)w->id, ((Browser*)w->data)->path);
        else kprintf("files: cannot open %s (not a directory, or no room for a window)\n", p);
        return TRUE;
    }
    return FALSE;
}

static U0 files_help(U0) {
    kprintf("  Files (a browser window: arrows, Enter or double click to open, Backspace to go up,\n");
    kprintf("         F5 copy, F2 rename, Delete delete, F7 new folder, Esc cancels)\n");
    kprintf("    files [path]   files list\n");
}

static const ShellExtension s_files_shell = { files_run, files_help, NULL, NULL };

U0 files_shell_register(U0) {
    shell_register(&s_files_shell);
}
