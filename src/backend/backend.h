#ifndef REI_BACKEND_H
#define REI_BACKEND_H

#include <stdbool.h>

#include "output.h"

struct rei_server;
struct rei_backend;
struct rei_output_config;

/*
 * A backend is where outputs (and, later, input devices) come from: a pretend
 * monitor for headless runs, a window in a parent compositor for the nested
 * backend, and eventually DRM/KMS + libinput.
 *
 * All backend functions run on the event thread.
 */
struct rei_backend_impl {
	const char *name;
	/* Registers fds with the server's event loop and adds outputs. */
	bool (*start)(struct rei_backend *backend);
	/* Removes its outputs and frees the backend. */
	void (*destroy)(struct rei_backend *backend);
	/* Optional. The pointer moved. Returns true if the backend shows the pointer
	 * itself on every output (a hardware cursor), so no redraw is needed. */
	bool (*cursor_moved)(struct rei_backend *backend);
	/* Optional. Switch to virtual terminal `vt` (1-12). */
	void (*switch_vt)(struct rei_backend *backend, unsigned vt);
};

struct rei_backend {
	const struct rei_backend_impl *impl;
	struct rei_server *server;
};

/* "headless", "nested" or "drm"; NULL for an unknown name or if setup fails. */
struct rei_backend *rei_backend_create(const char *name, struct rei_server *server);

struct rei_backend *rei_headless_backend_create(struct rei_server *server);
struct rei_backend *rei_nested_backend_create(struct rei_server *server);
/* Only exists when built with the DRM backend (REI_HAVE_DRM). */
struct rei_backend *rei_drm_backend_create(struct rei_server *server);

/*
 * Fills `p` for the index-th configured output, or the built-in default
 * (1920x1080@60) when the config lists none. `fallback_prefix` names outputs
 * the config left unnamed ("HEADLESS" -> "HEADLESS-1"). `name_buf` backs p->name.
 */
void rei_backend_output_params(struct rei_server *server, size_t index,
			       const char *fallback_prefix,
			       char *name_buf, size_t name_buf_len,
			       struct rei_output_params *p);

/* Number of outputs a backend should create (at least one). */
size_t rei_backend_output_count(struct rei_server *server);

#endif
