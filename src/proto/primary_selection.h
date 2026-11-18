#ifndef REI_PRIMARY_SELECTION_H
#define REI_PRIMARY_SELECTION_H

#include <stdbool.h>

struct rei_server;
struct wl_client;
struct rei_selection_source;

/*
 * zwp_primary_selection_device_manager_v1: the primary selection, i.e. what is highlighted, pasted with a middle
 * click. Terminals and most toolkits use it; Xwayland bridges it to the X11 PRIMARY selection, so X11 terminals
 * (urxvt, xterm) take part too. Like the clipboard, it can only be taken by the client being typed into, and
 * the client that gets the keyboard focus is told what it currently is.
 */
bool rei_primary_selection_init(struct rei_server *server);
void rei_primary_selection_send(struct rei_server *server, struct wl_client *client);
/* Makes `src` (a compositor-owned source) the primary selection. */
void rei_primary_selection_set(struct rei_server *server, struct rei_selection_source *src);

#endif
