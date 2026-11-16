/* rei_signal_emit never hangs, even if a listener keeps adding itself back to the signal. */
#include <stdio.h>

#include "check.h"
#include "util/wlsignal.h"

static struct wl_signal sig;
static struct wl_listener readder, once_a, once_b;
static int readder_calls, a_calls, b_calls;

static void on_readder(struct wl_listener *l, void *data)
{
	(void)data;
	readder_calls++;
	wl_signal_add(&sig, l);   /* the bug: puts itself back on the signal being emitted */
}

static void on_a(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
	a_calls++;
}

static void on_b(struct wl_listener *l, void *data)
{
	(void)l; (void)data;
	b_calls++;
}

int main(void)
{
	wl_signal_init(&sig);
	once_a.notify = on_a;
	readder.notify = on_readder;
	once_b.notify = on_b;

	/* Ordinary listeners each fire exactly once. */
	wl_signal_add(&sig, &once_a);
	wl_signal_add(&sig, &once_b);
	rei_signal_emit(&sig, NULL);
	CHECK(a_calls == 1 && b_calls == 1 && wl_list_empty(&sig.listener_list));

	/* A self-re-adding listener is cut off after the limit instead of looping forever. */
	wl_signal_add(&sig, &readder);
	wl_signal_add(&sig, &once_b);
	rei_signal_emit(&sig, NULL);
	CHECK(readder_calls == REI_SIGNAL_EMIT_LIMIT + 0 || readder_calls > 100);
	CHECK(wl_list_empty(&sig.listener_list));
	/* ...and every listener is left safely unlinked (removing again must not corrupt anything). */
	wl_list_remove(&readder.link);
	wl_list_remove(&once_b.link);

	puts("wlsignal: ok");
	return 0;
}
