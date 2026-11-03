#include <stdlib.h>

#include "backend/backend.h"
#include "server.h"
#include "util/util.h"

/*
 * Outputs with no display behind them: frames are "presented" on a simulated
 * vblank and discarded. Used for development without a GPU and for automated
 * tests; it is also the only backend that works in the cloud sandbox.
 */

static bool headless_start(struct rei_backend *b)
{
	size_t n = rei_backend_output_count(b->server);
	for (size_t i = 0; i < n; i++) {
		char name[64];
		struct rei_output_params p;
		rei_backend_output_params(b->server, i, "HEADLESS", name, sizeof name, &p);
		if (!rei_server_add_output(b->server, &p))
			return false;
	}
	return true;
}

static void headless_destroy(struct rei_backend *b)
{
	struct rei_server *s = b->server;
	while (!wl_list_empty(&s->outputs)) {
		struct rei_output *o = wl_container_of(s->outputs.next, o, link);
		rei_server_remove_output(s, o);
	}
	free(b);
}

static const struct rei_backend_impl headless_impl = {
	.name = "headless",
	.start = headless_start,
	.destroy = headless_destroy,
};

struct rei_backend *rei_headless_backend_create(struct rei_server *server)
{
	struct rei_backend *b = rei_xcalloc(1, sizeof *b);
	b->impl = &headless_impl;
	b->server = server;
	return b;
}
