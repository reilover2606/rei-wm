#ifndef REI_SESSIONBUS_H
#define REI_SESSIONBUS_H

struct rei_server;

/*
 * Optionally gives the programs rei-wm launches a D-Bus session bus of their own
 * ([general] session_bus = "private"). Many GTK/GNOME programs are single-instance through the
 * session bus: started from rei-wm while another desktop of the same user is running, they find
 * that desktop's copy on the shared bus and open their window there instead. A private bus makes
 * them start (and show up) here. Trade-off: services of the other desktop (portals, notifications,
 * keyring) are not on it.
 */
void rei_sessionbus_start(struct rei_server *server);
void rei_sessionbus_stop(struct rei_server *server);
/* The private bus's address ("unix:path=..."), or NULL when not in use. */
const char *rei_sessionbus_address(const struct rei_server *server);

#endif
