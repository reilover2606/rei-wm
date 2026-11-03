#ifndef REI_WL_OUTPUT_H
#define REI_WL_OUTPUT_H

struct rei_server;
struct rei_output;

/* Advertises an output to clients as a wl_output global. */
void rei_wl_output_add(struct rei_server *server, struct rei_output *output);

/* Re-sends geometry/mode/scale to clients if anything changed. */
void rei_wl_output_update(struct rei_output *output);

/* Withdraws the global. Clients get a grace period before it is destroyed,
 * as the protocol recommends, so they can react to the removal. */
void rei_wl_output_remove(struct rei_output *output);

/* Destroys all wl_output globals immediately (shutdown). */
void rei_wl_output_finish(struct rei_server *server);

#endif
