#ifndef REI_XDG_DECORATION_H
#define REI_XDG_DECORATION_H

#include <stdbool.h>

struct rei_server;

/*
 * zxdg_decoration_manager_v1: lets a window and the compositor agree on who draws the window's title bar. rei-wm
 * draws no title bars of its own (only the small notches over the focused window), so with
 * [decoration] client_titlebars = false it answers "server side": programs that honour it (Qt, SDL, GTK4, many
 * others) leave their title bar out. With true (the default) it answers "client side": they draw their own, as they
 * do for a compositor without this protocol. Programs that never ask (GTK3's headerbars, Firefox's own tab strip)
 * decide for themselves.
 */
bool rei_xdg_decoration_init(struct rei_server *server);
/* The config was reloaded: tell every window that asked about the mode it has now. */
void rei_xdg_decoration_apply(struct rei_server *server);

#endif
