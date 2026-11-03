#ifndef REI_ACTIONS_H
#define REI_ACTIONS_H

#include "input/keybindings.h"

struct rei_server;

/* Carries out a keybinding's action. */
void rei_actions_run(struct rei_server *server, const struct rei_action *action);

/* Starts `command` (via /bin/sh -c) detached from the compositor, with
 * WAYLAND_DISPLAY pointing at it. Returns false if it couldn't be started. */
bool rei_spawn(struct rei_server *server, const char *command);

#endif
