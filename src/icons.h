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

/*
 * "Flavor icons": every image in a directory the user names, used as random icons for the
 * iconable elements (tray tiles, the Start button, Start-menu rows). Only PNGs that are square
 * and at most REI_ICON_MAX_SIZE pixels a side are accepted; anything else is logged and skipped.
 * Transparency is kept. Pass NULL or "" to turn the feature off. Returns how many were accepted.
 */
#define REI_ICON_MAX_SIZE 64
int rei_flavor_icons_configure(const char *dir);
int rei_flavor_icons_count(void);
/* A random icon for `key`: stable for a key during the run (it does not change on redraw), but
 * different keys get different ones. NULL when flavor icons are off. Do not unref. */
struct rei_pixbuf *rei_flavor_icon(const char *key);

/* Frees the caches (for leak checkers, at exit). */
void rei_icons_cleanup(void);

#endif
