#ifndef APPS_H
#define APPS_H

#include "types.h"
#include "desktop.h"

// Apps in windows: a HolyQ program from the disk runs in a PMP cell (an
// isolated hart) and its 320x240 canvas is shown in a window of its own
// (40 x 15 characters). Keys and presses in that window reach the app
// through input(); closing the window stops the app. See src/apps.c.

#define APPS_TIMEOUT_MS  (10 * 60 * 1000)   // the cell watchdog for an app

U0      apps_init(U0);                    // hooks into the core's cells, `app` command
Window* apps_run(const char* path, char* why, U32 why_max);   // NULL: why says why

#endif // APPS_H
