#include "server.h"

#include <event2/event.h>
#include <event2/thread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>

#include "backend/backend.h"
#include "messages.h"
#include "render_thread.h"
#include "snapshot.h"
#include "util/log.h"
#include "util/util.h"

/* ------------------------------------------------------------------------ */
/* wl_display <-> libevent bridge                                           */
/* ------------------------------------------------------------------------ */

static void dispatch_wayland(struct rei_server *s)
{
	/* A zero timeout also runs libwayland's idle sources, which otherwise
	 * would never fire because they are not backed by a file descriptor. */
	wl_event_loop_dispatch(s->wl_loop, 0);
	wl_display_flush_clients(s->display);
}

static void on_wayland_readable(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	dispatch_wayland(arg);
}

static void on_flush(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	dispatch_wayland(arg);
}

void rei_server_schedule_flush(struct rei_server *s)
{
	/* event_active on an already-active event is a no-op: free coalescing. */
	event_active(s->flush_event, 0, 0);
}

/* ------------------------------------------------------------------------ */
/* Signals                                                                  */
/* ------------------------------------------------------------------------ */

static void on_terminate(evutil_socket_t sig, short what, void *arg)
{
	(void)what;
	struct rei_server *s = arg;
	rei_info("received %s, shutting down", strsignal((int)sig));
	rei_server_stop(s);
}

/* Developer aid: force a new scene snapshot, exercising the render threads. */
static void on_usr1(evutil_socket_t sig, short what, void *arg)
{
	(void)sig;
	(void)what;
	rei_debug("SIGUSR1: damaging scene");
	rei_server_damage(arg);
}

/* ------------------------------------------------------------------------ */
/* Messages from render threads                                             */
/* ------------------------------------------------------------------------ */

static void on_inbox(struct rei_msg *m, void *userdata)
{
	struct rei_server *s = userdata;

	switch (m->type) {
	case REI_EMSG_FRAME: {
		const struct rei_frame_event *f = m->data;
		struct rei_output *o = rei_server_output_by_id(s, f->output_id);
		if (!o)
			break;  /* Output went away while the frame was in flight. */
		o->frames_presented = f->seq;
		o->last_presented_serial = f->scene_serial;
		rei_debug("frame output=%s seq=%llu serial=%llu", o->name,
			  (unsigned long long)f->seq, (unsigned long long)f->scene_serial);
		/* Later: send wl_surface frame callbacks and presentation
		 * feedback, and release buffers the frame no longer needs. */
		rei_server_schedule_flush(s);
		break;
	}
	case REI_EMSG_RENDER_FAILED: {
		const struct rei_frame_event *f = m->data;
		struct rei_output *o = rei_server_output_by_id(s, f->output_id);
		rei_error("renderer failed on output %s", o ? o->name : "(removed)");
		break;
	}
	}
}

/* ------------------------------------------------------------------------ */
/* Outputs and scene                                                        */
/* ------------------------------------------------------------------------ */

struct rei_output *rei_server_output_by_id(struct rei_server *s, uint32_t id)
{
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->id == id)
			return o;
	}
	return NULL;
}

void rei_server_damage(struct rei_server *s)
{
	struct rei_scene_snapshot *snap = rei_snapshot_create(++s->scene_serial);
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link)
		rei_render_submit(o->render, snap);
	rei_snapshot_unref(snap);
}

struct rei_output *rei_server_add_output(struct rei_server *s,
					 const struct rei_output_params *p)
{
	struct rei_output *o = rei_xcalloc(1, sizeof *o);
	o->id = ++s->next_output_id;
	o->name = rei_xstrdup(p->name);
	o->width = p->width;
	o->height = p->height;
	o->refresh_mhz = p->refresh_mhz;
	o->scale = p->scale > 0 ? p->scale : 1.0;
	o->has_position = p->has_position;
	o->x = p->x;
	o->y = p->y;
	o->backend_data = p->backend_data;

	struct rei_render_params rp = {
		.output_id = o->id,
		.name = o->name,
		.width = o->width,
		.height = o->height,
		.refresh_mhz = o->refresh_mhz,
		.ops = p->render_ops,
		.ops_data = p->render_ops_data,
	};
	o->render = rei_render_create(s, &rp);
	if (!o->render) {
		free(o->name);
		free(o);
		return NULL;
	}

	wl_list_insert(s->outputs.prev, &o->link);
	rei_outputs_layout(&s->outputs);

	rei_info("output added: name=%s id=%u mode=%dx%d@%.3f pos=%d,%d scale=%g",
		 o->name, o->id, o->width, o->height, o->refresh_mhz / 1000.0,
		 o->x, o->y, o->scale);

	/* Placement of other outputs may have shifted; a fresh snapshot gives
	 * every render thread (including the new one) something to draw. */
	rei_server_damage(s);
	return o;
}

void rei_server_remove_output(struct rei_server *s, struct rei_output *o)
{
	rei_info("output removed: name=%s id=%u", o->name, o->id);

	/* Join the render thread first so nothing is in flight referencing it. */
	rei_render_destroy(o->render);
	wl_list_remove(&o->link);
	rei_outputs_layout(&s->outputs);
	free(o->name);
	free(o);
}

void rei_server_set_output_mode(struct rei_server *s, struct rei_output *o,
				int32_t width, int32_t height)
{
	if (o->width == width && o->height == height)
		return;
	o->width = width;
	o->height = height;
	rei_render_resize(o->render, width, height);
	rei_outputs_layout(&s->outputs);
	rei_info("output resized: name=%s mode=%dx%d", o->name, width, height);
	rei_server_damage(s);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */
/* ------------------------------------------------------------------------ */

static const char *resolve_backend(const struct rei_server *s,
				   const struct rei_server_options *opts)
{
	const char *name = (opts && opts->backend_override) ? opts->backend_override
							    : s->config->backend;
	if (strcmp(name, "auto") != 0)
		return name;

	const char *parent = getenv("WAYLAND_DISPLAY");
	if (parent && parent[0])
		return "nested";

	rei_warn("no parent Wayland session and no DRM backend yet: running headless");
	return "headless";
}

struct rei_server *rei_server_create(const struct rei_config *config,
				     const struct rei_server_options *opts)
{
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir || !runtime_dir[0]) {
		rei_error("XDG_RUNTIME_DIR is not set; Wayland sockets live there");
		return NULL;
	}

	struct rei_server *s = rei_xcalloc(1, sizeof *s);
	s->config = config;
	wl_list_init(&s->outputs);

	/* Must precede the creation of any event base that other threads wake. */
	if (evthread_use_pthreads() != 0) {
		rei_error("libevent has no pthread support");
		goto fail;
	}

	s->base = event_base_new();
	if (!s->base) {
		rei_error("cannot create event loop");
		goto fail;
	}

	s->display = wl_display_create();
	if (!s->display) {
		rei_error("cannot create wl_display");
		goto fail;
	}
	s->wl_loop = wl_display_get_event_loop(s->display);

	if (opts && opts->socket_name) {
		if (wl_display_add_socket(s->display, opts->socket_name) != 0) {
			rei_error("cannot listen on %s/%s", runtime_dir, opts->socket_name);
			goto fail;
		}
		s->socket_name = rei_xstrdup(opts->socket_name);
	} else {
		const char *name = wl_display_add_socket_auto(s->display);
		if (!name) {
			rei_error("cannot create a Wayland socket in %s", runtime_dir);
			goto fail;
		}
		s->socket_name = rei_xstrdup(name);
	}

	s->wl_event = event_new(s->base, wl_event_loop_get_fd(s->wl_loop),
				EV_READ | EV_PERSIST, on_wayland_readable, s);
	s->flush_event = event_new(s->base, -1, 0, on_flush, s);
	s->sigint_event = evsignal_new(s->base, SIGINT, on_terminate, s);
	s->sigterm_event = evsignal_new(s->base, SIGTERM, on_terminate, s);
	s->sigusr1_event = evsignal_new(s->base, SIGUSR1, on_usr1, s);
	if (!s->wl_event || !s->flush_event || !s->sigint_event ||
	    !s->sigterm_event || !s->sigusr1_event) {
		rei_error("cannot create event sources");
		goto fail;
	}
	event_add(s->wl_event, NULL);
	event_add(s->sigint_event, NULL);
	event_add(s->sigterm_event, NULL);
	event_add(s->sigusr1_event, NULL);

	if (!rei_msgq_init(&s->inbox, s->base, on_inbox, s)) {
		rei_error("cannot create message queue");
		goto fail;
	}

	signal(SIGPIPE, SIG_IGN);

	const char *backend_name = resolve_backend(s, opts);
	s->backend = rei_backend_create(backend_name, s);
	if (!s->backend)
		goto fail;
	if (!s->backend->impl->start(s->backend)) {
		rei_error("backend '%s' failed to start", backend_name);
		goto fail;
	}

	rei_info("rei-wm listening on WAYLAND_DISPLAY=%s (backend: %s)",
		 s->socket_name, s->backend->impl->name);
	return s;

fail:
	rei_server_destroy(s);
	return NULL;
}

int rei_server_run(struct rei_server *s)
{
	int r = event_base_dispatch(s->base);
	return (r < 0 || s->exit_code) ? 1 : 0;
}

void rei_server_stop(struct rei_server *s)
{
	event_base_loopexit(s->base, NULL);
}

void rei_server_fail(struct rei_server *s)
{
	s->exit_code = 1;
	rei_server_stop(s);
}

void rei_server_destroy(struct rei_server *s)
{
	if (!s)
		return;

	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		rei_info("summary output=%s frames=%llu last_serial=%llu", o->name,
			 (unsigned long long)o->frames_presented,
			 (unsigned long long)o->last_presented_serial);
	}

	/* Backends remove their outputs, which joins the render threads. After
	 * this nothing can post to the inbox any more. */
	if (s->backend)
		s->backend->impl->destroy(s->backend);
	while (!wl_list_empty(&s->outputs)) {
		o = wl_container_of(s->outputs.next, o, link);
		rei_server_remove_output(s, o);
	}

	if (s->display)
		wl_display_destroy_clients(s->display);

	if (s->inbox.wake)
		rei_msgq_finish(&s->inbox);

	struct event **events[] = { &s->wl_event, &s->flush_event, &s->sigint_event,
				    &s->sigterm_event, &s->sigusr1_event };
	for (size_t i = 0; i < REI_ARRAY_LEN(events); i++) {
		if (*events[i]) {
			event_free(*events[i]);
			*events[i] = NULL;
		}
	}

	if (s->display)
		wl_display_destroy(s->display);
	if (s->base)
		event_base_free(s->base);
	free(s->socket_name);
	free(s);
}
