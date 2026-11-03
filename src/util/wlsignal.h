#ifndef REI_WLSIGNAL_H
#define REI_WLSIGNAL_H

#include <wayland-server-core.h>

/*
 * Emits a wl_signal in a way that tolerates listeners removing themselves *or
 * other listeners* while it runs.
 *
 * wl_signal_emit() caches each node's successor before calling it, so it only
 * survives the current listener removing itself. If a listener's work causes a
 * not-yet-visited listener to unlink (and re-initialise) itself, the cached
 * pointer then refers to a self-linked node and the loop calls it forever. That
 * is easy to hit here: unmapping a window, which a destroy listener does,
 * changes keyboard focus, which unlinks the focus listeners from the very
 * signal being emitted.
 *
 * This takes the first listener off the list before calling it, every time, so
 * the list is never walked while it can change underneath us. Each listener is
 * called at most once per emit. Listeners must not re-add themselves to this
 * signal from inside their callback.
 */
static inline void rei_signal_emit(struct wl_signal *signal, void *data)
{
	while (!wl_list_empty(&signal->listener_list)) {
		struct wl_listener *l = wl_container_of(signal->listener_list.next, l, link);
		wl_list_remove(&l->link);
		wl_list_init(&l->link);  /* So its own wl_list_remove() later is harmless. */
		l->notify(l, data);
	}
}

#endif
