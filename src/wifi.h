#ifndef REI_WIFI_H
#define REI_WIFI_H

#include <stdbool.h>

struct rei_server;

/*
 * Wifi, for the toolbar: whether the machine has a wireless interface, whether it is connected, and how strong
 * the signal is. Read from /sys/class/net (an interface with a `wireless` directory; connected when its
 * operstate is "up", which is when it is associated) and /proc/net/wireless (the link quality, out of 70), every
 * few seconds: these are small local files, so the event loop reads them itself.
 *
 * Clicking the item runs [toolbar] wifi_command (default wpa_gui); a command that cannot be run shows its error as a
 * message. With no wireless interface (a desktop) the item does not appear.
 */
struct rei_wifi_info {
	bool present;      /* a wireless interface exists (and the item is on) */
	bool connected;
	int level;         /* 0..3: arcs to light (3 strong, 1 weak); 0 when not connected */
	char iface[256];   /* the interface the rest describes (the connected one with the best signal) */
};

/* Reads the interfaces under `netdir` (/sys/class/net) and the quality table `procfile` (/proc/net/wireless). */
bool rei_wifi_read(const char *netdir, const char *procfile, struct rei_wifi_info *out);

/* Starts or stops the polling to match [toolbar] wifi. Cheap when nothing changed. */
void rei_wifi_configure(struct rei_server *server);
void rei_wifi_stop(struct rei_server *server);
struct rei_wifi_info rei_wifi_state(const struct rei_server *server);
/* The item was clicked. */
void rei_wifi_click(struct rei_server *server);

#endif
