#ifndef REI_IDLE_H
#define REI_IDLE_H

#include <stdbool.h>

struct rei_server;

/*
 * Display sleep. After [general] display_sleep minutes without input the displays are turned off (the backend's
 * set_display_power; the DRM backend switches the monitors off, others only log it). Input wakes them. A key press,
 * button press or scroll that wakes the display is swallowed (it must not type into, or click on, what the user could not
 * see); pointer motion wakes it and carries on. A program holding an idle inhibitor (zwp_idle_inhibit_manager_v1: video
 * players, browsers playing video) keeps the displays on. The REI_DISPLAY_SLEEP_MS environment variable overrides the
 * timeout, in milliseconds, for the tests.
 */
void rei_idle_init(struct rei_server *server);
void rei_idle_finish(struct rei_server *server);
/* The config was reloaded: the timeout may have changed. */
void rei_idle_configure(struct rei_server *server);

/* The user did something. Returns true if the event must be dropped (it only woke the display; `wake_only`
 * says whether this kind of event is swallowed when it does). */
bool rei_idle_activity(struct rei_server *server, bool wake_only);

/* Idle inhibitors came or went (the protocol). */
void rei_idle_inhibit_add(struct rei_server *server, int delta);
bool rei_idle_asleep(const struct rei_server *server);

#endif
