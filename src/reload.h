#ifndef REI_RELOAD_H
#define REI_RELOAD_H

#include <stdbool.h>
#include <stddef.h>

struct rei_server;

/*
 * Config hot reload. The config file is watched (inotify on its directory, so
 * editors that save by renaming a new file over it are seen too) and SIGHUP forces a
 * reload. A reload parses the new file completely first; if it is invalid the old
 * settings stay in force and the error is logged.
 *
 * Applied live: keybindings and the composition key, the keyboard layout and key
 * repeat, gaps, the notch, the toolbar, the wallpaper and background colour, window
 * placement defaults, focus-follows-mouse, and pointer-device settings. Changes to
 * [[output]] and to the backend need a restart; the log says so.
 */
void rei_reload_start(struct rei_server *server);
void rei_reload_stop(struct rei_server *server);

/* Reloads now. On failure returns false with the reason in err. */
bool rei_reload_config(struct rei_server *server, char *err, size_t errlen);

#endif
