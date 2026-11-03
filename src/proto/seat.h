#ifndef REI_SEAT_H
#define REI_SEAT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct rei_server;
struct rei_data_source;

/*
 * The one seat ("seat0"). Right now it only has the protocol plumbing:
 * binding, capability negotiation, and the pointer/keyboard/touch objects.
 * It advertises no capabilities and sends no input events until libinput and
 * libxkbcommon arrive in milestone 4.
 */
struct rei_seat {
	struct rei_server *server;
	struct wl_global *global;
	uint32_t capabilities;       /* WL_SEAT_CAPABILITY_* currently offered. */
	uint32_t ever_capabilities;  /* Ever offered (a request is legal if so). */
	struct wl_list resources;    /* Bound wl_seat objects. */
	struct wl_list data_devices; /* wl_data_device objects, all clients. */

	/* The clipboard. Delivery to the focused client arrives with keyboard
	 * focus in milestone 4. */
	struct rei_data_source *selection;
	struct wl_listener selection_destroy;
};

bool rei_seat_init(struct rei_server *server);
void rei_seat_finish(struct rei_server *server);
void rei_seat_set_capabilities(struct rei_seat *seat, uint32_t capabilities);

#endif
