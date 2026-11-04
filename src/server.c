#include "server.h"
#include "proto/linux_dmabuf.h"

#include <event2/event.h>
#include <event2/thread.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <wayland-server-core.h>

#include "backend/backend.h"
#include "desktop.h"
#include "input/input.h"
#include "messages.h"
#include "proto/seat.h"
#include "proto/surface.h"
#include "proto/wl_output.h"
#include "proto/xdg_shell.h"
#include "audio.h"
#include "battery.h"
#include "reload.h"
#include "render_thread.h"
#include "scene.h"
#include "snapshot.h"
#include "toolbar.h"
#include "wallpaper.h"
#include "xwayland.h"
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
	if (s->shutting_down)
		return;
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
	rei_server_schedule_damage(arg);
}

static void on_usr2(evutil_socket_t sig, short what, void *arg)
{
	(void)sig;
	(void)what;
	rei_server_screenshot(arg);
}

/* ------------------------------------------------------------------------ */
/* Messages from render threads                                             */
/* ------------------------------------------------------------------------ */

static bool rects_intersect(const struct rei_rect *a, int32_t bx, int32_t by, int32_t bw, int32_t bh)
{
	return a->x < bx + bw && a->x + a->w > bx && a->y < by + bh && a->y + a->h > by;
}

/*
 * A frame containing snapshot `f->scene_serial` reached `out`. Tell every client
 * whose window is (partly) on that output, or that isn't on any output, that
 * it may draw its next frame.
 */
static void fire_frame_callbacks(struct rei_server *s, const struct rei_output *out,
				 const struct rei_frame_event *f)
{
	uint32_t ms = (uint32_t)(f->present_ns / 1000000ull);
	int32_t ow, oh;
	rei_output_logical_size(out, &ow, &oh);

	struct rei_surface *surf;
	wl_list_for_each(surf, &s->surfaces, link) {
		if (wl_list_empty(&surf->frames))
			continue;

		struct rei_rect r;
		bool fire = true;
		if (rei_wm_surface_hidden(rei_surface_root(surf)))
			continue;  /* On a workspace that isn't showing: clients throttle. */
		if (rei_wm_surface_rect(rei_surface_root(surf), &r)) {
			bool here = rects_intersect(&r, out->x, out->y, ow, oh);
			bool anywhere = here;
			const struct rei_output *o;
			wl_list_for_each(o, &s->outputs, link) {
				int32_t w, h;
				rei_output_logical_size(o, &w, &h);
				anywhere = anywhere || rects_intersect(&r, o->x, o->y, w, h);
			}
			fire = here || !anywhere;
		}
		if (fire)
			rei_surface_fire_frames(surf, f->scene_serial, ms);
	}
}

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
		rei_buffer_leases_check(s);
		rei_debug("frame output=%s seq=%llu serial=%llu", o->name,
			  (unsigned long long)f->seq, (unsigned long long)f->scene_serial);
		fire_frame_callbacks(s, o, f);
		rei_server_schedule_flush(s);
		break;
	}
	case REI_EMSG_RENDER_FAILED: {
		const struct rei_frame_event *f = m->data;
		struct rei_output *o = rei_server_output_by_id(s, f->output_id);
		rei_error("renderer failed on output %s", o ? o->name : "(removed)");
		break;
	}
	case REI_EMSG_SCREENSHOT: {
		const struct rei_screenshot_event *e = m->data;
		if (e->ok)
			rei_info("screenshot written: %s", e->path);
		else
			rei_error("screenshot failed: %s", e->path);
		break;
	}
	}
}

/* ------------------------------------------------------------------------ */
/* Outputs and scene                                                        */
/* ------------------------------------------------------------------------ */

void rei_server_schedule_damage(struct rei_server *s)
{
	if (!s || s->shutting_down)
		return;
	s->damage_pending = true;
	event_active(s->damage_event, 0, 0);
}

void rei_server_cursor_moved(struct rei_server *s)
{
	if (s->backend && s->backend->impl->cursor_moved && s->backend->impl->cursor_moved(s->backend))
		return;
	rei_server_schedule_damage(s);
}

void rei_server_flush_damage(struct rei_server *s)
{
	if (!s->damage_pending)
		return;
	s->damage_pending = false;

	uint64_t serial = ++s->scene_serial;
	struct rei_scene_snapshot *snap = rei_scene_build(s, serial);

	if (wl_list_empty(&s->outputs)) {
		/* Nothing will ever present this; don't leave clients waiting. */
		struct rei_surface *surf;
		uint32_t ms = (uint32_t)(rei_now_ns() / 1000000ull);
		wl_list_for_each(surf, &s->surfaces, link)
			rei_surface_fire_frames(surf, serial, ms);
		rei_server_schedule_flush(s);
	}

	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link)
		rei_render_submit(o->render, snap);
	rei_snapshot_unref(snap);
}

static void on_damage(evutil_socket_t fd, short what, void *arg)
{
	(void)fd;
	(void)what;
	rei_server_flush_damage(arg);
}

void rei_server_screenshot(struct rei_server *s)
{
	/* Make sure the render threads have seen the latest state first; their
	 * queues are ordered, so the screenshot request lands after it. */
	rei_server_flush_damage(s);

	const char *dir = getenv("REI_SCREENSHOT_DIR");
	if (!dir || !dir[0])
		dir = getenv("XDG_RUNTIME_DIR");

	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		char path[256];
		snprintf(path, sizeof path, "%s/rei-wm-%s.ppm", dir ? dir : ".", o->name);
		rei_render_screenshot(o->render, path);
	}
}

struct rei_output *rei_server_output_by_id(struct rei_server *s, uint32_t id)
{
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->id == id)
			return o;
	}
	return NULL;
}

static void refresh_wallpaper(struct rei_server *s, struct rei_output *o)
{
	rei_pixbuf_unref(o->wallpaper);
	o->wallpaper = NULL;
	if (!s->wallpaper_image)
		return;
	o->wallpaper = rei_wallpaper_render(s->wallpaper_image,
					    (enum rei_wallpaper_mode)s->wallpaper_mode,
					    s->config->background_rgb, o->width, o->height);
}

void rei_server_reload_look(struct rei_server *s)
{
	const struct rei_config *config = s->config;
	rei_image_free(s->wallpaper_image);
	s->wallpaper_image = NULL;
	s->wallpaper_mode = rei_wallpaper_parse_mode(config->wallpaper_mode);
	if (config->wallpaper_path) {
		char err[256];
		s->wallpaper_image = rei_image_load(config->wallpaper_path, err, sizeof err);
		if (!s->wallpaper_image)
			rei_warn("wallpaper not loaded: %s", err);
	}
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link)
		refresh_wallpaper(s, o);
	rei_server_schedule_damage(s);
}

/*
 * Pushes position/size/scale to anything that cares, but only where it changed.
 * Called after every layout change, since adding or removing one output can
 * move the others.
 */
static void sync_output_geometry(struct rei_server *s)
{
	struct rei_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->sent_x == o->x && o->sent_y == o->y && o->sent_width == o->width &&
		    o->sent_height == o->height && o->sent_scale == o->scale)
			continue;
		o->sent_x = o->x;
		o->sent_y = o->y;
		o->sent_width = o->width;
		o->sent_height = o->height;
		o->sent_scale = o->scale;
		rei_render_set_geometry(o->render, o->width, o->height, o->x, o->y, o->scale);
		rei_wl_output_update(o);
	}
	rei_input_clamp_cursor(s);
	rei_wm_arrange_all(s);  /* Window positions derive from output positions. */
	rei_server_schedule_flush(s);
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
	o->server = s;

	wl_list_insert(s->outputs.prev, &o->link);
	rei_outputs_layout(&s->outputs);

	struct rei_render_params rp = {
		.output_id = o->id,
		.name = o->name,
		.width = o->width,
		.height = o->height,
		.x = o->x,
		.y = o->y,
		.scale = o->scale,
		.refresh_mhz = o->refresh_mhz,
		.ops = p->render_ops,
		.ops_data = p->render_ops_data,
	};
	o->render = rei_render_create(s, &rp);
	if (!o->render) {
		wl_list_remove(&o->link);
		rei_outputs_layout(&s->outputs);
		free(o->name);
		free(o);
		return NULL;
	}
	o->sent_x = o->x;
	o->sent_y = o->y;
	o->sent_width = o->width;
	o->sent_height = o->height;
	o->sent_scale = o->scale;

	refresh_wallpaper(s, o);
	rei_wl_output_add(s, o);

	rei_info("output added: name=%s id=%u mode=%dx%d@%.3f pos=%d,%d scale=%g",
		 o->name, o->id, o->width, o->height, o->refresh_mhz / 1000.0,
		 o->x, o->y, o->scale);

	if (!s->cursor_placed) {
		int32_t w, h;
		rei_output_logical_size(o, &w, &h);
		s->cursor_x = o->x + w / 2;
		s->cursor_y = o->y + h / 2;
		s->cursor_placed = true;
	}

	rei_wm_output_added(s, o);

	/* Other outputs may have shifted; every render thread needs a fresh scene. */
	sync_output_geometry(s);
	rei_server_schedule_damage(s);
	return o;
}

void rei_server_remove_output(struct rei_server *s, struct rei_output *o)
{
	rei_info("output removed: name=%s id=%u", o->name, o->id);

	rei_wm_output_removed(s, o);
	rei_toolbar_output_removed(o);
	rei_wl_output_remove(o);
	/* Join the render thread first so nothing is in flight referencing it. */
	rei_render_destroy(o->render);
	wl_list_remove(&o->link);
	rei_outputs_layout(&s->outputs);
	rei_pixbuf_unref(o->wallpaper);
	free(o->name);
	free(o);

	sync_output_geometry(s);
	rei_server_schedule_damage(s);
}

void rei_server_set_output_mode(struct rei_server *s, struct rei_output *o,
				int32_t width, int32_t height)
{
	if (o->width == width && o->height == height)
		return;
	o->width = width;
	o->height = height;
	rei_outputs_layout(&s->outputs);
	refresh_wallpaper(s, o);
	rei_info("output resized: name=%s mode=%dx%d", o->name, width, height);
	sync_output_geometry(s);
	rei_server_schedule_damage(s);
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

#ifdef REI_HAVE_DRM
	return "drm";
#else
	rei_warn("no parent Wayland session and this build has no DRM backend: running headless");
	return "headless";
#endif
}

/* ---- Client diagnostics: who connected, and what we told them went wrong ---- */

static void on_client_created(struct wl_listener *l, void *data)
{
	(void)l;
	struct wl_client *client = data;
	pid_t pid = 0;
	uid_t uid = 0;
	gid_t gid = 0;
	wl_client_get_credentials(client, &pid, &uid, &gid);

	char path[64], exe[256] = "?";
	snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fgets(exe, sizeof exe, f))
			exe[strcspn(exe, "\n")] = '\0';
		fclose(f);
	}
	rei_info("client connected: pid=%d (%s)", (int)pid, exe);
}

static void protocol_logger(void *user_data, enum wl_protocol_logger_type type,
			    const struct wl_protocol_logger_message *m)
{
	(void)user_data;
	if (type != WL_PROTOCOL_LOGGER_EVENT || m->resource == NULL)
		return;
	if (strcmp(wl_resource_get_class(m->resource), "wl_display") != 0 ||
	    strcmp(m->message->name, "error") != 0 || m->arguments_count < 3)
		return;
	struct wl_client *client = wl_resource_get_client(m->resource);
	pid_t pid = 0;
	uid_t uid;
	gid_t gid;
	wl_client_get_credentials(client, &pid, &uid, &gid);
	rei_warn("protocol error sent to client pid=%d: code %u: %s", (int)pid,
		  m->arguments[1].u, m->arguments[2].s ? m->arguments[2].s : "");
}

static struct wl_listener client_created_listener = { .notify = on_client_created };

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
	wl_list_init(&s->surfaces);
	wl_list_init(&s->views);
	rei_wm_init(s);
	wl_list_init(&s->popups);
	wl_list_init(&s->wl_outputs);
	wl_list_init(&s->buffer_leases);

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
	if (s->display) {
		wl_display_add_client_created_listener(s->display, &client_created_listener);
		s->protocol_logger = wl_display_add_protocol_logger(s->display, protocol_logger, s);
	}
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

	if (wl_display_init_shm(s->display) != 0 || !rei_surface_init(s) ||
	    !rei_xdg_shell_init(s) || !rei_seat_init(s)) {
		rei_error("cannot register Wayland globals");
		goto fail;
	}
	char input_err[256];
	if (!rei_input_init(s, input_err, sizeof input_err)) {
		rei_error("input: %s", input_err);
		goto fail;
	}

	s->wl_event = event_new(s->base, wl_event_loop_get_fd(s->wl_loop),
				EV_READ | EV_PERSIST, on_wayland_readable, s);
	s->flush_event = event_new(s->base, -1, 0, on_flush, s);
	s->sigint_event = evsignal_new(s->base, SIGINT, on_terminate, s);
	s->sigterm_event = evsignal_new(s->base, SIGTERM, on_terminate, s);
	s->sigusr1_event = evsignal_new(s->base, SIGUSR1, on_usr1, s);
	s->sigusr2_event = evsignal_new(s->base, SIGUSR2, on_usr2, s);
	s->damage_event = event_new(s->base, -1, 0, on_damage, s);
	if (!s->damage_event || !s->sigusr2_event || !s->wl_event || !s->flush_event || !s->sigint_event ||
	    !s->sigterm_event || !s->sigusr1_event) {
		rei_error("cannot create event sources");
		goto fail;
	}
	event_add(s->wl_event, NULL);
	event_add(s->sigint_event, NULL);
	event_add(s->sigterm_event, NULL);
	event_add(s->sigusr1_event, NULL);
	event_add(s->sigusr2_event, NULL);

	if (!rei_msgq_init(&s->inbox, s->base, on_inbox, s)) {
		rei_error("cannot create message queue");
		goto fail;
	}

	signal(SIGPIPE, SIG_IGN);
	rei_toolbar_init(s);

	s->wallpaper_mode = rei_wallpaper_parse_mode(config->wallpaper_mode);
	if (config->wallpaper_path) {
		char err[256];
		s->wallpaper_image = rei_image_load(config->wallpaper_path, err, sizeof err);
		if (s->wallpaper_image)
			rei_info("wallpaper: %s (%dx%d, %s)", config->wallpaper_path,
				 s->wallpaper_image->width, s->wallpaper_image->height,
				 config->wallpaper_mode);
		else
			rei_warn("wallpaper not loaded: %s", err);
	}

	const char *backend_name = resolve_backend(s, opts);
	s->backend = rei_backend_create(backend_name, s);
	if (!s->backend)
		goto fail;
	if (!s->backend->impl->start(s->backend)) {
		rei_error("backend '%s' failed to start", backend_name);
		goto fail;
	}

	rei_xwayland_start(s);
	rei_reload_start(s);
	rei_audio_start(s);
	rei_battery_start(s);

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
	s->shutting_down = true;

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

	/* Destroying clients tears down every surface and window; do it while the
	 * event sources they poke at still exist. */
	if (s->display)
		wl_display_destroy_clients(s->display);
	if (s->display)
		rei_wl_output_finish(s);
	rei_battery_stop(s);
	rei_audio_stop(s);
	rei_reload_stop(s);
	rei_xwayland_stop(s);
	rei_linux_dmabuf_finish(s);
	if (s->protocol_logger)
		wl_protocol_logger_destroy(s->protocol_logger);
	s->protocol_logger = NULL;

	if (s->inbox.wake)
		rei_msgq_finish(&s->inbox);

	struct event **events[] = { &s->wl_event, &s->flush_event, &s->sigint_event,
				    &s->sigterm_event, &s->sigusr1_event, &s->sigusr2_event,
				    &s->damage_event };
	for (size_t i = 0; i < REI_ARRAY_LEN(events); i++) {
		if (*events[i]) {
			event_free(*events[i]);
			*events[i] = NULL;
		}
	}

	if (s->display)
		wl_display_destroy(s->display);
	rei_input_finish(s);
	rei_toolbar_finish(s);
	rei_wm_finish(s);
	rei_seat_finish(s);
	rei_image_free(s->wallpaper_image);
	if (s->base)
		event_base_free(s->base);
	free(s->socket_name);
	free(s);
}
