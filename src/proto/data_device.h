#ifndef REI_DATA_DEVICE_H
#define REI_DATA_DEVICE_H

#include <stdbool.h>

struct rei_server;

/* wl_data_device_manager: clipboard sources, offers and devices. Drag and drop
 * is accepted but cancelled until pointer input exists. */
bool rei_data_device_init(struct rei_server *server);

struct wl_client;

/*
 * Tells `client`'s data devices about the current clipboard. The keyboard-focus
 * code (milestone 4) calls this when focus moves to a client; until then nothing
 * does, so clipboard contents are not yet delivered.
 */
void rei_data_device_send_selection(struct rei_server *server, struct wl_client *client);

#endif
