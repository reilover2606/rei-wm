#ifndef REI_STARTMENU_H
#define REI_STARTMENU_H

#include "geom.h"
#include "toolbar.h"

struct rei_server;
struct rei_output;

/*
 * The Start menu, opened by the Start button: your shortcuts at the top, then "Run" (which opens
 * the floating command prompt), then Logout / Shutdown / Reboot as one unit at the bottom.
 * Logout, Shutdown and Reboot ask for a second click first unless `confirm_power` is off.
 */
void rei_startmenu_open(struct rei_server *server, struct rei_output *output, struct rei_rect bar,
			enum rei_bar_position position, struct rei_rect anchor);
void rei_startmenu_finish(struct rei_server *server);

#endif
