#include <errno.h>
#include <event2/event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "backend/backend.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "xdg-shell-client-protocol.h"

/*
 * Runs rei-wm as a client of another Wayland compositor: each output is a
 * toplevel window in the parent. This is how the compositor is developed on a
 * machine that is already running a desktop.
 *
 * Milestone 1 scope: windows open, resize and close, and are filled with a
 * solid colour so you can see them. Real rendering arrives with the EGL
 * renderer, which will attach a wl_egl_window per output on the render thread.
 */

#define NESTED_FILL_COLOR 0xff1b2733u

struct nested_backend;

/* A wl_buffer we have sent but the parent has not released yet. */
struct nested_buffer {
	struct wl_list link;
	struct wl_buffer *buffer;
};

struct nested_window {
	struct wl_list link;
	struct nested_backend *nb;
	struct rei_output *output;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;

	int32_t width, height;                  /* Applied size. */
	int32_t pending_width, pending_height;  /* From the last toplevel.configure. */
	bool configured;
	bool close_requested;

	struct wl_list buffers;  /* nested_buffer, outstanding */
};

struct nested_backend {
	struct rei_backend base;
	struct wl_display *parent;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct event *fd_event;
	struct wl_list windows;
};

/* ---- drawing ----------------------------------------------------------- */

static void nested_buffer_destroy(struct nested_buffer *nb)
{
	wl_buffer_destroy(nb->buffer);
	wl_list_remove(&nb->link);
	free(nb);
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
	(void)buffer;
	nested_buffer_destroy(data);
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void window_draw(struct nested_window *w)
{
	struct nested_backend *nb = w->nb;
	int32_t stride = w->width * 4;
	size_t size = (size_t)stride * (size_t)w->height;

	int fd = memfd_create("rei-wm-nested", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		rei_error("nested: cannot allocate shm buffer: %s", strerror(errno));
		if (fd >= 0)
			close(fd);
		return;
	}
	uint32_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		rei_error("nested: mmap failed: %s", strerror(errno));
		close(fd);
		return;
	}
	for (size_t i = 0; i < size / 4; i++)
		px[i] = NESTED_FILL_COLOR;
	munmap(px, size);

	struct wl_shm_pool *pool = wl_shm_create_pool(nb->shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, w->width, w->height,
							  stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	struct nested_buffer *nbuf = rei_xcalloc(1, sizeof *nbuf);
	nbuf->buffer = buf;
	wl_list_insert(&w->buffers, &nbuf->link);
	wl_buffer_add_listener(buf, &buffer_listener, nbuf);
	wl_surface_attach(w->surface, buf, 0, 0);
	wl_surface_damage_buffer(w->surface, 0, 0, w->width, w->height);
	wl_surface_commit(w->surface);
}

/* ---- xdg-shell listeners ----------------------------------------------- */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t width,
			       int32_t height, struct wl_array *states)
{
	(void)t;
	(void)states;
	struct nested_window *w = data;
	/* 0 means "you choose"; keep what we have. */
	w->pending_width = width;
	w->pending_height = height;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
	(void)t;
	struct nested_window *w = data;
	w->close_requested = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	struct nested_window *w = data;
	xdg_surface_ack_configure(xs, serial);

	bool resized = false;
	if (w->pending_width > 0 && w->pending_height > 0 &&
	    (w->pending_width != w->width || w->pending_height != w->height)) {
		w->width = w->pending_width;
		w->height = w->pending_height;
		resized = true;
	}

	window_draw(w);
	w->configured = true;

	if (resized && w->output)
		rei_server_set_output_mode(w->nb->base.server, w->output, w->width, w->height);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

/* ---- windows ----------------------------------------------------------- */

static void window_destroy(struct nested_window *w)
{
	if (w->output)
		rei_server_remove_output(w->nb->base.server, w->output);
	struct nested_buffer *nbuf, *ntmp;
	wl_list_for_each_safe(nbuf, ntmp, &w->buffers, link)
		nested_buffer_destroy(nbuf);
	if (w->toplevel)
		xdg_toplevel_destroy(w->toplevel);
	if (w->xdg_surface)
		xdg_surface_destroy(w->xdg_surface);
	if (w->surface)
		wl_surface_destroy(w->surface);
	wl_list_remove(&w->link);
	free(w);
}

static struct nested_window *window_create(struct nested_backend *nb, size_t index)
{
	char name[64];
	struct rei_output_params p;
	rei_backend_output_params(nb->base.server, index, "NESTED", name, sizeof name, &p);

	struct nested_window *w = rei_xcalloc(1, sizeof *w);
	w->nb = nb;
	w->width = p.width;
	w->height = p.height;
	wl_list_init(&w->buffers);
	wl_list_insert(nb->windows.prev, &w->link);

	w->surface = wl_compositor_create_surface(nb->compositor);
	w->xdg_surface = xdg_wm_base_get_xdg_surface(nb->wm_base, w->surface);
	xdg_surface_add_listener(w->xdg_surface, &xdg_surface_listener, w);
	w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
	xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);

	char title[96];
	snprintf(title, sizeof title, "rei-wm (%s)", name);
	xdg_toplevel_set_title(w->toplevel, title);
	xdg_toplevel_set_app_id(w->toplevel, "rei-wm");
	wl_surface_commit(w->surface);

	/* Block until the parent sends the first configure, so the output is
	 * created with the size the window actually got. */
	while (!w->configured && !w->close_requested) {
		if (wl_display_dispatch(nb->parent) < 0) {
			rei_error("nested: lost connection to parent compositor");
			window_destroy(w);
			return NULL;
		}
	}
	if (w->close_requested) {
		window_destroy(w);
		return NULL;
	}

	p.width = w->width;
	p.height = w->height;
	p.backend_data = w;
	w->output = rei_server_add_output(nb->base.server, &p);
	if (!w->output) {
		window_destroy(w);
		return NULL;
	}
	return w;
}

/* ---- registry ---------------------------------------------------------- */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
			    const char *iface, uint32_t version)
{
	struct nested_backend *nb = data;
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		nb->compositor = wl_registry_bind(reg, name, &wl_compositor_interface,
						  version < 4 ? version : 4);
	} else if (strcmp(iface, wl_shm_interface.name) == 0) {
		nb->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
		nb->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface,
					       version < 2 ? version : 2);
		xdg_wm_base_add_listener(nb->wm_base, &wm_base_listener, nb);
	}
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	(void)data;
	(void)reg;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* ---- event loop integration -------------------------------------------- */

static void reap_closed_windows(struct nested_backend *nb)
{
	struct nested_window *w, *tmp;
	bool closed_any = false;
	wl_list_for_each_safe(w, tmp, &nb->windows, link) {
		if (w->close_requested) {
			window_destroy(w);
			closed_any = true;
		}
	}
	if (closed_any && wl_list_empty(&nb->windows)) {
		rei_info("last nested window closed");
		rei_server_stop(nb->base.server);
	}
}

static void on_parent_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	struct nested_backend *nb = arg;
	struct wl_display *d = nb->parent;

	while (wl_display_prepare_read(d) != 0)
		wl_display_dispatch_pending(d);
	if (wl_display_read_events(d) < 0) {
		rei_error("nested: parent compositor connection lost");
		rei_server_fail(nb->base.server);
		return;
	}
	wl_display_dispatch_pending(d);
	reap_closed_windows(nb);

	/* A short write is retried on the next wake-up; the traffic volume here
	 * (a few requests per resize) makes full-buffer back-pressure moot. */
	wl_display_flush(d);
}

/* ---- backend ----------------------------------------------------------- */

static bool nested_start(struct rei_backend *b)
{
	struct nested_backend *nb = (struct nested_backend *)b;

	nb->parent = wl_display_connect(NULL);
	if (!nb->parent) {
		rei_error("nested: cannot connect to the parent Wayland compositor "
			  "(is WAYLAND_DISPLAY set?)");
		return false;
	}

	nb->registry = wl_display_get_registry(nb->parent);
	wl_registry_add_listener(nb->registry, &registry_listener, nb);
	wl_display_roundtrip(nb->parent);

	if (!nb->compositor || !nb->shm || !nb->wm_base) {
		rei_error("nested: parent compositor lacks wl_compositor, wl_shm or xdg_wm_base");
		return false;
	}

	size_t n = rei_backend_output_count(b->server);
	for (size_t i = 0; i < n; i++) {
		if (!window_create(nb, i))
			return false;
	}
	wl_display_flush(nb->parent);

	nb->fd_event = event_new(b->server->base, wl_display_get_fd(nb->parent),
				 EV_READ | EV_PERSIST, on_parent_readable, nb);
	event_add(nb->fd_event, NULL);
	return true;
}

static void nested_destroy(struct rei_backend *b)
{
	struct nested_backend *nb = (struct nested_backend *)b;

	if (nb->fd_event)
		event_free(nb->fd_event);

	struct nested_window *w, *tmp;
	wl_list_for_each_safe(w, tmp, &nb->windows, link)
		window_destroy(w);

	if (nb->wm_base)
		xdg_wm_base_destroy(nb->wm_base);
	if (nb->shm)
		wl_shm_destroy(nb->shm);
	if (nb->compositor)
		wl_compositor_destroy(nb->compositor);
	if (nb->registry)
		wl_registry_destroy(nb->registry);
	if (nb->parent)
		wl_display_disconnect(nb->parent);
	free(nb);
}

static const struct rei_backend_impl nested_impl = {
	.name = "nested",
	.start = nested_start,
	.destroy = nested_destroy,
};

struct rei_backend *rei_nested_backend_create(struct rei_server *server)
{
	struct nested_backend *nb = rei_xcalloc(1, sizeof *nb);
	nb->base.impl = &nested_impl;
	nb->base.server = server;
	wl_list_init(&nb->windows);
	return &nb->base;
}
