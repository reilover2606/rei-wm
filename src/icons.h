#ifndef REI_ICONS_H
#define REI_ICONS_H

#include "pixbuf.h"

/*
 * Application icons for the toolbar's tray. An application is known by its app id
 * (Wayland) or window class (X11). It is looked up through the desktop entries
 * (the `.desktop` files in `applications/`: by file name, StartupWMClass or the program in Exec=),
 * whose Icon= names a file in an icon theme (PNG; SVG-only themes are not read) or a
 * path. If nothing is found the icon is a coloured square with the first letter.
 */

/* Never NULL. Cached for the life of the process: do not unref. */
struct rei_pixbuf *rei_icon_for_app(const char *app_id, const char *fallback_name);

/* The application's name from its desktop entry, or NULL. Valid until exit. */
const char *rei_icon_app_name(const char *app_id);

/* Frees the caches (for leak checkers, at exit). */
void rei_icons_cleanup(void);

#endif
