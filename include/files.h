#ifndef FILES_H
#define FILES_H

#include "types.h"
#include "desktop.h"

// The file browser and the text viewer, in desktop windows.
// Browser: one directory of the RedSea disk (through the VFS), ".." first
// below the root, then the entries as the VFS sorts them; a selection bar,
// a status line. Up/Down/Home/End/PageUp/PageDown move, Enter or a double
// click opens, Backspace goes up. A directory is entered, a .wav file is
// played in the background (the desktop keeps working, the status line
// says when it has ended), a .HQ file runs as an app in an isolated cell
// (src/apps.c), any other file is shown in a viewer window if
// it is text (no NUL byte, at most FILES_VIEW_MAX bytes).
// F5 copies a file, F2 renames it (on the RedSea disk: a second name for
// the data, then the old one removed), Delete deletes a file or an empty
// folder after Enter, F7 makes a folder; the name is typed in the status
// line, Esc cancels. Folders are not copied or renamed yet.
// Viewer: the lines cut at the window's width, tabs to 8 columns,
// Up/Down/PageUp/PageDown/Home/End scroll.

#define FILES_COLS        48
#define FILES_ROWS        20
#define FILES_MAX         64              // entries listed per directory
#define VIEW_COLS         72
#define VIEW_ROWS         24
#define FILES_VIEW_MAX    (1024 * 1024)
#define FILES_TEXT        0x202020
#define FILES_SEL_BG      0x2B4C7E
#define FILES_SEL_TEXT    0xFFFFFF
#define FILES_STATUS_BG   0xD8D4C8
#define FILES_STATUS_TEXT 0x404040

Window* files_open(const char* path);     // a browser window; NULL if it cannot be opened
// A file as the browser lists it (RedSea disk or RAM fs), Malloc'd; NULL if none.
U8*     files_read(const char* path, size_t* size);
U0      files_shell_register(U0);

#endif // FILES_H
