#ifndef REI_FRACTIONAL_SCALE_H
#define REI_FRACTIONAL_SCALE_H

#include <stdbool.h>

struct rei_server;

/*
 * wp_fractional_scale_v1: tells a client the exact scale of the output its window is on (1.4, not
 * the rounded-up 2), in 120ths. Together with wp_viewporter the client draws exactly that many
 * pixels and is shown 1:1, sharp, instead of at 2x and scaled down.
 */
bool rei_fractional_scale_init(struct rei_server *server);
/* Re-sends the preferred scale wherever it has changed (an output's scale, or a window moved). */
void rei_fractional_scale_update_all(struct rei_server *server);

#endif
