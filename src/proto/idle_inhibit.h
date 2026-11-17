#ifndef REI_IDLE_INHIBIT_H
#define REI_IDLE_INHIBIT_H

#include <stdbool.h>

struct rei_server;

/* zwp_idle_inhibit_manager_v1: a client holding an inhibitor keeps the displays from going to sleep (see idle.h). */
bool rei_idle_inhibit_init(struct rei_server *server);

#endif
