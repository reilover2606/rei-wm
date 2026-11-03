#include <errno.h>
#include <event2/event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-protocol.h>

#include "backend/backend.h"
#include "input/input.h"
#include "proto/seat.h"
#include "messages.h"
#include "render/gles.h"
#include "render_thread.h"
#include "util/msgq.h"
#include "server.h"
#include "util/log.h"
#include "util/util.h"
#include "xdg-shell-client-protocol.h"

/*
 * Runs rei-wm as a client of another Wayland compositor: each output is a
 * toplevel window in the parent. This is how the compositor is developed on a
 * machine that is already running a desktop.
 *
 * Input: the parent's keyboard and pointer, as delivered to our windows, are
 * forwarded as rei-wm's own input. Beware that the parent compositor sees key
 * combinations first, so a composition key of "super" is usually swallowed by
 * it; set composition_key = "alt" when developing nested.
 *
 * Presentation: each output's render thread draws with the OpenGL renderer,
 * reads the frame back into a shared-memory file and hands it to the event
 * thread, which attaches it to the parent window as a wl_shm buffer. That keeps
 * every call on the parent connection on one thread. The readback costs
 * performance, which is acceptable for a development backend; the DRM backend
 * will scan the render target out directly.
 */

/* Shown until the first real frame arrives, and needed so the window maps. */
#define NESTED_PLACEHOLDER_COLOR 0xff1b2733u

enum { NMSG_FRAME = 1 };

/* A rendered frame in transit from a render thread to the event thread. */
struct nested_frame {
	uint32_t window_id;
	int fd;  /* memfd holding width*height XRGB8888 pixels */
	int32_t width, height;
};

static void nested_frame_destroy(void *p)
{
	struct nested_frame *f = p;
	close(f->fd);
	free(f);
}

/* Render-thread state for one window; outlives the window object. */
struct nested_present {
	struct nested_backend *nb;
	uint32_t window_id;
};

struct nested_backend;

/* A wl_buffer we have sent but the parent has not released yet. */
struct nested_buffer {
	struct wl_list link;
	struct wl_buffer *buffer;
};

struct nested_window {
	struct wl_list link;
	uint32_t id;
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
	struct wl_seat *parent_seat;
	struct wl_pointer *parent_pointer;
	struct wl_keyboard *parent_keyboard;
	struct nested_window *pointer_window;  /* The window the pointer is over. */
	struct wl_list windows;
	struct rei_msgq inbox;  /* frames from render threads */
	bool inbox_ready;
	uint32_t next_window_id;
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

/* Attaches `fd` (XRGB8888, width x height) to the window and commits. Takes the fd. */
static void window_attach(struct nested_window *w, int fd, int32_t width, int32_t height)
{
	struct nested_backend *nb = w->nb;
	int32_t stride = width * 4;
	size_t size = (size_t)stride * (size_t)height;

	struct wl_shm_pool *pool = wl_shm_create_pool(nb->shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
							  WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	struct nested_buffer *nbuf = rei_xcalloc(1, sizeof *nbuf);
	nbuf->buffer = buf;
	wl_list_insert(&w->buffers, &nbuf->link);
	wl_buffer_add_listener(buf, &buffer_listener, nbuf);
	wl_surface_attach(w->surface, buf, 0, 0);
	wl_surface_damage_buffer(w->surface, 0, 0, width, height);
	wl_surface_commit(w->surface);
}

static void window_draw_placeholder(struct nested_window *w)
{
	size_t size = (size_t)w->width * 4 * (size_t)w->height;
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
		px[i] = NESTED_PLACEHOLDER_COLOR;
	munmap(px, size);
	window_attach(w, fd, w->width, w->height);
}

/* ---- render thread side -------------------------------------------------- */

static void present_frame(void *data, struct rei_gles *gles,
			  const struct rei_scene_snapshot *snap, int32_t width, int32_t height)
{
	(void)snap;
	struct nested_present *np = data;
	if (!gles || width <= 0 || height <= 0)
		return;

	size_t size = (size_t)width * (size_t)height * 4;
	int fd = memfd_create("rei-wm-frame", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		if (fd >= 0)
			close(fd);
		return;
	}
	uint8_t *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		close(fd);
		return;
	}
	bool ok = rei_gles_read_pixels(gles, px);
	if (ok) {
		/* GL gives R,G,B,A; XRGB8888 is B,G,R,X in memory. */
		for (size_t i = 0; i < size; i += 4) {
			uint8_t r = px[i];
			px[i] = px[i + 2];
			px[i + 2] = r;
			px[i + 3] = 0xff;
		}
	}
	munmap(px, size);
	if (!ok) {
		close(fd);
		return;
	}

	struct nested_frame *f = rei_xcalloc(1, sizeof *f);
	f->window_id = np->window_id;
	f->fd = fd;
	f->width = width;
	f->height = height;
	rei_msgq_post(&np->nb->inbox, NMSG_FRAME, f, nested_frame_destroy);
}

static void present_fini(void *data)
{
	free(data);
}

/* ---- event thread side --------------------------------------------------- */

static struct nested_window *window_by_id(struct nested_backend *nb, uint32_t id)
{
	struct nested_window *w;
	wl_list_for_each(w, &nb->windows, link) {
		if (w->id == id)
			return w;
	}
	return NULL;
}

static void on_nested_message(struct rei_msg *m, void *userdata)
{
	struct nested_backend *nb = userdata;
	if (m->type != NMSG_FRAME)
		return;
	struct nested_frame *f = m->data;
	struct nested_window *w = window_by_id(nb, f->window_id);
	if (w && w->configured && !w->close_requested) {
		window_attach(w, f->fd, f->width, f->height);
		wl_display_flush(nb->parent);
	} else {
		close(f->fd);
	}
	free(f);
	m->data = NULL;
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

	/* The first buffer is what maps the window; later ones come from the
	 * renderer. */
	if (!w->configured)
		window_draw_placeholder(w);
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
	if (w->nb->pointer_window == w)
		w->nb->pointer_window = NULL;
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
	w->id = ++nb->next_window_id;
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

	struct nested_present *np = rei_xcalloc(1, sizeof *np);
	np->nb = nb;
	np->window_id = w->id;

	p.width = w->width;
	p.height = w->height;
	p.backend_data = w;
	p.render_ops.present = present_frame;
	p.render_ops.fini = present_fini;
	p.render_ops_data = np;
	w->output = rei_server_add_output(nb->base.server, &p);
	if (!w->output) {
		free(np);
		window_destroy(w);
		return NULL;
	}
	return w;
}

/* ---- input forwarding ---------------------------------------------------- */

static uint32_t now_ms(void)
{
	return (uint32_t)(rei_now_ns() / 1000000ull);
}

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	struct nested_backend *nb = data;
	struct nested_window *w;
	nb->pointer_window = NULL;
	wl_list_for_each(w, &nb->windows, link) {
		if (w->surface == surface)
			nb->pointer_window = w;
	}
	/* rei-wm draws its own cursor; hide the parent's so there aren't two. */
	wl_pointer_set_cursor(p, serial, NULL, 0, 0);

	if (nb->pointer_window && nb->pointer_window->output) {
		const struct rei_output *o = nb->pointer_window->output;
		rei_input_pointer_motion_abs(nb->base.server, now_ms(),
					     o->x + wl_fixed_to_double(sx) / o->scale,
					     o->y + wl_fixed_to_double(sy) / o->scale);
	}
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)p; (void)serial; (void)s;
	((struct nested_backend *)data)->pointer_window = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
			   wl_fixed_t sx, wl_fixed_t sy)
{
	(void)p;
	struct nested_backend *nb = data;
	if (!nb->pointer_window || !nb->pointer_window->output)
		return;
	const struct rei_output *o = nb->pointer_window->output;
	rei_input_pointer_motion_abs(nb->base.server, time,
				     o->x + wl_fixed_to_double(sx) / o->scale,
				     o->y + wl_fixed_to_double(sy) / o->scale);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
			   uint32_t button, uint32_t state)
{
	(void)p; (void)serial;
	rei_input_pointer_button(((struct nested_backend *)data)->base.server, time, button,
				 state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
			 wl_fixed_t value)
{
	(void)p;
	rei_input_pointer_axis(((struct nested_backend *)data)->base.server, time,
			       axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_to_double(value));
}

static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
	(void)p;
	rei_input_pointer_axis(((struct nested_backend *)data)->base.server, time,
			       axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0.0);
}

static void pointer_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{
	(void)d; (void)p; (void)a; (void)v;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
	(void)data; (void)k; (void)format; (void)size;
	close(fd);  /* We have our own keymap; only raw key codes are forwarded. */
}

static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
			   struct wl_array *keys)
{
	(void)d; (void)k; (void)serial; (void)s; (void)keys;
}

static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	(void)k; (void)serial; (void)s;
	/* No more events will come for held keys, so release them ourselves. */
	rei_input_release_all(((struct nested_backend *)data)->base.server, now_ms());
}

static void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
			 uint32_t key, uint32_t state)
{
	(void)k; (void)serial;
	rei_input_key(((struct nested_backend *)data)->base.server, time, key,
		      state == WL_KEYBOARD_KEY_STATE_PRESSED);
}

static void keyboard_modifiers(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a,
			       uint32_t b, uint32_t c, uint32_t g)
{
	(void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g;
}

static void keyboard_repeat_info(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)d; (void)k; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
	struct nested_backend *nb = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !nb->parent_pointer) {
		nb->parent_pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(nb->parent_pointer, &pointer_listener, nb);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && nb->parent_pointer) {
		wl_pointer_release(nb->parent_pointer);
		nb->parent_pointer = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !nb->parent_keyboard) {
		nb->parent_keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(nb->parent_keyboard, &keyboard_listener, nb);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && nb->parent_keyboard) {
		wl_keyboard_release(nb->parent_keyboard);
		nb->parent_keyboard = NULL;
	}

	/* Offer our clients exactly what the parent gives us. */
	rei_seat_set_capabilities(nb->base.server->seat,
				  caps & (WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD));
}

static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

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
	} else if (strcmp(iface, wl_seat_interface.name) == 0 && !nb->parent_seat) {
		nb->parent_seat = wl_registry_bind(reg, name, &wl_seat_interface,
						   version < 5 ? version : 5);
		wl_seat_add_listener(nb->parent_seat, &seat_listener, nb);
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
	wl_display_roundtrip(nb->parent);  /* Second trip: the seat's capabilities. */

	if (!nb->compositor || !nb->shm || !nb->wm_base) {
		rei_error("nested: parent compositor lacks wl_compositor, wl_shm or xdg_wm_base");
		return false;
	}

	if (!rei_msgq_init(&nb->inbox, b->server->base, on_nested_message, nb)) {
		rei_error("nested: cannot create frame queue");
		return false;
	}
	nb->inbox_ready = true;

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

	/* Windows are gone, so every render thread has been joined and nothing can
	 * post here any more. Frames already queued carry fds that must be closed. */
	if (nb->inbox_ready) {
		rei_msgq_close(&nb->inbox);
		rei_msgq_finish(&nb->inbox);
	}

	if (nb->parent_pointer)
		wl_pointer_release(nb->parent_pointer);
	if (nb->parent_keyboard)
		wl_keyboard_release(nb->parent_keyboard);
	if (nb->parent_seat)
		wl_seat_release(nb->parent_seat);
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
